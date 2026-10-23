// The side-channel worker (WS-4, master §4.7, S5): INV-68 (bounded queue, the engine thread never
// blocks), INV-83 (status and log I/O only on the worker), latest-wins status, bounded flush/shutdown.

#include "core/SideChannel.h"
#include "support/EngineTestSupport.h"
#include "support/FakeFileIo.h"
#include "support/SanitizerFailLine.h"

#include <QElapsedTimer>
#include <algorithm>
#include <QTest>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

using namespace asb;
using namespace asb::test;
using namespace std::chrono_literals;

namespace {

// Records every sink call and its thread; never blocks.
class RecordingSink final : public LogSink {
public:
    void write(QtMsgType, const QDateTime&, const QString& text) override
    {
        std::lock_guard lock(mutex);
        lines << text;
        threads.insert(std::this_thread::get_id());
    }
    std::mutex mutex;
    QStringList lines;
    std::set<std::thread::id> threads;
};

// A sink that takes `delay` per line.
class SlowSink final : public LogSink {
public:
    explicit SlowSink(std::chrono::milliseconds d) : delay(d) {}
    void write(QtMsgType, const QDateTime&, const QString&) override
    {
        std::this_thread::sleep_for(delay);
        ++count;
    }
    std::chrono::milliseconds delay;
    std::atomic<int> count{0};
};

// The FileIo seam with every call's thread recorded (INV-83), forwarding to FakeFileIo.
class ThreadRecordingFileIo final : public FileIo {
public:
    std::unique_ptr<TempFile> createTemp(const DirHandle& d, OwnerPolicy p, const NameGen& g, QString* e) override
    {
        record();
        return inner.createTemp(d, p, g, e);
    }
    bool remove(const DirHandle& d, const QString& n, QString* e) override
    {
        record();
        return inner.remove(d, n, e);
    }
    bool syncDir(const DirHandle& d, QString* e) override
    {
        record();
        return inner.syncDir(d, e);
    }
    std::unique_ptr<ExistingFile> openExisting(const DirHandle& d, const QString& n, QString* e) override
    {
        record();
        return inner.openExisting(d, n, e);
    }
    void record()
    {
        std::lock_guard lock(mutex);
        threads.insert(std::this_thread::get_id());
        ++calls;
    }
    FakeFileIo inner;
    std::mutex mutex;
    std::set<std::thread::id> threads;
    int calls = 0;
};

// A sink that spends `cost` per line busy-waiting: the same rate on every OS (sleep_for(1ms) is ~15 ms
// on Windows).
class SpinSink final : public LogSink {
public:
    explicit SpinSink(std::chrono::microseconds c) : cost(c) {}
    void write(QtMsgType, const QDateTime&, const QString& text) override
    {
        const auto until = std::chrono::steady_clock::now() + cost;
        while (std::chrono::steady_clock::now() < until) {
        }
        if (text.startsWith(QStringLiteral("side channel: "))) ++notices;
        ++count;
    }
    std::chrono::microseconds cost;
    std::atomic<int> count{0}, notices{0};
};

// A gate a sink blocks on, shared with the test so the test can open it after the channel (and the
// sink it owns) may already be gone.
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool open = false;
    int entered = 0;
    QStringList received;

    bool waitEntered(int n, std::chrono::milliseconds budget)
    {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, budget, [&] { return entered >= n; });
    }
    void release()
    {
        std::lock_guard lock(mutex);
        open = true;
        cv.notify_all();
    }
};

class GatedSink final : public LogSink {
public:
    explicit GatedSink(std::shared_ptr<Gate> g) : gate(std::move(g)) {}
    void write(QtMsgType, const QDateTime&, const QString& text) override
    {
        std::unique_lock lock(gate->mutex);
        ++gate->entered;
        gate->cv.notify_all();
        gate->cv.wait(lock, [&] { return gate->open; });
        gate->received << text;
    }
    std::shared_ptr<Gate> gate;
};

// FakeFileIo that counts createTemp calls into a counter the test keeps.
class CountingFileIo final : public FakeFileIo {
public:
    explicit CountingFileIo(std::shared_ptr<std::atomic<int>> c) : calls(std::move(c)) {}
    std::unique_ptr<TempFile> createTemp(const DirHandle& d, OwnerPolicy p, const NameGen& g, QString* e) override
    {
        ++*calls;
        return FakeFileIo::createTemp(d, p, g, e);
    }
    std::shared_ptr<std::atomic<int>> calls;
};

StatusReport report(int pid)
{
    StatusReport r;
    r.pid = pid;
    r.updated = QDateTime::currentDateTimeUtc();
    r.startedAt = r.updated;
    r.instanceId = QByteArray(32, 'a');
    return r;
}

}  // namespace

