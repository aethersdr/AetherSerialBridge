// INV-60 (WS-7): the service status table, the allowed transitions, and no Running after a stop request.
// INV-93 / D59 (WS-7): the service host arms or tightens the kill deadline before any report that can block,
// and every exit goes through the exit primitive. Portable: runs on every OS against a recording reporter.

#include "platform/win/ServiceExit.h"
#include "platform/win/ServiceLifecycle.h"
#include "platform/win/ServiceStatus.h"

#include <QStringList>
#include <QTest>

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace asb;
using namespace asb::win;

namespace {

// Every call, in order, from every fake: 'a' arm, 't' tighten, 'e' extend, 'x' expireNow, 'R' a report
// (the row is kept), 'X' the exit primitive, 'S' the runner's stop, 'F' the side-channel shutdown, 'W' the
// not-activated line said (the text is kept).
struct Order {
    std::mutex m;
    std::string calls;
    std::vector<NativeServiceStatus> rows;
    QStringList said;
    void add(char c)
    {
        std::lock_guard g(m);
        calls += c;
    }
};
Order* g_order = nullptr;

class Recorder final : public IServiceStatusReporter {
public:
    explicit Recorder(Order& o) : m_o(o) {}
    bool report(const NativeServiceStatus& s) override
    {
        std::lock_guard g(m_o.m);
        m_o.calls += 'R';
        m_o.rows.push_back(s);
        return true;
    }

private:
    Order& m_o;
};

class Clock final : public IClock {
public:
    Deadline now() const override { return Deadline{} + std::chrono::hours(1); }
    void sleepFor(std::chrono::milliseconds) override {}
};

class Watchdog final : public IDeadlineWatchdog {
public:
    explicit Watchdog(Order& o) : m_o(o) {}
    void arm(std::chrono::steady_clock::time_point d, int c) override
    {
        m_o.add('a');
        armed = d;
        code = c;
    }
    void extend(std::chrono::steady_clock::time_point) override { m_o.add('e'); }
    void tighten(std::chrono::steady_clock::time_point d, int c) override
    {
        m_o.add('t');
        tightened = d;
        code = c;
    }
    void expireNow() override { m_o.add('x'); }
    std::optional<std::chrono::steady_clock::time_point> armed, tightened;
    int code = -1;

private:
    Order& m_o;
};

// The exit primitive as Exit.h specifies it (tighten to now + 2 s, then the report, then terminate), minus the
// termination; records what it was called with.
int g_exitCode = -100;
void (*g_exitReport)(int) = nullptr;
int g_exits = 0;
void recordingExit(IDeadlineWatchdog& kill, int code, void (*report)(int))
{
    g_order->add('X');
    ++g_exits;
    g_exitCode = code;
    g_exitReport = report;
    kill.tighten(std::chrono::steady_clock::now() + Exit::kReportBudget, code);
    if (report) report(code);
}

struct Rig {
    Order order;
    Recorder reporter{order};
    ServiceStatus status{reporter};
    Watchdog kill{order};
    Clock clock;
    std::vector<std::function<void()>> mainQueue;
    std::function<void()> stopDone;
    std::function<void()> onSay;   // runs inside the say hook
    ServiceLifecycle life;

    Rig()
        : life(status, kill, clock,
               ServiceLifecycle::Hooks{
                   [this](std::function<void()> f) { mainQueue.push_back(std::move(f)); },
                   [this](std::function<void()> done) {
                       order.add('S');
                       stopDone = std::move(done);
                   },
                   [this](std::chrono::milliseconds) {
                       order.add('F');
                       return true;
                   },
                   {},
                   [this](const QString& text) {
                       order.add('W');
                       order.said << text;
                       if (onSay) onSay();
                   }},
               &recordingExit)
    {
        g_order = &order;
        g_exits = 0;
        g_exitCode = -100;
        g_exitReport = nullptr;
        ServiceExit::setFinalReporter(&reporter);
    }
    ~Rig() { ServiceExit::setFinalReporter(nullptr); }
    void runMain()
    {
        auto q = std::move(mainQueue);
        mainQueue.clear();
        for (auto& f : q) f();
    }
};

}  // namespace

