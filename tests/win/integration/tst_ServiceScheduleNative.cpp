// The deferred native gates WS-5 and WS-6 hand to WS-7, on windows-latest with the real service, SCM and WMI
// provider (labels as in the WS-5 / WS-6 docs):
//
//  INV-119  blocked_connect_server_exits_7, blocked_enumeration_exits_7, slow_filter_reads_exceed_aggregate_budget,
//           monitor_independent_of_blocked_side_channel (+ sustained_traffic_check_on_time: echo traffic all along),
//           late_enable_kills_within_35s, exit7_retry_loop_then_repair
//  INV-92   kill9_engine_releases_lock, stopped_before_exit_lock_wait,
//           service_started_while_applier_holds_lock_fails_closed, incomplete_install_lock_bootstrap,
//           deadline_state_unknown_no_r3
//  INV-104  engine_lock_dacl_validated (the installed service refuses a widened engine.lock: 4; Start repairs it)
//  INV-113  paused_launch_after_interrupted_apply, untick_deletes_token, first_install_token_only_after_commit,
//           crash_after_token_before_start_recovery_ok
//  INV-63   start_token_link_refused, config_json_directory_untrusted
//  INV-41   swapped_headless_exe_refused
//  WS-5     orphaned_status_temp_swept_at_start (the status temps P5 leaves to the service)
//  INV-91   save_and_remove_first_operation_connects (WS-5 round 4)
//  INV-114  hung_provider_keeps_apply_lock (a CIM call blocked in the provider past the 60 s firewall deadline:
//           apply.lock and engine.lock stay held until it returns)
//  WS-6     cim_works_under_job: every running row here has the service under its job guard with the CIM monitor
//           working; the guard row is tst_ProcessGuardNative.

#include "ServiceNativeSupport.h"
#include "WinNativeSupport.h"
#include "support/Fakes.h"

#include "core/OperationHost.h"
#include "platform/win/CimFirewall.h"
#include "platform/win/EngineLock.h"
#include "platform/win/FileApplyLock.h"
#include "platform/win/NativeSeams.h"
#include "platform/win/WbemSession.h"
#include "platform/win/WindowsService.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QSet>
#include <QTcpSocket>
#include <QTest>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace asb;
using namespace asb::test;
using namespace std::chrono;

namespace {
constexpr int kPort = 47031;

QString eventPrefix()
{
    static const QString p = QStringLiteral("Global\\asbsched-%1").arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
    return p;
}
void release(const QString& suffix)
{
    const QString name = eventPrefix() + QLatin1Char('-') + suffix;
    HANDLE e = CreateEventW(nullptr, TRUE, FALSE, reinterpret_cast<const wchar_t*>(name.utf16()));
    if (e) {
        SetEvent(e);
        CloseHandle(e);
    }
}
QStringList withSeams(QStringList kv)
{
    kv << QStringLiteral("ASB_TEST_EVENT_PREFIX=") + eventPrefix() << QStringLiteral("ASB_TEST_DEVICE=echo");
    return kv;
}

win::cim::Props ruleProps(const QString& id, qint64 enabled)
{
    win::cim::Props r;
    r[QStringLiteral("InstanceID")] = win::cim::Value::string(id);
    r[QStringLiteral("ElementName")] = win::cim::Value::string(id);
    r[QStringLiteral("RuleGroup")] = win::cim::Value::string(QStringLiteral("Aether Serial Bridge"));
    r[QStringLiteral("Direction")] = win::cim::Value::integer(1);
    r[QStringLiteral("Action")] = win::cim::Value::integer(2);
    r[QStringLiteral("Enabled")] = win::cim::Value::integer(enabled);
    return r;
}

void removeRule(win::WbemSession& cim, const QString& id)
{
    for (const auto& i : cim.query(win::groupQuery(), win::cim::Store::Persistent))
        if (i.props.value(QStringLiteral("InstanceID")).s == id) cim.remove(i.relPath, win::cim::Store::Persistent);
}

// A firewall whose sync blocks until released (the Applier stays inside the "provider").
struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool entered = false, open = false;
    void enter()
    {
        std::unique_lock lk(m);
        entered = true;
        cv.notify_all();
        cv.wait(lk, [&] { return open; });
    }
    bool waitEntered(int ms)
    {
        std::unique_lock lk(m);
        return cv.wait_for(lk, milliseconds(ms), [&] { return entered; });
    }
    void release()
    {
        std::lock_guard g(m);
        open = true;
        cv.notify_all();
    }
};

