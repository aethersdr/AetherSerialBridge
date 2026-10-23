#include "core/SideChannel.h"

#include <QDateTime>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace asb {

namespace {

struct Entry {
    QtMsgType type = QtInfoMsg;
    QDateTime when;
    QByteArray text;   // UTF-8, ≤ kMaxLineBytes
};

}  // namespace

struct SideChannel::State {
    std::unique_ptr<LogSink> sink;
    StatusWriter statusWriter;
    Options options;

    std::mutex mutex;
    std::condition_variable wake;   // worker: work arrived / stop / flush
    std::condition_variable done;   // flushers and shutdown: progress

    // The ring, preallocated: a push moves one entry in, never allocates under the lock.
    std::array<Entry, kQueueCapacity> ring;
    int head = 0;    // next to write out
    int count = 0;
    quint64 pushed = 0, written = 0;
    // Drops: counted at once; reported by one notice once every line queued before the first
    // unreported drop (`dropMark` lines in all) has been written.
    quint64 dropped = 0, droppedReported = 0, droppedNoticed = 0, dropMark = 0;

    std::optional<StatusReport> status;   // the latest-wins slot
    quint64 statusPublished = 0, statusWritten = 0;
    quint64 statusFlushTarget = 0;   // a flush waits for this one: written past the rate limit
    std::chrono::steady_clock::time_point nextStatusAt{};   // rate limit
    bool statusFailing = false;

    bool accepting = true;
    bool stopping = false;
    bool finished = false;

    void run();
};

QByteArray sideChannelLine(const QString& text)
{
    QString flat = text;
    flat.replace(QLatin1Char('\r'), QLatin1Char(' ')).replace(QLatin1Char('\n'), QLatin1Char(' '));
    QByteArray utf8 = flat.toUtf8();
    if (utf8.size() > SideChannel::kMaxLineBytes) {
        qsizetype cut = SideChannel::kMaxLineBytes;
        while (cut > 0 && (quint8(utf8.at(cut)) & 0xC0) == 0x80) --cut;   // never split a character
        utf8.truncate(cut);
    }
    return utf8;
}

void SideChannel::State::run()
{
    std::unique_lock lock(mutex);
    for (;;) {
        // 1. A due status first. It is rate-limited (once per interval, or once per flush target), so
        //    it cannot starve the log, and checking it before every line means a log backlog cannot
        //    starve it either.
        const bool statusDue = status
            && (std::chrono::steady_clock::now() >= nextStatusAt || statusWritten < statusFlushTarget || stopping);
        if (statusDue) {
            StatusReport r = std::move(*status);
            status.reset();
            const quint64 seq = statusPublished;
            lock.unlock();
            QString err;
            const bool ok = !statusWriter || statusWriter(r, &err);
            // A failing status write never stops anything (INV-8); it is logged once per change.
            if (ok == statusFailing) {
                statusFailing = !ok;
                sink->write(ok ? QtInfoMsg : QtWarningMsg, QDateTime::currentDateTimeUtc(),
                            ok ? QStringLiteral("status file written again")
                               : QStringLiteral("status file not written: %1").arg(err));
            }
            lock.lock();
            statusWritten = seq;
            nextStatusAt = std::chrono::steady_clock::now() + options.statusInterval;
            done.notify_all();
            continue;
        }
        // 2. The dropped-lines notice, at its place in the stream.
        if (dropped > droppedReported && written >= dropMark) {
            const quint64 n = dropped - droppedReported;
            droppedReported = dropped;
            lock.unlock();
            sink->write(QtWarningMsg, QDateTime::currentDateTimeUtc(),
                        QStringLiteral("side channel: %1 log line(s) dropped (queue full)").arg(n));
            lock.lock();
            droppedNoticed = droppedReported;
            done.notify_all();
            continue;
        }
        // 3. One log line, then round again.
        if (count > 0) {
            Entry e = std::move(ring[size_t(head)]);
            head = (head + 1) % kQueueCapacity;
            --count;
            lock.unlock();
            sink->write(e.type, e.when, QString::fromUtf8(e.text));
            lock.lock();
            ++written;
            done.notify_all();
            continue;
        }
        if (stopping) break;
        if (status) {
            wake.wait_until(lock, nextStatusAt);
        } else {
            wake.wait(lock);
        }
    }
    finished = true;
    done.notify_all();
}

StatusWriter statusFileWriter(std::shared_ptr<FileIo> io, std::shared_ptr<const DirHandle> dir)
{
    // By value: the writer lives in the worker's state, which can outlive the caller (abandoned worker).
    return [io = std::move(io), dir = std::move(dir)](const StatusReport& r, QString* err) {
        const WriteResult w = StatusStore::write(*io, *dir, r);
        if (!w.replaced && err) *err = w.error;
        return w.replaced;
    };
}

SideChannel::SideChannel(std::unique_ptr<LogSink> sink, StatusWriter status)
    : SideChannel(std::move(sink), std::move(status), Options{})
{
}

SideChannel::SideChannel(std::unique_ptr<LogSink> sink, StatusWriter status, Options options)
    : m_state(std::make_shared<State>())
{
    m_state->sink = std::move(sink);
    m_state->statusWriter = std::move(status);
    m_state->options = options;
    // The worker owns a reference to the state, so an abandoned worker never touches freed memory.
    std::thread([s = m_state] { s->run(); }).detach();
}

SideChannel::~SideChannel() { shutdown(kShutdownBudget); }

void SideChannel::log(QtMsgType type, const QString& text)
{
    Entry e{type, QDateTime::currentDateTimeUtc(), sideChannelLine(text)};   // built outside the lock
    State& s = *m_state;
    {
        const std::lock_guard lock(s.mutex);
        if (!s.accepting) return;
        if (s.count == kQueueCapacity) {
            // The first drop since the last notice: the notice goes after the lines queued so far.
            if (s.dropped == s.droppedReported) s.dropMark = s.pushed;
            ++s.dropped;
            return;
        }
        s.ring[size_t((s.head + s.count) % kQueueCapacity)] = std::move(e);
        ++s.count;
        ++s.pushed;
    }
    s.wake.notify_one();
}

void SideChannel::publishStatus(const StatusReport& report)
{
    State& s = *m_state;
    {
        const std::lock_guard lock(s.mutex);
        if (!s.accepting) return;
        s.status = report;   // implicitly shared copy; latest wins
        ++s.statusPublished;
    }
    s.wake.notify_one();
}

bool SideChannel::flush(std::chrono::milliseconds budget)
{
    State& s = *m_state;
    std::unique_lock lock(s.mutex);
    const quint64 logTarget = s.pushed;
    const quint64 statusTarget = s.statusPublished;
    const quint64 droppedTarget = s.dropped;
    const auto reached = [&] {
        return s.written >= logTarget && s.statusWritten >= statusTarget && s.droppedNoticed >= droppedTarget;
    };
    s.statusFlushTarget = std::max(s.statusFlushTarget, statusTarget);   // past the rate limit, once
    s.wake.notify_one();
    s.done.wait_for(lock, budget, [&] { return s.finished || reached(); });
    return reached();
}

bool SideChannel::shutdown(std::chrono::milliseconds budget)
{
    State& s = *m_state;
    std::unique_lock lock(s.mutex);
    s.accepting = false;
    s.stopping = true;
    s.wake.notify_one();
    return s.done.wait_for(lock, budget, [&] { return s.finished; });
}

quint64 SideChannel::droppedLines() const
{
    const std::lock_guard lock(m_state->mutex);
    return m_state->dropped;
}

}  // namespace asb
