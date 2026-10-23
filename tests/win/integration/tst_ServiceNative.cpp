// WS-7 native rows on windows-latest (admin, real SCM, real Windows Firewall through CIM, real job objects): the
// service installed into %ProgramFiles%\AetherSerialBridge by the Applier over the real Windows Platform, started
// and stopped by SCM, its test seams driven through the service's own Environment value (ServiceMain.cpp).
//
//  INV-60   svc_install_ack_echo_stop_uninstall (the full path), untrusted_start_reports_3, stop_during_start_pending
//  INV-103  engine_lock_busy_exits_4, lock_held_until_process_exit
//  INV-113  stop_deletes_token (then a raw start exits 6, Stopped(not activated) = NO_ERROR, the line in bridge.log)
//  v1.1 station fix  stopped_start_is_not_a_failure (--stop, then an SCM start: no 7024/7031/7034, never restarted)
//  INV-7    svc_fault_restarts (PID1 -> fault -> PID2 running <= 10 s; recovery actions read back)
//  INV-82   slot_exception_terminates (with the status mutex held by another thread), qfatal_terminates
//  INV-81   stop_with_blocked_device_close, stop_with_blocked_teardown_and_blocked_report (the B9-1 fixture: report
//           blocking activated after Running, the StopPending report blocked, firewall checks succeeding),
//           stop_with_blocked_sink
//  INV-93   orderly_stop_with_blocked_worker (<= 3 s), stop_with_blocked_final_report (<= 3 s),
//           startup_exit_with_blocked_report (exit 6 with SetServiceStatus blocked: gone <= 2.5 s)
//  INV-117  stale_rule_at_start_never_listens, admin_rule_in_reserved_group_exits_7
//  Foreign  foreign_service_refused
// Every row starts from a clean slate (no service, no Environment value) and leaves one.

#include "ServiceNativeSupport.h"
#include "WinNativeSupport.h"

#include "core/StatusStore.h"
#include "platform/win/CimFirewall.h"
#include "platform/win/EngineLock.h"
#include "platform/win/NativeSeams.h"
#include "platform/win/WbemSession.h"
#include "platform/win/WindowsService.h"

#include "core/AppLocations.h"
#include "core/BridgeRunner.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTest>

#include <thread>

using namespace asb;
using namespace asb::test;
using namespace std::chrono;

namespace {
constexpr int kPort = 47011;

QString eventPrefix()
{
    static const QString p = QStringLiteral("Global\\asbsvcevt-%1").arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
    return p;
}

QString reportLog() { return QDir::toNativeSeparators(QDir::tempPath() + QLatin1Char('/') + QLatin1String(names::kReportLog)); }

QStringList seams(QStringList kv)
{
    kv << QStringLiteral("ASB_TEST_EVENT_PREFIX=") + eventPrefix() << QStringLiteral("ASB_TEST_REPORT_LOG=") + reportLog();
    return kv;
}

QString reportLines()
{
    QFile f(reportLog());
    return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()) : QString();
}

// Signals a test-seam event (releases whatever blocks on it).
void release(const QString& suffix)
{
    const QString name = eventPrefix() + QLatin1Char('-') + suffix;
    HANDLE e = CreateEventW(nullptr, TRUE, FALSE, reinterpret_cast<const wchar_t*>(name.utf16()));
    if (e) {
        SetEvent(e);
        CloseHandle(e);
    }
}
}  // namespace

class tst_ServiceNative : public QObject {
    Q_OBJECT

    std::unique_ptr<win::WindowsPlatform> m_p;
    bool m_hadInstallFolders = false;   // the install or data folder existed before the first row

    win::WindowsPlatform& p() { return *m_p; }

    // Installed, started with the echo device and the given seams, ACK'ed: the pid of the running service.
    quint32 runningWith(const QStringList& kv, const QByteArray& config = svc::configJson(kPort))
    {
        const Result r = svc::install(p(), config);
        if (r.outcome != Outcome::Ok) qWarning("install: %s", qPrintable(svc::describe(r)));
        if (r.outcome != Outcome::Ok) return 0;
        const Result s = svc::stop(p());
        if (s.outcome != Outcome::Ok) qWarning("stop: %s", qPrintable(svc::describe(s)));
        svc::setServiceEnvironment(seams(QStringList{QStringLiteral("ASB_TEST_DEVICE=echo")} + kv));
        QFile::remove(reportLog());
        const Result st = svc::start(p());
        if (st.outcome != Outcome::Ok) qWarning("start: %s", qPrintable(svc::describe(st)));
        if (st.outcome != Outcome::Ok) return 0;
        return svc::query().pid;
    }