class BlockingSync final : public IFirewall {
public:
    BlockingSync(IFirewall& real, Gate& gate) : m_real(real), m_gate(gate) {}
    void connect(Deadline d) override { m_real.connect(d); }
    void sync(const BridgeConfig& c, const FirewallGeneration& g, Deadline d) override
    {
        m_gate.enter();
        m_real.sync(c, g, d);
    }
    void rollback(const FirewallGeneration& g, Deadline d) override { m_real.rollback(g, d); }
    void sweep(const FirewallGeneration& g, Deadline d) override { m_real.sweep(g, d); }
    void removeAll(Deadline d) override { m_real.removeAll(d); }

private:
    IFirewall& m_real;
    Gate& m_gate;
};

class FailingSync final : public IFirewall {
public:
    explicit FailingSync(IFirewall& real) : m_real(real) {}
    void connect(Deadline d) override { m_real.connect(d); }
    void sync(const BridgeConfig&, const FirewallGeneration&, Deadline) override { throw PlatformError("injected sync failure"); }
    void rollback(const FirewallGeneration& g, Deadline d) override { m_real.rollback(g, d); }
    void sweep(const FirewallGeneration& g, Deadline d) override { m_real.sweep(g, d); }
    void removeAll(Deadline d) override { m_real.removeAll(d); }

private:
    IFirewall& m_real;
};

// A CIM session whose puts block until released (the BlockingWbemServices seam, around the real provider).
class BlockingPuts final : public win::ICimSession {
public:
    BlockingPuts(win::ICimSession& real, Gate& gate) : m_real(real), m_gate(gate) {}
    void connect() override { m_real.connect(); }
    QList<win::cim::Instance> query(const QString& w, win::cim::Store s) override { return m_real.query(w, s); }
    void put(const QString& c, const QString& p, const win::cim::Props& v, win::cim::PutFlag f, win::cim::Store s,
             const std::function<void()>& beforeWrite = {}) override
    {
        m_gate.enter();
        m_real.put(c, p, v, f, s, beforeWrite);
    }
    void remove(const QString& p, win::cim::Store s) override { m_real.remove(p, s); }

private:
    win::ICimSession& m_real;
    Gate& m_gate;
};

struct Observer : IOperationObserver {
    std::atomic<int> interims{0};
    std::atomic<bool> done{false};
    Result final;
    void interim(const Interim&) override { ++interims; }
    void finished(const Result& r) override
    {
        final = r;
        done = true;
    }
};
}  // namespace

class tst_ServiceScheduleNative : public QObject {
    Q_OBJECT
    std::unique_ptr<win::WindowsPlatform> m_p;
    bool m_hadInstallFolders = false;   // the install or data folder existed before the first row
    win::WindowsPlatform& p() { return *m_p; }

    // Installed and running (real device factory replaced by the echo seam, plus `kv`), ACK'ed.
    quint32 running(const QStringList& kv = {})
    {
        Result r = svc::install(p(), svc::configJson(kPort, QStringLiteral("127.0.0.1")));
        if (r.outcome != Outcome::Ok) {
            qWarning("install: %s", qPrintable(svc::describe(r)));
            return 0;
        }
        r = svc::stop(p());
        svc::setServiceEnvironment(withSeams(kv));
        r = svc::start(p());
        if (r.outcome != Outcome::Ok) {
            qWarning("start: %s", qPrintable(svc::describe(r)));
            return 0;
        }
        return svc::query().pid;
    }
    // Restarts the installed service through SCM only (the token stays) with `kv`; the new pid (0 if none came).
    quint32 rawRestart(const QStringList& kv)
    {
        svc::setServiceEnvironment(withSeams(kv));
        if (svc::query().state != SERVICE_STOPPED) {
            svc::stopSync();
            svc::waitState(SERVICE_STOPPED, 25000);
        }
        if (svc::startRaw() != 0) return 0;
        QElapsedTimer t;
        t.start();
        quint32 pid = 0;
        while (!pid && t.elapsed() < 10000) {
            pid = svc::query().pid;
            Sleep(5);
        }
        return pid;
    }
    void cleanSlate()
    {
        release(QStringLiteral("cim"));
        release(QStringLiteral("sink"));
        if (svc::query().exists) {
            svc::setServiceEnvironment({});
            svc::uninstall(p());
        }
        if (!svc::removeServiceHard(p().installedExe())) qWarning("the service is still not deleted after 60 s");
    }

private slots:
    void initTestCase()
    {
        QString err;
        m_p = svc::platform(&err);
        QVERIFY2(m_p, qPrintable(err));
        m_hadInstallFolders = QFileInfo::exists(p().lockDirectory()) || QFileInfo::exists(p().dataFolder());
        svc::removeServiceHard(p().installedExe());
        svc::deployDevRuntime(p().lockDirectory());
    }
    // The install and data folders the rows created (the service, the dev runtime, the lock pair, the config) are
    // removed at the end, unless they were there before the test began (a persistent runner keeps nothing).
    void cleanupTestCase()
    {
        if (!m_p || m_hadInstallFolders) return;
        native::removeTree(p().lockDirectory());
        native::removeTree(p().dataFolder());
    }
    void init() { cleanSlate(); }
    void cleanup() { cleanSlate(); }