class tst_ServiceStatus : public QObject {
    Q_OBJECT

private slots:
    // ---- the table: every report sets every native field of its row ----
    void tableRows_data()
    {
        QTest::addColumn<int>("which");
        QTest::addColumn<quint32>("state");
        QTest::addColumn<quint32>("controls");
        QTest::addColumn<quint32>("win32");
        QTest::addColumn<quint32>("specific");
        QTest::addColumn<quint32>("checkPoint");
        QTest::addColumn<quint32>("waitHint");
        const quint32 accept = svc::kAcceptStop | svc::kAcceptShutdown | svc::kAcceptPreshutdown;
        QTest::newRow("StartPending#1") << 0 << quint32(2) << 0u << 0u << 0u << 1u << 10000u;
        QTest::newRow("StartPending#7") << 1 << quint32(2) << 0u << 0u << 0u << 7u << 10000u;
        QTest::newRow("Running") << 2 << quint32(4) << accept << 0u << 0u << 0u << 0u;
        QTest::newRow("StopPending#1") << 3 << quint32(3) << 0u << 0u << 0u << 1u << 25000u;
        QTest::newRow("Stopped(normal)") << 4 << quint32(1) << 0u << 0u << 0u << 0u << 0u;
        QTest::newRow("Stopped(untrusted)") << 5 << quint32(1) << 0u << 1066u << 3u << 0u << 0u;
        QTest::newRow("Stopped(lock)") << 6 << quint32(1) << 0u << 1066u << 4u << 0u << 0u;
        // v1.1 station fix: not activated is a normal stop for SCM (NO_ERROR), so no recovery action runs.
        QTest::newRow("Stopped(not activated)") << 7 << quint32(1) << 0u << 0u << 0u << 0u << 0u;
    }
    void tableRows()
    {
        QFETCH(int, which);
        QFETCH(quint32, state);
        QFETCH(quint32, controls);
        QFETCH(quint32, win32);
        QFETCH(quint32, specific);
        QFETCH(quint32, checkPoint);
        QFETCH(quint32, waitHint);
        NativeServiceStatus s;
        switch (which) {
        case 0: s = startPendingRow(1); break;
        case 1: s = startPendingRow(7); break;
        case 2: s = runningRow(); break;
        case 3: s = stopPendingRow(1); break;
        case 4: s = stoppedRow(0); break;
        case 5: s = stoppedRow(3); break;
        case 6: s = stoppedRow(4); break;
        default: s = stoppedRow(6); break;
        }
        QCOMPARE(s.serviceType, svc::kOwnProcess);
        QCOMPARE(s.currentState, state);
        QCOMPARE(s.controlsAccepted, controls);
        QCOMPARE(s.win32ExitCode, win32);
        QCOMPARE(s.serviceSpecificExitCode, specific);
        QCOMPARE(s.checkPoint, checkPoint);
        QCOMPARE(s.waitHint, waitHint);
    }

    void transitionTable()
    {
        using S = ServiceState;
        QVERIFY(transitionAllowed(S::StartPending, S::StartPending));
        QVERIFY(transitionAllowed(S::StartPending, S::Running));
        QVERIFY(transitionAllowed(S::StartPending, S::StopPending));
        QVERIFY(transitionAllowed(S::StartPending, S::Stopped));
        QVERIFY(transitionAllowed(S::Running, S::StopPending));
        QVERIFY(!transitionAllowed(S::Running, S::StartPending));
        QVERIFY(!transitionAllowed(S::Running, S::Running));
        QVERIFY(!transitionAllowed(S::Running, S::Stopped));   // only through StopPending
        QVERIFY(transitionAllowed(S::StopPending, S::StopPending));
        QVERIFY(transitionAllowed(S::StopPending, S::Stopped));
        QVERIFY(!transitionAllowed(S::StopPending, S::Running));
        QVERIFY(!transitionAllowed(S::StopPending, S::StartPending));
        for (S to : {S::StartPending, S::Running, S::StopPending, S::Stopped}) QVERIFY(!transitionAllowed(S::Stopped, to));
    }