class tst_SideChannel : public QObject {
    Q_OBJECT

private slots:
    void linesReachTheSinkInOrder()
    {
        auto* sink = new RecordingSink;
        SideChannel side(std::unique_ptr<LogSink>(sink), {});
        for (int i = 0; i < 50; ++i) side.log(QtInfoMsg, QStringLiteral("line %1").arg(i));
        QVERIFY(side.flush(5s));
        std::lock_guard lock(sink->mutex);
        QCOMPARE(sink->lines.size(), 50);
        QCOMPARE(sink->lines.first(), QStringLiteral("line 0"));
        QCOMPARE(sink->lines.last(), QStringLiteral("line 49"));
    }

    // INV-68: a full ring drops and counts; nothing blocks.
    void fullQueueDropsAndCounts()
    {
        auto* sink = new BlockingSink(10s);
        SideChannel side(std::unique_ptr<LogSink>(sink), {});
        side.log(QtInfoMsg, QStringLiteral("first"));
        QVERIFY(sink->waitEntered(1, 5s));   // the worker holds "first" inside the blocked sink
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < SideChannel::kQueueCapacity + 10; ++i) side.log(QtInfoMsg, QStringLiteral("l%1").arg(i));
        QVERIFY2(t.elapsed() < 1000, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(side.droppedLines(), quint64(10));
        sink->release();
        QVERIFY(side.flush(10s));
        const QStringList got = sink->receivedLines();
        QCOMPARE(got.size(), 1 + SideChannel::kQueueCapacity + 1);   // + the "dropped" notice
        QCOMPARE(got[SideChannel::kQueueCapacity], QStringLiteral("l%1").arg(SideChannel::kQueueCapacity - 1));
        QCOMPARE(got.last(), QStringLiteral("side channel: 10 log line(s) dropped (queue full)"));
    }