    // ---------------------------------------------------------------- INV-119 schedules
    void blocked_connect_server_exits_7()
    {
        QVERIFY(running());
        const quint32 pid = rawRestart({QStringLiteral("ASB_TEST_CIM_BLOCK=connect")});
        QVERIFY(pid);
        void* proc = svc::openProcess(pid);
        bool listened = false;
        QElapsedTimer t;
        t.start();
        quint32 code = 0;
        while (!svc::waitExit(proc, 50, nullptr, &code) && t.elapsed() < 15000)
            if (svc::canConnect(kPort, 20)) listened = true;
        svc::closeHandle(proc);
        QVERIFY2(t.elapsed() < 9000, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(code, 7u);
        QVERIFY(!listened);
    }

    void slow_filter_reads_exceed_aggregate_budget()
    {
        QVERIFY(running());
        // One rule = four association reads per check; 1.6 s each = 6.4 s: every read is quick enough for a per-call
        // timeout, the check as a whole is not (5 s aggregate budget).
        const quint32 pid = rawRestart({QStringLiteral("ASB_TEST_CIM_BLOCK=assoc-delay:1600")});
        QVERIFY(pid);
        void* proc = svc::openProcess(pid);
        bool listened = false;
        QElapsedTimer t;
        t.start();
        quint32 code = 0;
        while (!svc::waitExit(proc, 50, nullptr, &code) && t.elapsed() < 20000)
            if (svc::canConnect(kPort, 20)) listened = true;
        svc::closeHandle(proc);
        QCOMPARE(code, 7u);
        QVERIFY(!listened);
    }

    void blocked_enumeration_exits_7()
    {
        QVERIFY(running());
        const quint32 pid = rawRestart({QStringLiteral("ASB_TEST_CIM_BLOCK=group-query")});   // the first check passes
        QVERIFY(pid);
        QVERIFY(svc::waitState(SERVICE_RUNNING, 10000));
        QElapsedTimer sinceRunning;
        sinceRunning.start();
        QVERIFY(svc::canConnect(kPort, 3000));
        void* proc = svc::openProcess(pid);
        quint32 code = 0;
        QVERIFY(svc::waitExit(proc, 60000, nullptr, &code));
        svc::closeHandle(proc);
        QCOMPARE(code, 7u);
        // The periodic check at +30 s hangs; the watchdog fires at its start + 5 s.
        QVERIFY2(sinceRunning.elapsed() >= 28000 && sinceRunning.elapsed() <= 37000, qPrintable(QString::number(sinceRunning.elapsed())));
    }

    // The side channel's sink blocks forever; the monitor keeps checking on time (two periodic checks pass) and
    // bytes keep flowing (sustained_traffic_check_on_time): the engine is still up after 70 s.
    void monitor_independent_of_blocked_side_channel()
    {
        const quint32 pid = running({QStringLiteral("ASB_TEST_BLOCK_SINK=1")});
        QVERIFY(pid);
        void* proc = svc::openProcess(pid);
        QTcpSocket c;
        c.connectToHost(QStringLiteral("127.0.0.1"), kPort);
        QVERIFY(c.waitForConnected(3000));
        QElapsedTimer t;
        t.start();
        qint64 echoed = 0;
        while (t.elapsed() < 70000) {
            c.write(QByteArray(512, 'x'));
            c.waitForBytesWritten(200);
            if (c.waitForReadyRead(50)) echoed += c.readAll().size();
            QVERIFY2(!svc::waitExit(proc, 0), "the engine died while the side channel was blocked");
        }
        svc::closeHandle(proc);
        QVERIFY(echoed > 0);
    }

    // An administrator enables a rule in the reserved group while the engine runs: dead within 35 s, the
    // listener closed.
    void late_enable_kills_within_35s()
    {
        const quint32 pid = running();
        QVERIFY(pid);
        win::WbemSession cim;
        cim.connect();
        const QString id = QStringLiteral("asb-test-late-enable");
        cim.put(QStringLiteral("MSFT_NetFirewallRule"), {}, ruleProps(id, 2), win::cim::PutFlag::CreateOnly, win::cim::Store::Persistent);
        Sleep(2000);
        void* proc = svc::openProcess(pid);
        QVERIFY(!svc::waitExit(proc, 0));   // a disabled rule is no exposure
        for (const auto& i : cim.query(win::groupQuery(), win::cim::Store::Persistent))
            if (i.props.value(QStringLiteral("InstanceID")).s == id) {
                win::cim::Props e;
                e[QStringLiteral("Enabled")] = win::cim::Value::integer(1);
                cim.put({}, i.relPath, e, win::cim::PutFlag::UpdateOnly, win::cim::Store::Persistent);
            }
        milliseconds took{};
        quint32 code = 0;
        const bool died = svc::waitExit(proc, 45000, &took, &code);
        svc::closeHandle(proc);
        const bool listening = svc::canConnect(kPort, 200);
        removeRule(cim, id);
        QVERIFY2(died, "alive 45 s after the enable");
        QCOMPARE(code, 7u);
        QVERIFY(took <= seconds(36));
        QVERIFY(!listening);
    }

    // A stray enabled rule makes every SCM restart exit 7 (availability only); an apply sweeps it and the engine
    // then runs (exit7_retry_loop_then_repair), with a blocked diagnostic sink all along.
    void exit7_retry_loop_then_repair()
    {
        QVERIFY(running());
        win::WbemSession cim;
        cim.connect();
        const QString id = QStringLiteral("asb-test-retry-loop");
        cim.put(QStringLiteral("MSFT_NetFirewallRule"), {}, ruleProps(id, 1), win::cim::PutFlag::CreateOnly, win::cim::Store::Persistent);
        const quint32 first = rawRestart({QStringLiteral("ASB_TEST_BLOCK_SINK=1")});
        QVERIFY(first);
        QSet<quint32> exited7;
        QElapsedTimer t;
        t.start();
        quint32 pid = first;
        while (exited7.size() < 2 && t.elapsed() < 40000) {
            void* proc = svc::openProcess(pid);
            quint32 code = 0;
            if (svc::waitExit(proc, 15000, nullptr, &code) && code == 7) exited7.insert(pid);
            svc::closeHandle(proc);
            for (int i = 0; i < 200; ++i) {   // SCM's restart (5 s)
                const quint32 next = svc::query().pid;
                if (next && next != pid) {
                    pid = next;
                    break;
                }
                Sleep(50);
            }
        }
        QVERIFY2(exited7.size() >= 2, "SCM did not keep restarting the engine into exit 7");
        const Result r = svc::apply(p(), svc::configJson(kPort, QStringLiteral("127.0.0.1")));
        removeRule(cim, id);   // (already swept by the apply's R9)
        QVERIFY2(r.outcome == Outcome::Ok, qPrintable(svc::describe(r)));
        const quint32 healthy = svc::query().pid;
        void* proc = svc::openProcess(healthy);
        QVERIFY(!svc::waitExit(proc, 35000));   // past a periodic check
        svc::closeHandle(proc);
    }

    // ---------------------------------------------------------------- INV-92
    void kill9_engine_releases_lock()
    {
        const quint32 pid = running();
        QVERIFY(pid);
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        QVERIFY(h);
        TerminateProcess(h, 9);
        WaitForSingleObject(h, 5000);
        CloseHandle(h);
        win::EngineLock other(win::nativeSeams(), p().lockDirectory(), win::LockRole::Applier);
        LockResult why = LockResult::Busy;
        QVERIFY(other.tryAcquire(&why));   // SCM's restart comes 5 s later
    }

    // The Applier's Stop proves the stop by the lock, which only process exit releases: Stop returns after the
    // process is gone, however long the engine takes between its STOPPED report and its exit.
    void stopped_before_exit_lock_wait()
    {
        const quint32 pid = running({QStringLiteral("ASB_TEST_PAUSE_BEFORE_EXIT_MS=5000")});
        QVERIFY(pid);
        void* proc = svc::openProcess(pid);
        QElapsedTimer t;
        t.start();
        const Result r = svc::stop(p());
        QVERIFY2(r.outcome == Outcome::Ok, qPrintable(svc::describe(r)));
        QVERIFY(svc::waitExit(proc, 0));   // gone by the time Stop returned
        svc::closeHandle(proc);
        QVERIFY(t.elapsed() >= 4000);
    }

    // A service started (by SCM) while an Applier is inside its transaction cannot run: the engine lock is held
    // (and the token revoked), so it exits 4 and never listens.
    void service_started_while_applier_holds_lock_fails_closed()
    {
        QVERIFY(running());
        Gate gate;
        Platform& real = p().platform();
        BlockingSync blocking(real.firewall, gate);
        Platform pf{real.folder, real.service, blocking, real.applyLock, real.engineLock, real.token, real.status, real.io};
        Result applied;
        std::thread applier([&] { applied = Applier(pf).run(svc::request(svc::configJson(kPort, QStringLiteral("127.0.0.1"))), false); });
        QVERIFY(gate.waitEntered(60000));
        // Starts are blocked (disabled) by B1: enable it behind the Applier's back, as an administrator could.
        win::ScmSeams::Handle h = 0;
        auto& scm = win::nativeScmSeams();
        QCOMPARE(scm.openService(QStringLiteral("AetherSerialBridge"), SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_QUERY_STATUS, &h), 0u);
        QCOMPARE(scm.changeStartType(h, SERVICE_DEMAND_START), 0u);
        QCOMPARE(scm.start(h), 0u);
        scm.close(h);
        svc::ScmState s;
        QVERIFY(svc::waitState(SERVICE_STOPPED, 15000, &s));
        QCOMPARE(s.win32ExitCode, 1066u);
        QCOMPARE(s.specificExitCode, 4u);
        QVERIFY(!svc::canConnect(kPort, 200));
        gate.release();
        applier.join();
        QVERIFY2(applied.outcome == Outcome::Ok, qPrintable(svc::describe(applied)));
    }

    // INV-92 (WS-5 deferred, deadline_state_unknown_no_r3): the barrier cannot prove the stop within its 30 s and
    // the operation ends there. An old engine (asb_win_lock_helper in the service's lock role, a real share-0 hold)
    // keeps engine.lock while SCM reports the service stopped: B3 is Busy until the deadline, so the apply is
    // StateUnknown at "prove the bridge stopped" with nothing from R3 on (config.json unchanged, nothing listens,
    // no stop claimed). Once the holder exits, the same apply goes through.
    void deadline_state_unknown_no_r3()
    {
        QVERIFY(running());
        QVERIFY(svc::stop(p()).outcome == Outcome::Ok);
        const QString config = p().dataFolder() + QStringLiteral("\\config.json");
        const std::optional<QByteArray> before = native::readFile(config);
        QVERIFY(before);
        QProcess holder;
        holder.start(qEnvironmentVariable("ASB_WIN_LOCK_HELPER"), {QStringLiteral("hold-engine"), p().lockDirectory()});
        QVERIFY(holder.waitForStarted(10000));
        for (int i = 0; i < 300 && !holder.canReadLine(); ++i) holder.waitForReadyRead(100);
        QCOMPARE(holder.readLine().trimmed(), QByteArray("held"));
        constexpr int kOther = kPort + 1;
        const Result r = svc::apply(p(), svc::configJson(kOther, QStringLiteral("127.0.0.1")));
        QVERIFY2(r.outcome == Outcome::StateUnknown, qPrintable(svc::describe(r)));
        QCOMPARE(r.step, QStringLiteral("prove the bridge stopped"));
        QCOMPARE(r.persisted, Persisted::None);
        QVERIFY2(r.message.contains(QLatin1String("within 30 s")), qPrintable(r.message));
        QVERIFY2(!claimsStop(r.message) && !claimsStop(r.postcondition), qPrintable(svc::describe(r)));
        QCOMPARE(native::readFile(config).value_or("<missing>"), *before);   // R3 onwards never ran: no staging, no commit
        QVERIFY(!svc::canConnect(kOther, 200));
        QVERIFY(!svc::canConnect(kPort, 200));
        holder.kill();
        QVERIFY(holder.waitForFinished(10000));
        const Result again = svc::apply(p(), svc::configJson(kOther, QStringLiteral("127.0.0.1")));
        QVERIFY2(again.outcome == Outcome::Ok, qPrintable(svc::describe(again)));
        QVERIFY(svc::canConnect(kOther, 5000));
    }

    // IncompleteInstall (engine.lock.id missing): Apply refuses ("finish the install"); --install bootstraps the
    // record from the lock it holds and succeeds.
    void incomplete_install_lock_bootstrap()
    {
        QVERIFY(running());
        QVERIFY(svc::stop(p()).outcome == Outcome::Ok);
        const QString record = p().lockDirectory() + QStringLiteral("\\engine.lock.id");
        QVERIFY(DeleteFileW(reinterpret_cast<const wchar_t*>(record.utf16())));
        QCOMPARE(p().service().detect(steady_clock::now() + seconds(10)).state, InstallState::IncompleteInstall);
        const Result a = svc::apply(p(), svc::configJson(kPort, QStringLiteral("127.0.0.1")));
        QCOMPARE(a.outcome, Outcome::NothingChanged);
        const Result i = svc::install(p(), svc::configJson(kPort, QStringLiteral("127.0.0.1")));
        QVERIFY2(i.outcome == Outcome::Ok, qPrintable(svc::describe(i)));
        QVERIFY(QFileInfo::exists(record));
    }

    // ---------------------------------------------------------------- INV-104
    void engine_lock_dacl_validated()
    {
        QVERIFY(running());
        QCOMPARE(svc::stopSync(), 0u);
        QVERIFY(svc::waitState(SERVICE_STOPPED, 15000));
        const QString lock = p().lockDirectory() + QStringLiteral("\\engine.lock");
        const QString before = native::sddlOf(lock, DACL_SECURITY_INFORMATION);
        QVERIFY(native::setSddl(lock, QStringLiteral("D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;BU)"), DACL_SECURITY_INFORMATION));
        QCOMPARE(svc::startRaw(), 0u);
        svc::ScmState s;
        QVERIFY(svc::waitState(SERVICE_STOPPED, 15000, &s));
        QCOMPARE(s.specificExitCode, 4u);
        const Result r = svc::start(p());   // the Applier repairs the lock's DACL and starts
        QVERIFY2(r.outcome == Outcome::Ok, qPrintable(svc::describe(r)));
        QVERIFY2(!native::sddlOf(lock, DACL_SECURITY_INFORMATION).contains(QLatin1String(";BU)")), qPrintable(before));
    }

    // ---------------------------------------------------------------- INV-113
    // A launch SCM accepted before an apply (held in START_PENDING before the engine lock) reaches its lock after
    // the apply revoked the token: it exits 6; the apply then starts a fresh engine.
    void paused_launch_after_interrupted_apply()
    {
        QVERIFY(running());
        const quint32 paused = rawRestart({QStringLiteral("ASB_TEST_HOLD_START_MS=8000")});
        QVERIFY(paused);
        void* proc = svc::openProcess(paused);
        svc::setServiceEnvironment(withSeams({}));
        const Result r = svc::apply(p(), svc::configJson(kPort, QStringLiteral("127.0.0.1")));
        quint32 code = 0;
        QVERIFY(svc::waitExit(proc, 30000, nullptr, &code));
        svc::closeHandle(proc);
        QCOMPARE(code, 6u);
        QVERIFY2(r.outcome == Outcome::Ok, qPrintable(svc::describe(r)));
        QVERIFY(svc::query().pid != paused);
    }

    void untick_deletes_token()
    {
        QVERIFY(running());
        const QString token = p().dataFolder() + QStringLiteral("\\start.token");
        QVERIFY(QFileInfo::exists(token));
        const Result r = svc::uninstall(p());
        QVERIFY2(r.outcome == Outcome::Ok || r.outcome == Outcome::SavedNotApplied, qPrintable(svc::describe(r)));
        QVERIFY(!QFileInfo::exists(token));
    }

    // A first install whose firewall sync fails writes no token and commits nothing.
    void first_install_token_only_after_commit()
    {
        svc::removeServiceHard(p().installedExe());
        const QString folder = p().dataFolder();
        QFile::remove(folder + QStringLiteral("\\start.token"));
        QFile::remove(folder + QStringLiteral("\\config.json"));
        Platform& real = p().platform();
        FailingSync failing(real.firewall);
        Platform pf{real.folder, real.service, failing, real.applyLock, real.engineLock, real.token, real.status, real.io};
        const Result r = Applier(pf).run(svc::request(svc::configJson(kPort)), /*install=*/true);
        QCOMPARE(r.outcome, Outcome::Uncommitted);
        QVERIFY(!QFileInfo::exists(folder + QStringLiteral("\\start.token")));
        QVERIFY(!QFileInfo::exists(folder + QStringLiteral("\\config.json")));
    }

    // After R9's token write the token, the config and the rules agree: a later start (SCM recovery, a reboot)
    // runs, correctly (crash_after_token_before_start_recovery_ok).
    void crash_after_token_before_start_recovery_ok()
    {
        QVERIFY(running());
        QCOMPARE(svc::stopSync(), 0u);   // SCM only: the token stays (as after a crash between R9's token and start)
        QVERIFY(svc::waitState(SERVICE_STOPPED, 15000));
        QCOMPARE(svc::startRaw(), 0u);
        QVERIFY(svc::waitState(SERVICE_RUNNING, 15000));
        QString why;
        QVERIFY2(svc::echoRoundTrip(kPort, QByteArrayLiteral("FA;"), 5000, &why), qPrintable(why));
    }

    // INV-91 (WS-5 round 4): SaveAndRemove as the FIRST operation of a fresh process connects to the firewall
    // provider before removeAll: the rules are gone, then the service.
    void save_and_remove_first_operation_connects()
    {
        QVERIFY(running());
        QString err;
        auto fresh = svc::platform(&err);   // a new process's Platform: nothing connected yet
        QVERIFY2(fresh, qPrintable(err));
        const Result r = svc::uninstall(*fresh);
        QVERIFY2(r.outcome == Outcome::Ok || r.outcome == Outcome::SavedNotApplied, qPrintable(svc::describe(r)));
        QVERIFY2(svc::waitDeleted(60000), "SCM did not delete the service within 60 s");
        win::WbemSession cim;
        cim.connect();
        QVERIFY(cim.query(win::groupQuery(), win::cim::Store::Persistent).isEmpty());
    }

    // WS-5 round 1 hand-off: P5 no longer removes status.json temps; the service removes the ones a killed engine
    // left, with its first status write (after trust passed).
    void orphaned_status_temp_swept_at_start()
    {
        QVERIFY(running());
        QCOMPARE(svc::stopSync(), 0u);   // the token stays
        QVERIFY(svc::waitState(SERVICE_STOPPED, 15000));
        const QString orphan = p().dataFolder() + QStringLiteral("\\status.json.0123456789abcdef0123456789abcdef.asbtmp");
        QVERIFY(native::writeFile(orphan, QByteArrayLiteral("{\"half\": ")));
        QVERIFY(QFileInfo::exists(orphan));
        QCOMPARE(svc::startRaw(), 0u);
        QVERIFY(svc::waitState(SERVICE_RUNNING, 15000));
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(orphan), 10000);
        QVERIFY(QFileInfo::exists(p().dataFolder() + QStringLiteral("\\status.json")));
    }

