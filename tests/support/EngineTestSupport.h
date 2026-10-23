#pragma once

// Small helpers shared by the WS-4 tests: a recording ISideChannel, a log sink that can block, and a
// way to run a scenario in a child process (for the primitives that end the process).

#include "core/IBridgeEngine.h"
#include "core/LogSinks.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace asb::test {

// Keeps every log line ("I text", "W text", "C text", "D text"); never blocks.
class RecordingSideChannel final : public ISideChannel {
public:
    QStringList lines;
    QList<StatusReport> published;

    void log(QtMsgType t, const QString& text) override
    {
        const char* p = t == QtWarningMsg ? "W " : t == QtCriticalMsg ? "C " : t == QtDebugMsg ? "D " : "I ";
        lines << QLatin1String(p) + text;
    }
    void publishStatus(const StatusReport& r) override { published << r; }
    bool flush(std::chrono::milliseconds) override { return true; }

    int count(const QString& needle) const
    {
        int n = 0;
        for (const QString& l : lines)
            if (l.contains(needle)) ++n;
        return n;
    }
};

// A sink whose write() blocks until release() (or `maxBlock` passes), recording what it got and on
// which threads.
class BlockingSink final : public LogSink {
public:
    explicit BlockingSink(std::chrono::milliseconds maxBlock = std::chrono::seconds(10)) : m_maxBlock(maxBlock) {}

    void write(QtMsgType, const QDateTime&, const QString& text) override
    {
        std::unique_lock lock(m_mutex);
        threads.push_back(std::this_thread::get_id());
        ++entered;
        m_cv.notify_all();
        m_cv.wait_for(lock, m_maxBlock, [&] { return m_released; });
        received << text;
    }
    void release()
    {
        std::lock_guard lock(m_mutex);
        m_released = true;
        m_cv.notify_all();
    }
    bool waitEntered(int n, std::chrono::milliseconds budget)
    {
        std::unique_lock lock(m_mutex);
        return m_cv.wait_for(lock, budget, [&] { return entered >= n; });
    }
    QStringList receivedLines()
    {
        std::lock_guard lock(m_mutex);
        return received;
    }
    std::vector<std::thread::id> threadIds()
    {
        std::lock_guard lock(m_mutex);
        return threads;
    }

    int entered = 0;   // guarded by m_mutex (read through waitEntered)

private:
    std::chrono::milliseconds m_maxBlock;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_released = false;
    QStringList received;
    std::vector<std::thread::id> threads;
};

// Runs this test executable again with ASB_PROBE=<scenario>; its main() runs the scenario instead of
// the tests.
struct ProbeResult {
    bool finished = false;
    int exitCode = -1;
    QProcess::ExitStatus status = QProcess::CrashExit;
    qint64 ms = 0;
    QByteArray out;
};

inline ProbeResult runProbe(const QString& scenario, int timeoutMs = 15000)
{
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("ASB_PROBE"), scenario);
    p.setProcessEnvironment(env);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    ProbeResult r;
    QElapsedTimer t;
    p.start(QCoreApplication::applicationFilePath(), {});
    if (!p.waitForStarted(5000)) return r;
    t.start();
    r.finished = p.waitForFinished(timeoutMs);
    r.ms = t.elapsed();
    if (!r.finished) {
        p.kill();
        p.waitForFinished(5000);
    }
    r.exitCode = p.exitCode();
    r.status = p.exitStatus();
    r.out = p.readAllStandardOutput();
    return r;
}

}  // namespace asb::test