    void orderlyLifecycleReportsTheTable()
    {
        Order o;
        Recorder r(o);
        ServiceStatus s(r);
        QVERIFY(s.startPending());
        QVERIFY(s.startPending());
        QVERIFY(s.running());
        QVERIFY(s.stopPending());
        QVERIFY(s.stopPending());
        QCOMPARE(o.rows.size(), size_t(5));
        QCOMPARE(o.rows[0], startPendingRow(1));
        QCOMPARE(o.rows[1], startPendingRow(2));
        QCOMPARE(o.rows[2], runningRow());
        QCOMPARE(o.rows[3], stopPendingRow(1));   // restarts at 1 on entry
        QCOMPARE(o.rows[4], stopPendingRow(2));
        QCOMPARE(s.state(), ServiceState::StopPending);
    }

    // INV-60 mutant "Running after stop request".
    void noRunningAfterStop()
    {
        Order o;
        Recorder r(o);
        ServiceStatus s(r);
        QVERIFY(s.startPending());
        QVERIFY(s.stopPending());                 // a stop requested during start
        QVERIFY(!s.running());                    // never Running after it
        QVERIFY(!s.startPending());               // nor another start checkpoint
        QCOMPARE(o.rows.size(), size_t(2));
        QCOMPARE(o.rows.back().currentState, svc::kStopPending);
        QCOMPARE(s.state(), ServiceState::StopPending);
        QVERIFY(s.stopRequested());
    }

    void runningOnlyFromStartPending()
    {
        Order o;
        Recorder r(o);
        ServiceStatus s(r);
        QVERIFY(s.running());
        QVERIFY(!s.running());                    // Running -> Running is not a transition
        QVERIFY(!s.startPending());
        QCOMPARE(o.rows.size(), size_t(1));
    }

    void nothingLeavesStopped()
    {
        Order o;
        Recorder r(o);
        ServiceStatus s(r);
        QVERIFY(s.startPending());
        s.markStopped();
        QVERIFY(!s.startPending());
        QVERIFY(!s.running());
        QVERIFY(!s.stopPending());
        QCOMPARE(o.rows.size(), size_t(1));
        QCOMPARE(s.state(), ServiceState::Stopped);
    }

    // The final report is the pre-built row, made through the final reporter without the status mutex: a thread
    // holding that mutex cannot delay it (INV-82/INV-93).
    void finalReportTakesNoMutex()
    {
        Order o;
        Recorder r(o);
        ServiceStatus s(r);
        ServiceExit::setFinalReporter(&r);
        std::unique_lock held(s.mutexForTest());
        std::thread t([] { ServiceExit::reportStopped(4); });
        t.join();   // would deadlock if the final report waited for the status mutex
        held.unlock();
        ServiceExit::setFinalReporter(nullptr);
        QCOMPARE(o.rows.size(), size_t(1));
        QCOMPARE(o.rows[0], stoppedRow(4));
    }

    void noReporterNoReport()
    {
        ServiceExit::setFinalReporter(nullptr);
        ServiceExit::reportStopped(0);   // must not crash
        QVERIFY(ServiceExit::finalReporter() == nullptr);
    }

    // ---- the host's decisions (ServiceLifecycle) ----

    // INV-81 / D59: STOP arms the non-reporting kill deadline at STOP + 20 s BEFORE the StopPending report; the
    // orderly stop then ends through the exit primitive with code 0 (tighten, then the STOPPED report).
    void killDeadlineArmedBeforeStopPending()
    {
        Rig rig;
        QVERIFY(rig.status.startPending());
        QVERIFY(rig.life.started(RunnerStart::Ok));
        rig.order.calls.clear();
        QCOMPARE(rig.life.control(svc::kControlStop), svc::kNoError);
        QCOMPARE(QString::fromStdString(rig.order.calls), QStringLiteral("aR"));
        QVERIFY(rig.kill.armed);
        QCOMPARE(*rig.kill.armed, rig.clock.now() + std::chrono::seconds(20));
        QCOMPARE(rig.kill.code, 0);
        QCOMPARE(rig.order.rows.back(), stopPendingRow(1));
        rig.runMain();   // runner.stop queued onto the main thread
        QVERIFY(rig.stopDone);
        rig.stopDone();
        // side channel shut down (<= 2 s), then the exit primitive: tighten, then the final report.
        QCOMPARE(QString::fromStdString(rig.order.calls), QStringLiteral("aRSFXtR"));
        QCOMPARE(g_exits, 1);
        QCOMPARE(g_exitCode, 0);
        QVERIFY(g_exitReport == &ServiceExit::reportStopped);
        QCOMPARE(rig.order.rows.back(), stoppedRow(0));
    }