    // INV-41 swapped_headless_exe_refused: an install whose copy does not hash to this build's digest is refused —
    // no service, nothing at the installed path; and an installed exe swapped afterwards makes the installation
    // incomplete (Start/apply refuse until --install puts this build back).
    void swapped_headless_exe_refused()
    {
        svc::removeServiceHard(p().installedExe());
        DeleteFileW(reinterpret_cast<const wchar_t*>(p().installedExe().utf16()));
        QVERIFY(!QFileInfo::exists(p().installedExe()));
        QString err;
        const std::optional<QByteArray> image = win::readWholeFile(svc::headlessExe(), &err);
        QVERIFY2(image, qPrintable(err));
        win::WindowsPlatform::Options o;
        o.expectedHeadlessSha256 = QByteArray(64, 'a');   // not this image's digest
        o.image = [image](QString*) { return image; };
        auto wrong = win::WindowsPlatform::create(std::move(o), &err);
        QVERIFY2(wrong, qPrintable(err));
        const Result r = svc::install(*wrong, svc::configJson(kPort, QStringLiteral("127.0.0.1")));
        QVERIFY2(r.outcome != Outcome::Ok, qPrintable(svc::describe(r)));
        QVERIFY2(svc::waitDeleted(60000), "SCM did not delete the service within 60 s");
        QVERIFY(!QFileInfo::exists(p().installedExe()));

        QVERIFY(running());   // a good install
        QVERIFY(svc::stop(p()).outcome == Outcome::Ok);
        svc::killInstalledProcesses(p().installedExe());
        QFile f(p().installedExe());
        QVERIFY(f.open(QIODevice::Append));
        f.write("swapped");
        f.close();
        QCOMPARE(p().service().detect(steady_clock::now() + seconds(10)).state, InstallState::IncompleteInstall);
        QCOMPARE(svc::apply(p(), svc::configJson(kPort, QStringLiteral("127.0.0.1"))).outcome, Outcome::NothingChanged);
        QVERIFY(svc::install(p(), svc::configJson(kPort, QStringLiteral("127.0.0.1"))).outcome == Outcome::Ok);
        QCOMPARE(p().service().detect(steady_clock::now() + seconds(10)).state, InstallState::Current);
    }