    // INV-68 / INV-6: a slow sink and a blocked status write cost the caller nothing.
    void engineNotBlockedBySlowSink()
    {
        auto* sink = new SlowSink(100ms);
        std::atomic<bool> releaseStatus{false};
        SideChannel side(std::unique_ptr<LogSink>(sink), [&](const StatusReport&, QString*) {
            while (!releaseStatus) std::this_thread::sleep_for(10ms);
            return true;
        });
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < 20; ++i) side.log(QtWarningMsg, QStringLiteral("slow %1").arg(i));
        for (int i = 0; i < 50; ++i) side.publishStatus(report(i));
        QVERIFY2(t.elapsed() < 150, qPrintable(QString::number(t.elapsed())));
        releaseStatus = true;
        QVERIFY(side.shutdown(30s));
    }

    // INV-83: every status and log I/O call happens on the worker, never on the calling thread.
    void engineThreadDoesNoIo()
    {
        auto* sink = new RecordingSink;
        auto ioOwner = std::make_shared<ThreadRecordingFileIo>();
        ThreadRecordingFileIo& io = *ioOwner;
        SideChannel side(std::unique_ptr<LogSink>(sink), statusFileWriter(ioOwner, std::make_shared<FakeDir>()));
        side.log(QtInfoMsg, QStringLiteral("hello"));
        side.publishStatus(report(42));
        QVERIFY(side.flush(5s));
        const auto engine = std::this_thread::get_id();
        {
            std::lock_guard lock(io.mutex);
            QVERIFY(io.calls > 0);
            QVERIFY(io.threads.count(engine) == 0);
        }
        {
            std::lock_guard lock(sink->mutex);
            QCOMPARE(sink->lines, QStringList{QStringLiteral("hello")});
            QVERIFY(sink->threads.count(engine) == 0);
        }
        QVERIFY(io.inner.exists(QStringLiteral("status.json")));
        const auto back = StatusStore::fromJson(io.inner.contents(QStringLiteral("status.json")));
        QVERIFY(back.has_value());
        QCOMPARE(back->pid, qint64(42));
        QVERIFY(std::any_of(io.inner.log.cbegin(), io.inner.log.cend(),
                               [](const QString& l) { return l.startsWith(QStringLiteral("createTemp")) && l.endsWith(QStringLiteral("Runtime")); }));
    }

    void statusIsLatestWinsAndRateLimited()
    {
        auto* sink = new RecordingSink;
        std::mutex m;
        QList<qint64> written;
        SideChannel::Options o;
        o.statusInterval = 400ms;
        SideChannel side(std::unique_ptr<LogSink>(sink),
                         [&](const StatusReport& r, QString*) {
                             std::lock_guard lock(m);
                             written << r.pid;
                             return true;
                         },
                         o);
        side.publishStatus(report(1));
        QTRY_VERIFY_WITH_TIMEOUT([&] { std::lock_guard lock(m); return written.size() == 1; }(), 2000);
        for (int i = 2; i <= 20; ++i) side.publishStatus(report(i));
        QTest::qWait(150);
        {
            std::lock_guard lock(m);
            QCOMPARE(written.size(), 1);   // within the interval: nothing more yet
        }
        QTRY_VERIFY_WITH_TIMEOUT([&] { std::lock_guard lock(m); return written.size() == 2; }(), 2000);
        {
            std::lock_guard lock(m);
            QCOMPARE(written.last(), qint64(20));   // the latest only
        }
        // flush writes the newest at once, past the rate limit.
        side.publishStatus(report(99));
        QVERIFY(side.flush(1s));
        std::lock_guard lock(m);
        QCOMPARE(written.last(), qint64(99));
    }

    void failingStatusWriteNeverStopsLogging()
    {
        auto* sink = new RecordingSink;
        SideChannel side(std::unique_ptr<LogSink>(sink), [](const StatusReport&, QString* err) {
            *err = QStringLiteral("EACCES");
            return false;
        });
        side.publishStatus(report(1));
        side.log(QtInfoMsg, QStringLiteral("after"));
        QVERIFY(side.flush(5s));
        std::lock_guard lock(sink->mutex);
        QVERIFY(sink->lines.contains(QStringLiteral("after")));
        QVERIFY(sink->lines.contains(QStringLiteral("status file not written: EACCES")));
    }

    void flushIsBoundedOnABlockedSink()
    {
        auto* sink = new BlockingSink(10s);
        SideChannel side(std::unique_ptr<LogSink>(sink), {});
        side.log(QtInfoMsg, QStringLiteral("stuck"));
        QElapsedTimer t;
        t.start();
        QVERIFY(!side.flush(200ms));
        QVERIFY2(t.elapsed() < 1000, qPrintable(QString::number(t.elapsed())));
        sink->release();
        QVERIFY(side.flush(5s));
    }

    // Shutdown: stop accepting, wait at most the budget, report whether the worker finished.
    void shutdownIsBounded()
    {
        auto* sink = new BlockingSink(10s);
        auto side = std::make_unique<SideChannel>(std::unique_ptr<LogSink>(sink), StatusWriter{});
        side->log(QtInfoMsg, QStringLiteral("stuck"));
        QVERIFY(sink->waitEntered(1, 5s));
        QElapsedTimer t;
        t.start();
        QVERIFY(!side->shutdown(300ms));
        QVERIFY2(t.elapsed() < 1500, qPrintable(QString::number(t.elapsed())));
        side->log(QtInfoMsg, QStringLiteral("ignored after shutdown"));
        sink->release();
        QVERIFY(side->shutdown(5s));
        QCOMPARE(sink->receivedLines(), QStringList{QStringLiteral("stuck")});
        QCOMPARE(SideChannel::kShutdownBudget, 2000ms);
    }

    // An abandoned worker (the destructor's shutdown budget ran out while the sink was blocked, a
    // status still pending) must not touch the status writer's FileIo / DirHandle after the caller has
    // let go of them. Under ASan (the sanitizers leg) a writer holding references is a
    // heap-use-after-free here; without a sanitizer the test still checks that the worker finished,
    // wrote the pending status and released what it held.
    void abandonedWorkerKeepsItsStatusDependencies()
    {
        auto gate = std::make_shared<Gate>();
        auto calls = std::make_shared<std::atomic<int>>(0);
        // Separate allocations (not make_shared), so the last owner going frees the object itself.
        std::shared_ptr<CountingFileIo> io(new CountingFileIo(calls));
        std::shared_ptr<FakeDir> dir(new FakeDir);
        const std::weak_ptr<CountingFileIo> ioWatch = io;
        const std::weak_ptr<FakeDir> dirWatch = dir;
        auto side = std::make_unique<SideChannel>(std::make_unique<GatedSink>(gate), statusFileWriter(io, dir));
        side->log(QtInfoMsg, QStringLiteral("stuck"));
        QVERIFY(gate->waitEntered(1, 5s));   // the worker holds "stuck" inside the blocked sink
        side->publishStatus(report(7));
        QElapsedTimer t;
        t.start();
        side.reset();   // shutdown(kShutdownBudget) runs out: the worker is abandoned
        QVERIFY2(t.elapsed() >= 1900, qPrintable(QString::number(t.elapsed())));
        io.reset();     // the caller's own references go, as the documented lifetime allows
        dir.reset();
        QCOMPARE(calls->load(), 0);
        gate->release();   // the worker resumes: writes "stuck", then the pending status
        // The status write happened (through the dependencies the writer kept) and the worker then
        // finished and released them. A writer holding references dies here under ASan instead.
        QTRY_VERIFY_WITH_TIMEOUT(calls->load() >= 1 && ioWatch.expired() && dirWatch.expired(), 5000);
        std::lock_guard lock(gate->mutex);
        QCOMPARE(gate->received, QStringList{QStringLiteral("stuck")});
    }

    // A sustained log backlog (the ring never empties, lines are dropped) must not starve the status
    // slot: status is still written at the 1 s cadence, and a flush returns once its targets are
    // written (its lines, its status, and the notice for the lines dropped before it).
    void statusKeepsItsCadenceUnderALogFlood()
    {
        auto* sink = new SpinSink(500us);   // ~2000 lines/s: a full ring takes ~0.5 s to write
        std::mutex m;
        QList<std::chrono::steady_clock::time_point> writes;
        SideChannel side(std::unique_ptr<LogSink>(sink), [&](const StatusReport&, QString*) {
            std::lock_guard lock(m);
            writes << std::chrono::steady_clock::now();
            return true;
        });   // the default 1 s interval
        // The producer keeps the ring full whatever the runner's speed: it logs until a line is
        // dropped (the ring is full at that moment), then yields about 1 ms, in which the sink frees a
        // few slots. Every cycle ends on an overflow. (A fixed rate, 64 lines then a 1 ms sleep, fell
        // below the sink's rate on a slow ARM runner and never filled the ring.)
        std::atomic<bool> stop{false};
        std::atomic<int> overflows{0};
        std::thread producer([&] {
            for (int i = 0; !stop;) {
                const quint64 before = side.droppedLines();
                while (!stop && side.droppedLines() == before) side.log(QtInfoMsg, QStringLiteral("flood %1").arg(i++));
                if (!stop) ++overflows;
                std::this_thread::sleep_for(1ms);
            }
        });
        QElapsedTimer filling;   // the window starts with the ring full
        filling.start();
        while (overflows.load() == 0 && filling.elapsed() < 5000) std::this_thread::sleep_for(1ms);
        if (overflows.load() == 0) {
            stop = true;
            producer.join();
            QFAIL("the producer never filled the ring");
        }
        const auto start = std::chrono::steady_clock::now();
        bool flushed = false;
        qint64 flushMs = -1;
        for (int i = 0; std::chrono::steady_clock::now() - start < 4s; ++i) {
            side.publishStatus(report(i));
            if (!flushed && std::chrono::steady_clock::now() - start >= 2s) {
                QElapsedTimer f;
                f.start();
                flushed = side.flush(5s);
                flushMs = f.elapsed();
                if (!flushed) break;
            }
            std::this_thread::sleep_for(20ms);
        }
        stop = true;
        producer.join();
        QVERIFY2(flushed, "flush timed out during the log flood");
        QVERIFY2(flushMs < 3000, qPrintable(QString::number(flushMs)));
        QVERIFY(side.droppedLines() > 0);   // the ring overflowed, and kept overflowing:
        QVERIFY2(overflows.load() > 1, qPrintable(QString::number(overflows.load())));
        {
            std::lock_guard lock(m);
            QVERIFY2(writes.size() >= 3, qPrintable(QString::number(writes.size())));
            for (int i = 1; i < writes.size(); ++i) {
                const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(writes[i] - writes[i - 1]);
                QVERIFY2(gap < 1600ms, qPrintable(QStringLiteral("gap %1 ms before write %2").arg(gap.count()).arg(i)));
            }
            const auto lastGap = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - writes.last());
            QVERIFY2(lastGap < 2000ms, qPrintable(QString::number(lastGap.count())));
        }
        QVERIFY(side.flush(10s));
        QVERIFY(sink->notices.load() >= 1);
    }

    void linesAreFlattenedAndCutOnACharacterBoundary()
    {
        QCOMPARE(sideChannelLine(QStringLiteral("a\nb\r\nc")), QByteArray("a b  c"));
        const QString euro = QString(QChar(0x20AC));   // 3 UTF-8 bytes
        const QByteArray cut = sideChannelLine(euro.repeated(400));
        QVERIFY(cut.size() <= SideChannel::kMaxLineBytes);
        QCOMPARE(cut.size(), 510);   // 170 whole characters
        QCOMPARE(QString::fromUtf8(cut), euro.repeated(170));
    }
};

QTEST_GUILESS_MAIN(tst_SideChannel)
#include "tst_SideChannel.moc"