    // INV-93 mutant "report before tightening in a startup exit": every startup refusal reaches SCM only through
    // the exit primitive, after the kill deadline was tightened. (The spec names this row
    // tst_Exit::killDeadlineArmedBeforeReport; WS-4's tst_Exit proves the primitive's own order.)
    void killDeadlineArmedBeforeReport_data()
    {
        QTest::addColumn<int>("result");
        QTest::addColumn<int>("code");
        QTest::newRow("untrusted") << int(RunnerStart::Untrusted) << 3;
        QTest::newRow("data folder unknown") << int(RunnerStart::DataFolderUnknown) << 3;
        QTest::newRow("lock busy") << int(RunnerStart::EngineLockBusy) << 4;
    }
    void killDeadlineArmedBeforeReport()
    {
        QFETCH(int, result);
        QFETCH(int, code);
        Rig rig;
        QVERIFY(rig.status.startPending());
        rig.order.calls.clear();
        QVERIFY(!rig.life.started(RunnerStart(result)));
        QCOMPARE(QString::fromStdString(rig.order.calls), QStringLiteral("XtR"));
        QCOMPARE(g_exitCode, code);
        QCOMPARE(rig.kill.code, code);
        QCOMPARE(rig.order.rows.back(), stoppedRow(code));
        // These stay failures: SERVICE_SPECIFIC with the code, so SCM's recovery actions run (unlike not activated).
        QCOMPARE(rig.order.rows.back().win32ExitCode, svc::kErrorServiceSpecific);
        QCOMPARE(rig.order.rows.back().serviceSpecificExitCode, quint32(code));
        QVERIFY(rig.order.said.isEmpty());
        QCOMPARE(rig.status.state(), ServiceState::Stopped);
    }

    // v1.1 station fix (a stopped service restarted by SCM every 30 s for hours): a start without a valid start token
    // is not a failure. The kill deadline is tightened (now + 5 s, code 6) BEFORE the line, which may block; the line
    // names the token's reason and the way out; then the exit primitive (process code 6) reports STOPPED with
    // NO_ERROR, so the failure actions (fFailureActionsOnNonCrashFailures) never fire.
    // Kills: the failure-style report (SERVICE_SPECIFIC 6), the line not said, and the line said before the tighten.
    void notActivatedIsANormalStop()
    {
        Rig rig;
        QVERIFY(rig.status.startPending());
        rig.order.calls.clear();
        QVERIFY(!rig.life.started(RunnerStart::NotActivated, QStringLiteral("start.token is missing")));
        QCOMPARE(QString::fromStdString(rig.order.calls), QStringLiteral("tWXtR"));
        QCOMPARE(rig.order.said.size(), 1);
        QVERIFY2(rig.order.said.front().contains(QLatin1String("start.token is missing")), qPrintable(rig.order.said.front()));
        QVERIFY2(rig.order.said.front().contains(QLatin1String("Stop")), qPrintable(rig.order.said.front()));
        QVERIFY2(rig.order.said.front().contains(QLatin1String("Start or Save & apply")), qPrintable(rig.order.said.front()));
        QCOMPARE(g_exits, 1);
        QCOMPARE(g_exitCode, 6);   // the process exit code still says why
        QVERIFY(g_exitReport == &ServiceExit::reportStopped);
        const NativeServiceStatus last = rig.order.rows.back();
        QCOMPARE(last.currentState, svc::kStopped);
        QCOMPARE(last.win32ExitCode, svc::kNoError);
        QCOMPARE(last.serviceSpecificExitCode, 0u);
        QCOMPARE(last, stoppedRow(0));
        QCOMPARE(rig.status.state(), ServiceState::Stopped);
    }