    // ---------------------------------------------------------------- INV-63
    void start_token_link_refused()
    {
        QVERIFY(running());
        QCOMPARE(svc::stopSync(), 0u);
        QVERIFY(svc::waitState(SERVICE_STOPPED, 15000));
        const QString folder = p().dataFolder();
        const QString token = folder + QStringLiteral("\\start.token");
        const QString scratchDir = native::scratch(QStringLiteral("token"));
        struct RemoveAtExit {
            QString dir;
            ~RemoveAtExit() { native::removeTree(dir); }
        } removeScratch{scratchDir};   // also on a failed QVERIFY: the runner keeps nothing
        const QString elsewhere = scratchDir + QStringLiteral("\\start.token");
        QVERIFY(CopyFileW(reinterpret_cast<const wchar_t*>(token.utf16()), reinterpret_cast<const wchar_t*>(elsewhere.utf16()), FALSE));
        QVERIFY(DeleteFileW(reinterpret_cast<const wchar_t*>(token.utf16())));
        QVERIFY(native::makeSymlink(token, elsewhere, false));
        QCOMPARE(svc::startRaw(), 0u);
        svc::ScmState s;
        QVERIFY(svc::waitState(SERVICE_STOPPED, 15000, &s));
        DeleteFileW(reinterpret_cast<const wchar_t*>(token.utf16()));
        QCOMPARE(s.win32ExitCode, 1066u);
        QVERIFY2(s.specificExitCode == 3 || s.specificExitCode == 6, qPrintable(QString::number(s.specificExitCode)));
        QVERIFY(!svc::canConnect(kPort, 200));
    }