    void cleanSlate()
    {
        if (svc::query().exists) {
            svc::setServiceEnvironment({});
            const Result r = svc::uninstall(p());
            if (r.outcome != Outcome::Ok && r.outcome != Outcome::SavedNotApplied) qWarning("uninstall: %s", qPrintable(svc::describe(r)));
        }
        if (!svc::removeServiceHard(p().installedExe())) qWarning("the service is still not deleted after 60 s");
        release(QStringLiteral("cim"));
    }

private slots:
    void initTestCase()
    {
        QString err;
        m_p = svc::platform(&err);
        QVERIFY2(m_p, qPrintable(err));
        m_hadInstallFolders = QFileInfo::exists(p().lockDirectory()) || QFileInfo::exists(p().dataFolder());
        svc::removeServiceHard(p().installedExe());
        const QStringList copied = svc::deployDevRuntime(p().lockDirectory());
        qInfo("dev runtime: %s", qPrintable(copied.join(QStringLiteral(", "))));
    }
    // The install and data folders the rows created (the service, the dev runtime, the lock pair, the config) are
    // removed at the end, unless they were there before the test began (a persistent runner keeps nothing).
    void cleanupTestCase()
    {
        QFile::remove(reportLog());   // the report seam's log (a persistent runner keeps nothing)
        if (!m_p || m_hadInstallFolders) return;
        native::removeTree(p().lockDirectory());
        native::removeTree(p().dataFolder());
    }
    void init() { cleanSlate(); }
    void cleanup() { cleanSlate(); }

    // The whole path: install (copy, service, lock pair, firewall, token, start), ACK, bytes through the engine,
    // the firewall rule as the comparator sees it, an orderly stop (STOPPED, exit 0), the uninstall.
    void svc_install_ack_echo_stop_uninstall()
    {
        const Result r = svc::install(p(), svc::configJson(kPort, QStringLiteral("127.0.0.1, 10.0.0.0/8")));
        QVERIFY2(r.outcome == Outcome::Ok, qPrintable(svc::describe(r)));
        svc::ScmState s;
        QVERIFY(svc::waitState(SERVICE_RUNNING, 10000, &s));
        QVERIFY(QFileInfo::exists(p().installedExe()));
        QCOMPARE(p().service().detect(steady_clock::now() + seconds(10)).state, InstallState::Current);
        // The firewall the service checks: the comparator over the ActiveStore matches the committed plan.
        {
            QStringList ids;
            win::WbemSession cim;
            cim.connect();
            for (const auto& rule : win::CimFirewall::readGroup(cim, win::cim::Store::Active))
                ids << rule.rule.value(QStringLiteral("InstanceID")).s;
            QCOMPARE(ids.size(), 1);
        }
        QVERIFY(svc::canConnect(kPort, 3000));
        // An orderly stop from SCM: StopPending then STOPPED with exit 0, the process gone.
        void* proc = svc::openProcess(s.pid);
        QCOMPARE(svc::stopSync(), 0u);
        milliseconds took{};
        QVERIFY(svc::waitExit(proc, 10000, &took));
        svc::closeHandle(proc);
        QVERIFY(svc::waitState(SERVICE_STOPPED, 5000, &s));
        QCOMPARE(s.win32ExitCode, 0u);
        const Result u = svc::uninstall(p());
        QVERIFY2(u.outcome == Outcome::Ok || u.outcome == Outcome::SavedNotApplied, qPrintable(svc::describe(u)));
        QVERIFY2(svc::waitDeleted(60000), "SCM did not delete the service within 60 s");
        win::WbemSession cim;
        cim.connect();
        QVERIFY(win::CimFirewall::readGroup(cim, win::cim::Store::Persistent).isEmpty());
    }

    void echoThroughTheEngine()
    {
        const quint32 pid = runningWith({});
        QVERIFY(pid);
        QString why;
        QVERIFY2(svc::echoRoundTrip(kPort, QByteArrayLiteral("PS0;FA;"), 5000, &why), qPrintable(why));
    }

    // INV-22 / INV-60: an untrusted data folder: Stopped(untrusted) = SERVICE_SPECIFIC 3, nothing written.
    void untrusted_start_reports_3()
    {
        QVERIFY(runningWith({}));
        QVERIFY(svc::stop(p()).outcome == Outcome::Ok);
        // B0 revoked the token; a raw start would exit 6 before trust is even read, so plant one back is not
        // needed: trust is checked BEFORE the token (lock -> trust -> token). Make the folder untrusted.
        const QString folder = p().dataFolder();
        const QString before = native::sddlOf(folder, DACL_SECURITY_INFORMATION);
        QVERIFY(native::setSddl(folder, before + QStringLiteral("(A;OICI;FA;;;BU)"), DACL_SECURITY_INFORMATION));
        const qint64 logSize = QFileInfo(folder + QStringLiteral("\\bridge.log")).size();
        const QDateTime statusTime = QFileInfo(folder + QStringLiteral("\\status.json")).lastModified();
        QCOMPARE(svc::startRaw(), 0u);
        svc::ScmState s;
        QVERIFY(svc::waitState(SERVICE_STOPPED, 15000, &s));
        QVERIFY(native::setSddl(folder, before, DACL_SECURITY_INFORMATION));
        QCOMPARE(s.win32ExitCode, 1066u);
        QCOMPARE(s.specificExitCode, 3u);
        QCOMPARE(QFileInfo(folder + QStringLiteral("\\bridge.log")).size(), logSize);
        QCOMPARE(QFileInfo(folder + QStringLiteral("\\status.json")).lastModified(), statusTime);
    }