    // The line's deadline: now + kSayBudget (5 s), code 6, set before the line (a blocked write ends the process).
    void notActivatedLineIsBounded()
    {
        Rig rig;
        std::optional<std::chrono::steady_clock::time_point> deadlineWhenSaid;
        int codeWhenSaid = -1;
        rig.onSay = [&] {
            deadlineWhenSaid = rig.kill.tightened;
            codeWhenSaid = rig.kill.code;
        };
        QVERIFY(rig.status.startPending());
        QVERIFY(!rig.life.started(RunnerStart::NotActivated, QStringLiteral("start.token was revoked")));
        QVERIFY(deadlineWhenSaid);
        QCOMPARE(*deadlineWhenSaid, rig.clock.now() + ServiceLifecycle::kSayBudget);
        QCOMPARE(codeWhenSaid, 6);
        QVERIFY(!rig.kill.armed);   // tightened, never armed/extended: nothing can move it later
    }

    void refuseGoesThroughTheExitPrimitive()
    {
        Rig rig;
        rig.order.calls.clear();
        rig.life.refuse(5);
        QCOMPARE(QString::fromStdString(rig.order.calls), QStringLiteral("XtR"));
        QCOMPARE(g_exitCode, 5);
    }

    // A stop requested while the startup is still running: the startup's Running is refused, and the queued stop
    // ends the process (INV-60 "stop during StartPending").
    void stopDuringStartPendingNeverReportsRunning()
    {
        Rig rig;
        QVERIFY(rig.status.startPending());
        QCOMPARE(rig.life.control(svc::kControlStop), svc::kNoError);
        QVERIFY(!rig.life.started(RunnerStart::Ok));
        rig.runMain();
        rig.stopDone();
        for (const NativeServiceStatus& r : rig.order.rows) QVERIFY(r.currentState != svc::kRunning);
        QCOMPARE(rig.order.rows.back(), stoppedRow(0));
        QCOMPARE(g_exits, 1);
    }

    void secondStopChangesNothing()
    {
        Rig rig;
        QVERIFY(rig.status.startPending());
        QVERIFY(rig.life.started(RunnerStart::Ok));
        QCOMPARE(rig.life.control(svc::kControlStop), svc::kNoError);
        const std::string after = rig.order.calls;
        QCOMPARE(rig.life.control(svc::kControlShutdown), svc::kNoError);
        QCOMPARE(rig.life.control(svc::kControlPreshutdown), svc::kNoError);
        QCOMPARE(rig.order.calls, after);
        QCOMPARE(rig.mainQueue.size(), size_t(1));
    }

    void shutdownAndPreshutdownStopToo()
    {
        for (quint32 c : {svc::kControlShutdown, svc::kControlPreshutdown}) {
            Rig rig;
            QVERIFY(rig.status.startPending());
            QVERIFY(rig.life.started(RunnerStart::Ok));
            rig.order.calls.clear();
            QCOMPARE(rig.life.control(c), svc::kNoError);
            QCOMPARE(QString::fromStdString(rig.order.calls), QStringLiteral("aR"));
        }
    }

    void otherControls()
    {
        Rig rig;
        QCOMPARE(rig.life.control(svc::kControlInterrogate), svc::kNoError);
        QCOMPARE(rig.life.control(0x2), svc::kErrorCallNotImplemented);   // PAUSE
        QVERIFY(rig.order.calls.empty());
    }

    // The kill deadline is armed, then only tightened: nothing in the stop path extends it (INV-81 mutant "let a
    // firewall-check success extend the stop deadline" is native; this pins the host's own calls).
    void killDeadlineNeverExtended()
    {
        Rig rig;
        QVERIFY(rig.status.startPending());
        QVERIFY(rig.life.started(RunnerStart::Ok));
        rig.life.control(svc::kControlStop);
        rig.runMain();
        rig.stopDone();
        QVERIFY(rig.order.calls.find('e') == std::string::npos);
        QVERIFY(rig.order.calls.find('x') == std::string::npos);
    }
};

QTEST_GUILESS_MAIN(tst_ServiceStatus)
#include "tst_ServiceStatus.moc"