    void config_json_directory_untrusted()
    {
        QVERIFY(running());
        QCOMPARE(svc::stopSync(), 0u);
        QVERIFY(svc::waitState(SERVICE_STOPPED, 15000));
        const QString cfg = p().dataFolder() + QStringLiteral("\\config.json");
        const QString aside = cfg + QStringLiteral(".ws7-aside");
        QVERIFY(MoveFileW(reinterpret_cast<const wchar_t*>(cfg.utf16()), reinterpret_cast<const wchar_t*>(aside.utf16())));
        QVERIFY(CreateDirectoryW(reinterpret_cast<const wchar_t*>(cfg.utf16()), nullptr));
        QCOMPARE(svc::startRaw(), 0u);
        svc::ScmState s;
        QVERIFY(svc::waitState(SERVICE_STOPPED, 15000, &s));
        RemoveDirectoryW(reinterpret_cast<const wchar_t*>(cfg.utf16()));
        MoveFileW(reinterpret_cast<const wchar_t*>(aside.utf16()), reinterpret_cast<const wchar_t*>(cfg.utf16()));
        QCOMPARE(s.specificExitCode, 3u);
    }

    // ---------------------------------------------------------------- INV-114
    // A CIM call held inside the provider (the BlockingWbemServices seam around the real provider) far past the
    // 60 s firewall deadline: an interim notice, but apply.lock and engine.lock stay held until it returns; then
    // only failClosed runs and the final result is StateUnknown.
    void hung_provider_keeps_apply_lock()
    {
        QVERIFY(running());
        Gate gate;
        win::WbemSession real;
        BlockingPuts blocking(real, gate);
        win::CimFirewall firewall(blocking, p().installedExe());
        Platform& rp = p().platform();
        Platform pf{rp.folder, rp.service, firewall, rp.applyLock, rp.engineLock, rp.token, rp.status, rp.io};
        OperationHost host(pf);
        Observer o;
        QVERIFY(host.submitRun(svc::request(svc::configJson(kPort, QStringLiteral("127.0.0.1"))), false, false, o));
        QVERIFY(gate.waitEntered(60000));
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 65000) QTest::qWait(500);   // past the firewall phase deadline
        QVERIFY(o.interims.load() >= 1);
        QVERIFY(!o.done.load());
        win::FileApplyLock second(win::nativeSeams(), p().lockDirectory());
        QVERIFY(!second.acquire(seconds(1)));   // a second Applier cannot get in
        win::EngineLock engine(win::nativeSeams(), p().lockDirectory(), win::LockRole::Service);
        LockResult why = LockResult::Acquired;
        QVERIFY(!engine.tryAcquire(&why));       // nor an engine
        gate.release();
        QTRY_VERIFY_WITH_TIMEOUT(o.done.load(), 60000);
        QCOMPARE(o.final.outcome, Outcome::StateUnknown);
    }
};

QTEST_GUILESS_MAIN(tst_ServiceScheduleNative)
#include "tst_ServiceScheduleNative.moc"