    // INV-113: Stop deletes the token; a start without one is Stopped(not activated). v1.1 station fix: that is a
    // normal stop for SCM (NO_ERROR, no service-specific code), and bridge.log says why in one line.
    void stop_deletes_token()
    {
        QVERIFY(runningWith({}));
        QVERIFY(QFileInfo::exists(p().dataFolder() + QStringLiteral("\\start.token")));
        const Result r = svc::stop(p());
        QVERIFY2(r.outcome == Outcome::Ok, qPrintable(svc::describe(r)));
        QVERIFY(!QFileInfo::exists(p().dataFolder() + QStringLiteral("\\start.token")));
        QCOMPARE(svc::startRaw(), 0u);
        svc::ScmState s;
        QVERIFY(svc::waitState(SERVICE_STOPPED, 15000, &s));
        QCOMPARE(s.win32ExitCode, 0u);
        QCOMPARE(s.specificExitCode, 0u);
        QFile log(p().dataFolder() + QStringLiteral("\\bridge.log"));
        QVERIFY(log.open(QIODevice::ReadOnly));
        const QByteArray text = log.readAll();
        QVERIFY2(text.contains("Not activated (start.token is missing)"), text.right(600).constData());
    }

    // v1.1 station fix. On a production station a service in SCM's recovery cycle was stopped with --stop; SCM then
    // restarted it every 30 s for over 3 hours (388 restarts), each start exiting 6 with a 7024 AND a 7031, because the
    // install sets fFailureActionsOnNonCrashFailures and SERVICE_SPECIFIC 6 counted as a failure. The same happens at
    // boot (Stop keeps the start type: Automatic) and on any manual start. Here: install, --stop, an SCM start
    // (sc start); SCM records no failure for it (no 7023, 7024, 7031 or 7034), and it stays STOPPED with no process.
    // The wait: this row's service is new (cleanSlate), so its failure count is 0 and a failure's first recovery action
    // is a restart after 5 s (5 s / 5 s / 30 s): 12 s without a new process is more than twice that.
    void stopped_start_is_not_a_failure()
    {
        QVERIFY(runningWith({}));
        const Result r = svc::stop(p());   // the CLI's --stop and the window's Stop
        QVERIFY2(r.outcome == Outcome::Ok, qPrintable(svc::describe(r)));
        svc::ScmState s;
        QVERIFY(svc::waitState(SERVICE_STOPPED, 15000, &s));
        const quint32 mark = svc::systemLogMark();
        QVERIFY(mark != 0);
        QCOMPARE(svc::startRaw(), 0u);   // what a boot (Automatic), a scheduled recovery restart or sc start does
        QElapsedTimer t;
        t.start();
        QVERIFY(svc::waitState(SERVICE_STOPPED, 15000, &s));
        QCOMPARE(s.win32ExitCode, 0u);
        QCOMPARE(s.specificExitCode, 0u);
        while (t.elapsed() < 12000) {
            const svc::ScmState q = svc::query();
            QVERIFY2(q.state == SERVICE_STOPPED && q.pid == 0,
                     qPrintable(QStringLiteral("SCM restarted the stopped service: state %1, pid %2").arg(q.state).arg(q.pid)));
            Sleep(250);
        }
        QVERIFY(!svc::canConnect(kPort, 500));   // a stopped service never opens a port
        bool logRead = false;
        const QStringList failures = svc::scmEventsAfter(mark, {7023u, 7024u, 7031u, 7034u}, QString::fromLatin1(ServicePlan::kDisplayName), &logRead);
        QVERIFY2(logRead, "the System event log could not be read");
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QStringLiteral("; "))));
        QCOMPARE(svc::query().state, quint32(SERVICE_STOPPED));
    }

    // INV-103: engine.lock held by someone else: Stopped(lock busy) = SERVICE_SPECIFIC 4.
    void engine_lock_busy_exits_4()
    {
        QVERIFY(runningWith({}));
        QVERIFY(svc::stop(p()).outcome == Outcome::Ok);
        win::EngineLock other(win::nativeSeams(), p().lockDirectory(), win::LockRole::Applier);
        LockResult why = LockResult::Busy;
        auto hold = other.tryAcquire(&why);
        QVERIFY2(hold, qPrintable(other.lastError()));
        QCOMPARE(svc::startRaw(), 0u);
        svc::ScmState s;
        QVERIFY(svc::waitState(SERVICE_STOPPED, 15000, &s));
        QCOMPARE(s.win32ExitCode, 1066u);
        QCOMPARE(s.specificExitCode, 4u);
    }

    // INV-103 lock_held_until_process_exit: between the runner's stop and the exit (a test pause), the engine
    // lock is still held: an Applier's acquisition fails.
    void lock_held_until_process_exit()
    {
        const quint32 pid = runningWith({QStringLiteral("ASB_TEST_PAUSE_BEFORE_EXIT_MS=6000")});
        QVERIFY(pid);
        void* proc = svc::openProcess(pid);
        svc::stopAsync();
        Sleep(2500);   // the stop has run; the process pauses before Exit::now
        QVERIFY(!svc::waitExit(proc, 0));
        win::EngineLock other(win::nativeSeams(), p().lockDirectory(), win::LockRole::Applier);
        LockResult why = LockResult::Acquired;
        QVERIFY(!other.tryAcquire(&why));
        QCOMPARE(int(why), int(LockResult::Busy));
        QVERIFY(svc::waitExit(proc, 15000));
        svc::closeHandle(proc);
        auto hold = other.tryAcquire(&why);
        QVERIFY(hold);   // released by the process exit only
    }

    // INV-60: STOP while the service is still START_PENDING is refused by SCM (no controls accepted); the
    // service reaches Running, a later STOP works, and no Running report ever follows a StopPending one.
    void stop_during_start_pending()
    {
        QVERIFY(runningWith({QStringLiteral("ASB_TEST_HOLD_START_MS=6000")}));
        QVERIFY(svc::stop(p()).outcome == Outcome::Ok);
        QFile::remove(reportLog());
        std::thread starter([&] { svc::start(p()); });   // the Applier starts it: 6 s in START_PENDING
        QElapsedTimer t;
        t.start();
        quint32 refused = 0;
        bool caught = false;
        while (t.elapsed() < 20000 && !caught) {
            // Our own StartPending reports (checkpoint >= 2): the process is connected and accepts no control.
            const svc::ScmState q = svc::query();
            if (q.state == SERVICE_START_PENDING && q.checkPoint >= 2) {
                refused = svc::stopSync();
                caught = true;
            }
            Sleep(20);
        }
        starter.join();
        QVERIFY(caught);
        // SCM refuses the control: ERROR_INVALID_SERVICE_CONTROL (measured) or ERROR_SERVICE_CANNOT_ACCEPT_CTRL.
        QVERIFY2(refused == ERROR_INVALID_SERVICE_CONTROL || refused == ERROR_SERVICE_CANNOT_ACCEPT_CTRL, qPrintable(QString::number(refused)));
        svc::ScmState s;
        QVERIFY(svc::waitState(SERVICE_RUNNING, 20000, &s));
        QCOMPARE(svc::stopSync(), 0u);
        QVERIFY(svc::waitState(SERVICE_STOPPED, 15000, &s));
        bool stopSeen = false;
        for (const QString& l : reportLines().split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const int state = l.section(QLatin1Char(' '), 0, 0).toInt();
            if (state == 3) stopSeen = true;
            QVERIFY2(!(stopSeen && state == 4), qPrintable(reportLines()));
        }
        QVERIFY(stopSeen);
    }

    // INV-7: a fault ends the process; SCM restarts it (PID1 -> PID2 running <= 10 s); recovery actions read back.
    void svc_fault_restarts()
    {
        const quint32 pid1 = runningWith({QStringLiteral("ASB_TEST_FAULT=slot-exception"), QStringLiteral("ASB_TEST_FAULT_AFTER_MS=1500")});
        QVERIFY(pid1);
        const quint32 mark = svc::systemLogMark();
        void* proc = svc::openProcess(pid1);
        quint32 code = 0;
        QVERIFY(svc::waitExit(proc, 10000, nullptr, &code));
        svc::closeHandle(proc);
        QCOMPARE(code, 1u);
        QElapsedTimer t;
        t.start();
        quint32 pid2 = 0;
        while (t.elapsed() < 10000 && !pid2) {
            const svc::ScmState s = svc::query();
            if (s.state == SERVICE_RUNNING && s.pid && s.pid != pid1) pid2 = s.pid;
            Sleep(50);
        }
        QVERIFY2(pid2, "SCM did not restart the service within 10 s");
        // The positive control of stopped_start_is_not_a_failure's event check: a fault IS logged as a failure.
        bool logRead = false;
        const QStringList failures = svc::scmEventsAfter(mark, {7031u, 7034u}, QString::fromLatin1(ServicePlan::kDisplayName), &logRead);
        QVERIFY2(logRead, "the System event log could not be read");
        QVERIFY2(!failures.isEmpty(), "no 7031/7034 for the fault");
        // The recovery configuration as SCM holds it.
        auto& scm = win::nativeScmSeams();
        win::ScmSeams::Handle h = 0;
        QCOMPARE(scm.openService(QStringLiteral("AetherSerialBridge"), SERVICE_QUERY_CONFIG, &h), 0u);
        QList<ServicePlan::FailureAction> actions;
        int reset = 0;
        bool flag = false;
        QCOMPARE(scm.queryFailureActions(h, &actions, &reset, &flag), 0u);
        scm.close(h);
        QCOMPARE(actions.size(), 3);
        QCOMPARE(actions[0].delayMs, 5000);
        QCOMPARE(actions[2].delayMs, 30000);
        QCOMPARE(reset, 86400);
        QVERIFY(flag);
    }

    // INV-82: faults terminate without status or locks, even with the status mutex held by another thread.
    void slot_exception_terminates_data()
    {
        QTest::addColumn<QString>("fault");
        QTest::newRow("slot exception, status mutex held") << QStringLiteral("slot-exception-status-mutex");
        QTest::newRow("qfatal") << QStringLiteral("qfatal");
        QTest::newRow("std::terminate") << QStringLiteral("terminate");
    }
    void slot_exception_terminates()
    {
        QFETCH(QString, fault);
        const quint32 pid = runningWith({QStringLiteral("ASB_TEST_FAULT=") + fault, QStringLiteral("ASB_TEST_FAULT_AFTER_MS=1000")});
        QVERIFY(pid);
        void* proc = svc::openProcess(pid);
        quint32 code = 0;
        QVERIFY(svc::waitExit(proc, 10000, nullptr, &code));
        svc::closeHandle(proc);
        QCOMPARE(code, 1u);
        // No orderly report after Running: the fault reported nothing.
        const QStringList lines = reportLines().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QVERIFY(!lines.isEmpty());
        QCOMPARE(lines.last().section(QLatin1Char(' '), 0, 0).toInt(), 4);   // the last report is Running
    }

    // INV-81: a device close blocked in the driver: the kill deadline ends the process at STOP + 20 s.
    void stop_with_blocked_device_close()
    {
        const quint32 pid = runningWith({QStringLiteral("ASB_TEST_DEVICE=block-close")});
        QVERIFY(pid);
        QVERIFY(svc::canConnect(kPort, 3000));
        QTcpSocket client;
        client.connectToHost(QStringLiteral("127.0.0.1"), kPort);
        QVERIFY(client.waitForConnected(3000));
        Sleep(1000);   // the session opened the device
        void* proc = svc::openProcess(pid);
        svc::stopAsync();
        milliseconds took{};
        quint32 code = 99;
        QVERIFY2(svc::waitExit(proc, 30000, &took, &code), "still alive 30 s after STOP");
        svc::closeHandle(proc);
        QVERIFY2(took >= seconds(18) && took <= milliseconds(21500), qPrintable(QString::number(took.count())));
        QCOMPARE(code, 0u);   // faultDie(0): no report
    }

    // INV-81 / B9-1 fixture (README nit a): report blocking activated after Running; the StopPending report blocks
    // forever AND the device close blocks; the firewall checks keep succeeding (STOP 25 s after Running, so the
    // 30 s check succeeds during the stop). The process is dead <= 21 s after STOP: the kill deadline was armed
    // before the first potentially blocking report, and nothing extends it.
    void stop_with_blocked_teardown_and_blocked_report()
    {
        const quint32 pid = runningWith({QStringLiteral("ASB_TEST_DEVICE=block-close"), QStringLiteral("ASB_TEST_BLOCK_REPORT=stop-pending")});
        QVERIFY(pid);
        QElapsedTimer sinceRunning;
        sinceRunning.start();
        QTcpSocket client;
        client.connectToHost(QStringLiteral("127.0.0.1"), kPort);
        QVERIFY(client.waitForConnected(3000));
        while (sinceRunning.elapsed() < 25000) Sleep(100);
        void* proc = svc::openProcess(pid);
        svc::stopAsync();
        milliseconds took{};
        QVERIFY2(svc::waitExit(proc, 40000, &took), "still alive 40 s after STOP");
        svc::closeHandle(proc);
        QVERIFY2(took <= milliseconds(21500), qPrintable(QString::number(took.count())));
        QVERIFY2(reportLines().contains(QLatin1String("\n3 1 ")) || reportLines().startsWith(QLatin1String("3 1 ")), qPrintable(reportLines()));
    }

    // INV-93 / B9-1 (separate case): the FINAL report blocks; the exit primitive tightened the kill deadline to
    // now + 2 s first, so the process is gone <= 3 s after STOP (the report-then-kill mutant would wait 20 s).
    void stop_with_blocked_final_report()
    {
        const quint32 pid = runningWith({QStringLiteral("ASB_TEST_BLOCK_REPORT=final")});
        QVERIFY(pid);
        void* proc = svc::openProcess(pid);
        svc::stopAsync();
        milliseconds took{};
        QVERIFY2(svc::waitExit(proc, 30000, &took), "still alive 30 s after STOP");
        svc::closeHandle(proc);
        QVERIFY2(took <= milliseconds(3500), qPrintable(QString::number(took.count())));
    }

    // INV-81 stop_with_blocked_sink / INV-93 orderly_stop_with_blocked_worker: the side channel's sink blocks;
    // the stop waits <= 2 s for it, then exits: gone <= 3 s after STOP.
    void orderly_stop_with_blocked_worker()
    {
        const quint32 pid = runningWith({QStringLiteral("ASB_TEST_BLOCK_SINK=1")});
        QVERIFY(pid);
        void* proc = svc::openProcess(pid);
        svc::stopAsync();
        milliseconds took{};
        QVERIFY2(svc::waitExit(proc, 30000, &took), "still alive 30 s after STOP");
        svc::closeHandle(proc);
        QVERIFY2(took <= milliseconds(3500), qPrintable(QString::number(took.count())));
    }

    // INV-93 fatal_with_blocked_log: the side channel's sink is blocked; a fault still ends the process at once
    // (Fault::die does no I/O) and SCM recovery restarts it.
    void fatal_with_blocked_log()
    {
        const quint32 pid = runningWith({QStringLiteral("ASB_TEST_BLOCK_SINK=1"), QStringLiteral("ASB_TEST_FAULT=slot-exception"),
                                         QStringLiteral("ASB_TEST_FAULT_AFTER_MS=5000")});
        QVERIFY(pid);
        QElapsedTimer sinceRunning;
        sinceRunning.start();
        void* proc = svc::openProcess(pid);
        quint32 code = 0;
        QVERIFY(svc::waitExit(proc, 20000, nullptr, &code));
        svc::closeHandle(proc);
        QCOMPARE(code, 1u);
        // The fault fires 5 s after Running (the sink has been blocked since +2 s): dead within 1 s of it.
        QVERIFY2(sinceRunning.elapsed() <= 6500, qPrintable(QString::number(sinceRunning.elapsed())));
        quint32 restarted = 0;
        QElapsedTimer t;
        t.start();
        while (!restarted && t.elapsed() < 15000) {
            const svc::ScmState s = svc::query();
            if (s.state == SERVICE_RUNNING && s.pid && s.pid != pid) restarted = s.pid;
            Sleep(50);
        }
        QVERIFY2(restarted, "SCM recovery did not restart the service");
    }

    // INV-93: a startup refusal with SetServiceStatus blocked: the kill deadline (tightened to now + 2 s first)
    // ends the process <= 2.5 s after it began.
    void startup_exit_with_blocked_report()
    {
        QVERIFY(runningWith({}));
        QVERIFY(svc::stop(p()).outcome == Outcome::Ok);   // no token now: the next start exits 6
        QVERIFY(svc::setServiceEnvironment(seams({QStringLiteral("ASB_TEST_BLOCK_REPORT=final")})));
        QCOMPARE(svc::startRaw(), 0u);
        svc::ScmState s;
        QElapsedTimer t;
        t.start();
        while (!(s = svc::query()).pid && t.elapsed() < 5000) Sleep(5);
        void* proc = svc::openProcess(s.pid);
        quint32 code = 0;
        milliseconds took{};
        QVERIFY(svc::waitExit(proc, 10000, &took, &code));
        svc::closeHandle(proc);
        QVERIFY2(took <= milliseconds(2500), qPrintable(QString::number(took.count())));
        QCOMPARE(code, 6u);
    }

    // INV-117: an enabled rule an administrator puts into the reserved group: the running engine exits 7 within
    // 35 s (process gone, listener closed).
    void admin_rule_in_reserved_group_exits_7()
    {
        const quint32 pid = runningWith({});
        QVERIFY(pid);
        win::WbemSession cim;
        cim.connect();
        win::cim::Props r;
        r[QStringLiteral("InstanceID")] = win::cim::Value::string(QStringLiteral("asb-test-admin-rule"));
        r[QStringLiteral("ElementName")] = win::cim::Value::string(QStringLiteral("admin rule"));
        r[QStringLiteral("RuleGroup")] = win::cim::Value::string(QStringLiteral("Aether Serial Bridge"));
        r[QStringLiteral("Direction")] = win::cim::Value::integer(1);
        r[QStringLiteral("Action")] = win::cim::Value::integer(2);
        r[QStringLiteral("Enabled")] = win::cim::Value::integer(1);
        cim.put(QStringLiteral("MSFT_NetFirewallRule"), {}, r, win::cim::PutFlag::CreateOnly, win::cim::Store::Persistent);
        void* proc = svc::openProcess(pid);
        milliseconds took{};
        quint32 code = 0;
        const bool died = svc::waitExit(proc, 40000, &took, &code);
        svc::closeHandle(proc);
        for (const auto& i : cim.query(win::groupQuery(), win::cim::Store::Persistent))
            if (i.props.value(QStringLiteral("InstanceID")).s == QLatin1String("asb-test-admin-rule")) cim.remove(i.relPath, win::cim::Store::Persistent);
        QVERIFY2(died, "the engine still runs 40 s after the rule appeared");
        QCOMPARE(code, 7u);
        QVERIFY(took <= seconds(36));
    }

    // INV-117 (mutant "skip the start-time check"): a stale rule present at start: the engine never listens.
    void stale_rule_at_start_never_listens()
    {
        QVERIFY(runningWith({}));
        QVERIFY(svc::stop(p()).outcome == Outcome::Ok);
        win::WbemSession cim;
        cim.connect();
        win::cim::Props r;
        r[QStringLiteral("InstanceID")] = win::cim::Value::string(QStringLiteral("asb-test-stale-rule"));
        r[QStringLiteral("ElementName")] = win::cim::Value::string(QStringLiteral("stale rule"));
        r[QStringLiteral("RuleGroup")] = win::cim::Value::string(QStringLiteral("Aether Serial Bridge"));
        r[QStringLiteral("Direction")] = win::cim::Value::integer(1);
        r[QStringLiteral("Action")] = win::cim::Value::integer(2);
        r[QStringLiteral("Enabled")] = win::cim::Value::integer(1);
        // The Applier's Start sweeps every rule not of its generation (R9): add the stale rule after it, with the
        // engine stopped by SCM alone (the token stays), then start it raw.
        const Result a = svc::start(p());
        QVERIFY2(a.outcome == Outcome::Ok, qPrintable(svc::describe(a)));
        QCOMPARE(svc::stopSync(), 0u);
        svc::ScmState s;
        QVERIFY(svc::waitState(SERVICE_STOPPED, 15000, &s));
        cim.put(QStringLiteral("MSFT_NetFirewallRule"), {}, r, win::cim::PutFlag::CreateOrUpdate, win::cim::Store::Persistent);
        bool listened = false;
        QCOMPARE(svc::startRaw(), 0u);
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 8000) {
            if (svc::canConnect(kPort, 100)) listened = true;
            s = svc::query();
            Sleep(50);
        }
        for (const auto& i : cim.query(win::groupQuery(), win::cim::Store::Persistent))
            if (i.props.value(QStringLiteral("InstanceID")).s == QLatin1String("asb-test-stale-rule")) cim.remove(i.relPath, win::cim::Store::Persistent);
        QVERIFY(!listened);
        // v1.1 station fix: exit 7 says why in bridge.log (it used to name nothing): the comparator's reason.
        QFile log(p().dataFolder() + QStringLiteral("\\bridge.log"));
        QVERIFY(log.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(log.readAll());
        QVERIFY2(text.contains(QStringLiteral("The startup firewall check ")) &&
                     text.contains(QStringLiteral("asb-test-stale-rule")) && text.contains(QStringLiteral("exit 7")),
                 qPrintable(text.right(2000)));
    }

    // v1.1 station fix (2026-10-18). A real station's config shape: three ports, names with spaces, bound to one
    // of this machine's own addresses (the station: its VPN address), anyone allowed (""), and a firewall check slower
    // than 1 s (12 association reads at 150 ms = 1.8 s; a replica measured 1.83-1.90 s). The service must reach RUNNING,
    // stay up past the old three-stale-checks exit (about 3 s), and listen on all three ports. Kills: the startup
    // freshness measured from the check's start (exit 7 on every start).
    void station_shape_slow_check_listens()
    {
        QString bind;
        for (const QHostAddress& a : QNetworkInterface::allAddresses())
            if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback() && !a.isLinkLocal()) {
                bind = a.toString();
                break;
            }
        QVERIFY2(!bind.isEmpty(), "this machine has no IPv4 address besides loopback and link-local");
        const QStringList names{QStringLiteral("Elecraft KXPA100"), QStringLiteral("Elecraft KAT500"), QStringLiteral("Elecraft KPA100")};
        QStringList ports;
        for (int i = 0; i < names.size(); ++i)
            ports << QStringLiteral(R"({"enabled": true, "name": "%1", "comPort": "COM%2", "baudRate": 38400, "dataBits": 8,
      "parity": "None", "stopBits": "One", "handshake": "None", "dtrEnable": false, "rtsEnable": false,
      "tcpPort": %3, "bindAddress": "%4", "allowedClients": ""})")
                         .arg(names[i]).arg(240 + i).arg(kPort + i).arg(bind);
        const QByteArray config = QStringLiteral(R"({"version": 1, "ports": [%1]})").arg(ports.join(QStringLiteral(", "))).toUtf8();
        const quint32 pid = runningWith({QStringLiteral("ASB_TEST_CIM_BLOCK=assoc-delay:150")}, config);
        QVERIFY2(pid, "the service did not start with the station's config shape and a 1.8 s firewall check");
        void* proc = svc::openProcess(pid);
        quint32 code = 0;
        const bool exited = svc::waitExit(proc, 8000, nullptr, &code);   // the old rule ended it after about 3 s
        svc::closeHandle(proc);
        QVERIFY2(!exited, qPrintable(QStringLiteral("the service exited %1 within 8 s").arg(code)));
        for (int i = 0; i < names.size(); ++i) {
            QTcpSocket s;
            s.connectToHost(bind, quint16(kPort + i));
            QVERIFY2(s.waitForConnected(3000), qPrintable(QStringLiteral("%1 is not listening on %2:%3").arg(names[i], bind).arg(kPort + i)));
            s.abort();
        }
    }

    // WS-6 deferred row: the Windows known-folder lookups fail closed — the pure branches, then the service host:
    // a failing ProgramData lookup exits 3 (kWindowsDataFolderError), a failing Program Files lookup exits 4, and no
    // guessed folder is ever used (nothing is written anywhere).
    void windows_path_lookups_fail_closed()
    {
        QCOMPARE(win::programFilesLockDirectoryFrom(QString()), std::optional<QString>());
        QCOMPARE(win::programFilesLockDirectoryFrom(QStringLiteral("Program Files")), std::optional<QString>());
        QCOMPARE(win::programFilesLockDirectoryFrom(QStringLiteral("C:")), std::optional<QString>());
        QCOMPARE(win::programFilesLockDirectoryFrom(QStringLiteral("\\\\")), std::optional<QString>());
        QCOMPARE(win::programFilesLockDirectoryFrom(QStringLiteral("C:\\Program Files")),
                 std::optional<QString>(QStringLiteral("C:\\Program Files\\AetherSerialBridge")));
        QString why;
        QVERIFY(!win::windowsPathsFrom(serviceLocations(true, [] { return QString(); }), QStringLiteral("C:\\x"), &why));
        QCOMPARE(why, QString::fromLatin1(kWindowsDataFolderError));
        QVERIFY(!win::windowsPathsFrom(serviceLocations(), std::nullopt, &why));
        QVERIFY(why.contains(QLatin1String("Program Files")));

        QVERIFY(runningWith({}));
        QVERIFY(svc::stop(p()).outcome == Outcome::Ok);
        for (const auto& [which, code] : {std::pair{QStringLiteral("ProgramData"), 3u}, std::pair{QStringLiteral("ProgramFiles"), 4u}}) {
            QVERIFY(svc::setServiceEnvironment(seams({QStringLiteral("ASB_TEST_FAIL_KNOWN_FOLDER=") + which})));
            QCOMPARE(svc::startRaw(), 0u);
            svc::ScmState s;
            QVERIFY(svc::waitState(SERVICE_STOPPED, 15000, &s));
            QCOMPARE(s.win32ExitCode, 1066u);
            QCOMPARE(s.specificExitCode, code);
        }
    }

    // A service this program did not install, under its service name, refuses install (Foreign); nothing of it is
    // changed.
    void foreign_service_refused()
    {
        const QString dir = native::scratch(QStringLiteral("foreign"));
        const QString exe = dir + QStringLiteral("\\other-bridge.exe");
        const QByteArray image(4096, 'x');
        QVERIFY(native::writeFile(exe, image));
        SC_HANDLE m = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
        const std::wstring bin = (QStringLiteral("\"%1\" --service").arg(exe)).toStdWString();
        SC_HANDLE h = CreateServiceW(m, L"AetherSerialBridge", L"Aether Serial Bridge (foreign fixture)", SERVICE_ALL_ACCESS,
                                     SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, bin.c_str(),
                                     nullptr, nullptr, nullptr, nullptr, nullptr);
        QVERIFY(h);
        CloseServiceHandle(h);
        CloseServiceHandle(m);
        const Result r = svc::install(p(), svc::configJson(kPort));
        QCOMPARE(r.outcome, Outcome::NothingChanged);
        QVERIFY2(r.message.contains(QStringLiteral("not one this program installed")) && r.message.contains(exe),
                 qPrintable(r.message));
        QVERIFY(native::readFile(exe) == std::optional<QByteArray>(image));
        svc::removeServiceHard(p().installedExe());
        native::removeTree(dir);
    }
};

QTEST_GUILESS_MAIN(tst_ServiceNative)
#include "tst_ServiceNative.moc"
