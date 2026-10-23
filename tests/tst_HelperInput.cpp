// The helper's bounded read (master §5.7 input, S10, D34; INV-89) over real pipes: EOF is the only success; the cap
// is cumulative and exact (cap bytes accepted, cap + 1 refused) and nothing past cap + 1 is ever read; one deadline
// covers the whole request, so a trickle of bytes cannot extend it; the descriptor's flags are put back.

#include "cli/HelperInput.h"

#include <QElapsedTimer>
#include <QTest>

#include <atomic>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

using namespace asb::cli::helper;
using namespace std::chrono_literals;

namespace {

struct Pipe {
    int r = -1, w = -1;
    Pipe()
    {
        int fds[2];
        if (::pipe(fds) != 0) qFatal("pipe");
        r = fds[0];
        w = fds[1];
    }
    ~Pipe()
    {
        closeWrite();
        if (r >= 0) ::close(r);
    }
    void closeWrite()
    {
        if (w >= 0) ::close(w);
        w = -1;
    }
    void put(const QByteArray& b) const
    {
        qsizetype done = 0;
        while (done < b.size()) {
            const ssize_t n = ::write(w, b.constData() + done, size_t(b.size() - done));
            if (n <= 0) return;
            done += n;
        }
    }
};

// Bytes still waiting in the pipe (read without blocking).
qsizetype leftIn(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    qsizetype n = 0;
    char buf[4096];
    for (ssize_t got; (got = ::read(fd, buf, sizeof buf)) > 0;) n += got;
    ::fcntl(fd, F_SETFL, flags);
    return n;
}

}  // namespace

class tst_HelperInput : public QObject {
    Q_OBJECT

private slots:
    void eofEndsTheRequest()
    {
        Pipe p;
        p.put("{\"op\":\"inspect\"}\n");
        p.closeWrite();
        const ReadResult r = readBounded(p.r, 64, 2s);
        QCOMPARE(r.status, ReadResult::Status::Ok);
        QCOMPARE(r.bytes, QByteArray("{\"op\":\"inspect\"}\n"));
    }
    void emptyInputIsAnEmptyRequest()
    {
        Pipe p;
        p.closeWrite();
        const ReadResult r = readBounded(p.r, 64, 2s);
        QCOMPARE(r.status, ReadResult::Status::Ok);
        QVERIFY(r.bytes.isEmpty());
    }
    // A writer feeding many small chunks: the cap counts all of them (cumulative), exactly.
    void capIsCumulativeAndExact_data()
    {
        QTest::addColumn<int>("size");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("cap - 1") << 999 << true;
        QTest::newRow("cap") << 1000 << true;
        QTest::newRow("cap + 1") << 1001 << false;
        QTest::newRow("much more") << 5000 << false;
    }
    void capIsCumulativeAndExact()
    {
        QFETCH(int, size);
        QFETCH(bool, accepted);
        Pipe p;
        std::thread writer([&] {
            for (int i = 0; i < size; i += 7) p.put(QByteArray(qMin(7, size - i), ' '));
            if (accepted) p.closeWrite();   // the over-cap rows keep it open: the cap alone must end the read
        });
        const ReadResult r = readBounded(p.r, 1000, 5s);
        writer.join();
        QCOMPARE(r.status, accepted ? ReadResult::Status::Ok : ReadResult::Status::TooLarge);
        if (accepted) QCOMPARE(r.bytes.size(), qsizetype(size));
        else QCOMPARE(leftIn(p.r), qsizetype(size - 1001));   // not one byte read past cap + 1
    }
    // The over-cap refusal is immediate: it does not wait for EOF or the deadline.
    void overCapEndsAtOnce()
    {
        Pipe p;
        p.put(QByteArray(1001, 'x'));
        QElapsedTimer t;
        t.start();
        const ReadResult r = readBounded(p.r, 1000, 10s);
        QCOMPARE(r.status, ReadResult::Status::TooLarge);
        QVERIFY2(t.elapsed() < 1000, qPrintable(QString::number(t.elapsed())));
    }
    void noEofIsRefusedAtTheDeadline()
    {
        Pipe p;
        p.put("{\"op\":");
        QElapsedTimer t;
        t.start();
        const ReadResult r = readBounded(p.r, 1000, 300ms);
        QCOMPARE(r.status, ReadResult::Status::Deadline);
        QVERIFY2(t.elapsed() >= 290 && t.elapsed() < 2000, qPrintable(QString::number(t.elapsed())));
    }
    // One deadline for the whole request: a byte every 50 ms keeps arriving, yet the read ends at 300 ms.
    void trickleDoesNotExtendTheDeadline()
    {
        Pipe p;
        std::atomic<bool> stop{false};
        std::thread writer([&] {
            while (!stop) {
                p.put(" ");
                std::this_thread::sleep_for(50ms);
            }
        });
        QElapsedTimer t;
        t.start();
        const ReadResult r = readBounded(p.r, 100000, 300ms);
        const qint64 took = t.elapsed();
        stop = true;
        writer.join();
        QCOMPARE(r.status, ReadResult::Status::Deadline);
        QVERIFY2(took >= 290 && took < 2000, qPrintable(QString::number(took)));
    }
    void flagsAreRestored()
    {
        Pipe p;
        p.closeWrite();
        const int before = ::fcntl(p.r, F_GETFL);
        QVERIFY(!(before & O_NONBLOCK));
        QCOMPARE(readBounded(p.r, 10, 1s).status, ReadResult::Status::Ok);
        QCOMPARE(::fcntl(p.r, F_GETFL), before);
    }
    void closedDescriptorIsAnError()
    {
        const ReadResult r = readBounded(1000, 10, 1s);
        QCOMPARE(r.status, ReadResult::Status::Error);
        QVERIFY(r.error.contains("not open"));
    }
};

QTEST_GUILESS_MAIN(tst_HelperInput)
#include "tst_HelperInput.moc"
