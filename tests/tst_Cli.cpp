// tst_Cli::exitsAfterFinal (INV-118, WS-5's deferred gate, run by WS-9): the CLI runs its operation on an
// OperationHost worker; when a phase deadline passes while the worker is inside a native call, it prints ONE interim
// line on stderr and keeps waiting; it returns (the process would exit) only after the final result was delivered,
// with that result's exit code — never at the interim, never while the worker still holds its locks.

#include "cli/Commands.h"
#include "support/FakeCliHost.h"

#include <QTest>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>

using namespace asb;
using namespace asb::test;

namespace {

struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool open = false;
    void enter()
    {
        std::unique_lock lk(m);
        cv.wait(lk, [&] { return open; });
    }
    void release()
    {
        std::lock_guard g(m);
        open = true;
        cv.notify_all();
    }
};

std::atomic<bool> g_inTest{false};
void exitDuringTest()
{
    if (!g_inTest.load()) return;
    std::fprintf(stdout, "FAIL!  : tst_Cli::exitsAfterFinal() the process exited during the operation\n");
    std::fflush(stdout);
    std::_Exit(1);
}

}  // namespace

class tst_Cli : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { std::atexit(exitDuringTest); }

    void exitsAfterFinal()
    {
        g_inTest = true;
        Rig r;
        r.installedRunning(configA());
        FakeCliHost h(r);
        CapturingConsole c;
        Gate gate;
        std::atomic<bool> workerReturned{false};
        std::atomic<bool> releasedByInterim{false};
        // The firewall sync takes 61 s of fake time (its phase deadline is 60 s), then blocks until the CLI has
        // printed its interim line.
        r.firewall.onCall = [&](const QString& m) {
            if (m != QLatin1String("sync")) return;
            r.world.clock.advance(std::chrono::seconds(61));
            gate.enter();
            workerReturned = true;
        };
        c.onErr = [&](const QString& line) {
            if (line.contains(QLatin1String("still processing"))) {
                releasedByInterim = true;
                gate.release();
            }
        };
        struct OpenOnExit {
            Gate& g;
            ~OpenOnExit() { g.release(); }
        } openOnExit{gate};

        const int code = cli::runCommand({"--apply"}, h, c);
        g_inTest = false;

        QVERIFY2(releasedByInterim.load(), qPrintable(c.events.join('\n')));
        QVERIFY(workerReturned.load());   // the worker returned (and released its locks) before runCommand did
        QVERIFY(!r.world.applyLock.held());
        QCOMPARE(h.results.size(), 1);   // the final result was delivered exactly once
        const Result& final = h.results[0];
        QCOMPARE(final.outcome, Outcome::StateUnknown);
        QCOMPARE(code, 1);               // the final result's code
        // Exactly one interim line, before the final message.
        int interims = 0, interimAt = -1, finalAt = -1;
        for (int i = 0; i < c.events.size(); ++i) {
            if (c.events[i].contains(QLatin1String("still processing"))) {
                ++interims;
                interimAt = i;
            }
            if (c.events[i].contains(final.message)) finalAt = i;
        }
        QCOMPARE(interims, 1);
        QVERIFY2(interimAt >= 0 && finalAt > interimAt, qPrintable(c.events.join('\n')));
    }
};

QTEST_GUILESS_MAIN(tst_Cli)
#include "tst_Cli.moc"
