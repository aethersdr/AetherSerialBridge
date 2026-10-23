// BridgeRunner, the managed engine's body (S15, S18, D52, D56):
// engine.lock and its identity before anything is read (INV-103), Service-mode trust (INV-22),
// the start token before any config read (INV-113), config.json only (INV-43), invalid config ->
// zero ports (INV-34), status failures never stop bridging (INV-8), and the firewall monitor and
// watchdog (INV-119) on a fake clock, a fake monitor and a fake watchdog.

#include "core/BridgeRunner.h"
#include "core/ConfigFiles.h"
#include "core/ConfigJson.h"
#include "core/StartToken.h"
#include "support/Fakes.h"

#include <QTest>

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

using namespace asb;
using namespace asb::test;
using namespace std::chrono_literals;

namespace {

const QString kConfig = QString::fromLatin1(ConfigFiles::kConfig);
const QString kPending = QString::fromLatin1(ConfigFiles::kPending);
const QString kToken = QString::fromLatin1(ConfigFiles::kStartToken);
const FirewallGeneration kGen{QByteArrayLiteral("0123456789abcdef")};
const QByteArray kTokenValue = QByteArrayLiteral("cccccccccccccccccccccccccccccccc");

// The monitor's sleeps, driven by the fake clock: a sleeping monitor wakes when the test has moved the
// clock past its deadline and poked, or when the test shuts it down.
class Sleeper {
public:
    explicit Sleeper(FakeClock& c) : m_clock(c) {}
    bool sleepUntil(Deadline d)
    {
        std::unique_lock lk(m_m);
        ++m_sleeping;
        m_cv.notify_all();
        m_cv.wait(lk, [&] { return m_stop || m_clock.now() >= d; });
        --m_sleeping;
        return !m_stop;
    }
    void poke() { m_cv.notify_all(); }
    void shutdown()
    {
        std::lock_guard g(m_m);
        m_stop = true;
        m_cv.notify_all();
    }
    bool waitSleeping()
    {
        std::unique_lock lk(m_m);
        return m_cv.wait_for(lk, 5s, [&] { return m_sleeping > 0; });
    }

private:
    FakeClock& m_clock;
    std::mutex m_m;
    std::condition_variable m_cv;
    int m_sleeping = 0;
    bool m_stop = false;
};

// A check that blocks until released.
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
    bool waitEntered()
    {
        std::unique_lock lk(m);
        return cv.wait_for(lk, 5s, [&] { return entered; });
    }
    void release()
    {
        std::lock_guard g(m);
        open = true;
        cv.notify_all();
    }
};

// Runs at scope exit (declared after a future, so it runs before the future waits): lets a runner that
// is waiting on a blocked check give up, so a failed check never hangs the test.
struct Unblock {
    std::function<void()> f;
    ~Unblock() { f(); }
};

struct RunnerRig {
    FakeWorld world;
    FakeFolder folder{world};
    FakeService service{world};
    FakeEngineLock lock{world, service};
    FileStartToken token{folder, world.files};
    FakeEngine engine{world.log};
    FakeSideChannel side{world.log};
    FakeMonitor monitor{world.log, world.clock};
    FakeWatchdog watchdog{world.log, world.clock};
    Sleeper sleeper{world.clock};
    std::vector<std::thread> threads;
    int spawned = 0;
    Gate gate;
    std::unique_ptr<BridgeRunner> runner;

    RunnerRig()
    {
        world.checkLock = false;   // the service never holds apply.lock
        world.files.plantFile(kConfig, configA(), QStringLiteral("root"));
        world.files.plantFile(kToken, StartToken::format(kTokenValue, kGen), QStringLiteral("root"));
        lock.plantPair(QByteArrayLiteral("5555555555555555aaaaaaaaaaaaaaaa"));
        runner = std::make_unique<BridgeRunner>(
            lock, token, folder, engine, side, monitor, watchdog, QByteArrayLiteral("0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f"),
            world.clock,
            [this](std::function<void()> f) {
                ++spawned;
                threads.emplace_back(std::move(f));
            },
            [this](Deadline d) { return sleeper.sleepUntil(d); }, world.clock.clocks());
    }
    ~RunnerRig()
    {
        // The monitor thread is never joined in production; here it is ended and joined before the
        // runner and the fakes it uses go away.
        gate.release();
        sleeper.shutdown();
        for (auto& t : threads) t.join();
        runner.reset();
    }
    bool configRead() const { return folder.reads.contains(kConfig); }
};

}  // namespace

class tst_BridgeRunner : public QObject {
    Q_OBJECT
private slots:
    void startsAndPublishesTheAckFields();
    // INV-103
    void engineLockBeforeConfigRead_data();
    void engineLockBeforeConfigRead();
    void lockIdentityMismatchExits4();
    void stopDoesNotReleaseTheEngineLock();
    // INV-22
    void untrustedFolderExitsThreeWritingNothing();
    // INV-113
    void invalidTokenExits6BeforeConfigRead_data();
    void invalidTokenExits6BeforeConfigRead();
    // INV-34, INV-43, INV-8
    void invalidConfigStartsNothing();
    void unreadableConfigStartsNothing();
    void pendingIgnored();
    void unwritableStatusDoesNotStopRunner();
    void stopPublishesAFinalStatusThenCallsDone();
    // INV-119
    void watchdogArmedBeforeListen();
    void monitorFailureKillsWithin5s_data();
    void monitorFailureKillsWithin5s();
    void blockedMonitorKillsByDeadline();
    void staleStartupCheckRechecked();
    void threeStaleChecksAreStaleFirewall();
    void slowCheckStillListens();
    void giveUpLineBoundedAndNotReextended();
    void exit7SaysWhy_data();
    void exit7SaysWhy();
    void perCheckDeadlineTightened();
    void periodicChecksExtendOnlyOnSuccess();
    void monitorThreadOutlivesTheRunner();
    // INV-119, N9-1: the budgets that end the process are running time
    void suspendDuringStartupCheckDoesNotExpire();
    void suspendDuringPeriodicCheckKeepsItsBudget();
    void noOpMonitorOnLinux();
    // WS-3 hand-off: an unknown Windows ProgramData folder is a fatal startup error
    void dataFolderUnknownIsFatal();
    void locationsResolvedWhenTheLookupWorks();
};

void tst_BridgeRunner::startsAndPublishesTheAckFields()
{
    RunnerRig r;
    QCOMPARE(r.runner->start(), RunnerStart::Ok);
    QVERIFY(r.engine.started);
    QCOMPARE(*r.engine.started, *fromJson(configA(), nullptr));
    QVERIFY(!r.side.published.isEmpty());
    const StatusReport s = r.side.published.last();
    QCOMPARE(s.startToken, kTokenValue);
    QCOMPARE(s.configSha256, sha256Hex(configA()));
    QCOMPARE(s.instanceId, QByteArrayLiteral("0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f"));
    QVERIFY(s.pid > 0);
    QVERIFY(s.startedAt.isValid());
    QVERIFY(!s.configError);
    QCOMPARE(s.ports.size(), 1);
    QCOMPARE(r.folder.mutations(), 0);
}

// INV-103. Kills: reading config (or anything) before engine.lock, or continuing when it is not held.
void tst_BridgeRunner::engineLockBeforeConfigRead_data()
{
    QTest::addColumn<QString>("why");
    QTest::newRow("busy") << QStringLiteral("busy");
    QTest::newRow("missing") << QStringLiteral("missing");
    QTest::newRow("insecure") << QStringLiteral("insecure");
    QTest::newRow("unsupported") << QStringLiteral("unsupported");
}

void tst_BridgeRunner::engineLockBeforeConfigRead()
{
    QFETCH(QString, why);
    RunnerRig r;
    if (why == QLatin1String("busy")) r.service.engineHoldsLock = true;   // another instance holds it
    if (why == QLatin1String("missing")) r.world.lockFiles.eraseEntry(QStringLiteral("engine.lock"));
    if (why == QLatin1String("insecure")) r.lock.insecure = true;
    if (why == QLatin1String("unsupported")) r.lock.unsupportedFs = QStringLiteral("nfs");
    QCOMPARE(r.runner->start(), RunnerStart::EngineLockBusy);
    QCOMPARE(exitCode(RunnerStart::EngineLockBusy), 4);
    QCOMPARE(r.folder.inspections, 0);
    QVERIFY(r.folder.reads.isEmpty());
    QVERIFY(!r.engine.started);
    QVERIFY(r.side.published.isEmpty());
    QCOMPARE(r.world.log.indexOf(QStringLiteral("engineLock:tryAcquire")), 0);
}

void tst_BridgeRunner::lockIdentityMismatchExits4()
{
    RunnerRig r;
    r.lock.plantRecord(QByteArrayLiteral("{\"lockId\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}"));
    QCOMPARE(r.runner->start(), RunnerStart::EngineLockBusy);
    QVERIFY(r.folder.reads.isEmpty());
    QCOMPARE(r.lock.recordContent(), QByteArrayLiteral("{\"lockId\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}"));
}

// INV-103 (pure half; the native lock_held_until_process_exit is WS-7/WS-8's).
void tst_BridgeRunner::stopDoesNotReleaseTheEngineLock()
{
    RunnerRig r;
    QCOMPARE(r.runner->start(), RunnerStart::Ok);
    bool done = false;
    r.runner->stop([&] { done = true; });
    QVERIFY(done);
    QVERIFY(r.lock.appliersHold());   // the fake's "held" flag: still held by this process
    QVERIFY(!r.world.log.contains(QStringLiteral("engineLock:released")));
}

// INV-22. Kills: anything created or written before the trust decision.
void tst_BridgeRunner::untrustedFolderExitsThreeWritingNothing()
{
    for (int variant = 0; variant < 3; ++variant) {
        RunnerRig r;
        if (variant == 0) r.folder.owner = QStringLiteral("attacker");
        if (variant == 1) r.world.files.plantFile(QStringLiteral("planted.txt"), "x");
        if (variant == 2) r.folder.state = FakeFolder::DirState::Missing;
        const QStringList filesBefore = r.world.files.log;
        QCOMPARE(r.runner->start(), RunnerStart::Untrusted);
        QCOMPARE(exitCode(RunnerStart::Untrusted), 3);
        QCOMPARE(r.folder.mutations(), 0);
        QCOMPARE(r.world.files.log, filesBefore);
        QVERIFY(r.folder.reads.isEmpty());   // not even the token
        QVERIFY(r.side.published.isEmpty());
        QVERIFY(!r.engine.started);
    }
}

// INV-113. Kills: accepting a malformed / revoked / writable / oversized / half-written token, or
// reading config before the token. Rows whose damage also makes the FOLDER untrusted (a link, a
// writable file) are refused one step earlier by the Service-mode trust check (exit 3) — the master's
// order: untrusted folders are fenced before the token is read. Either way: zero config reads, zero
// listeners, no status.
void tst_BridgeRunner::invalidTokenExits6BeforeConfigRead_data()
{
    QTest::addColumn<QString>("kind");
    QTest::addColumn<int>("expected");
    QTest::newRow("missing") << QStringLiteral("missing") << int(RunnerStart::NotActivated);
    QTest::newRow("malformed") << QStringLiteral("malformed") << int(RunnerStart::NotActivated);
    QTest::newRow("revoked") << QStringLiteral("revoked") << int(RunnerStart::NotActivated);
    QTest::newRow("empty") << QStringLiteral("empty") << int(RunnerStart::NotActivated);
    QTest::newRow("half-written") << QStringLiteral("half") << int(RunnerStart::NotActivated);
    QTest::newRow("wrong owner") << QStringLiteral("owner") << int(RunnerStart::NotActivated);
    QTest::newRow("oversized") << QStringLiteral("oversized") << int(RunnerStart::NotActivated);
    QTest::newRow("writable (folder untrusted)") << QStringLiteral("writable") << int(RunnerStart::Untrusted);
    QTest::newRow("link (folder untrusted)") << QStringLiteral("link") << int(RunnerStart::Untrusted);
}

void tst_BridgeRunner::invalidTokenExits6BeforeConfigRead()
{
    QFETCH(QString, kind);
    QFETCH(int, expected);
    RunnerRig r;
    const QByteArray valid = StartToken::format(kTokenValue, kGen);
    r.world.files.eraseEntry(kToken);
    if (kind == QLatin1String("malformed")) r.world.files.plantFile(kToken, "{\"token\":1}", QStringLiteral("root"));
    if (kind == QLatin1String("revoked")) r.world.files.plantFile(kToken, StartToken::kRevoked, QStringLiteral("root"));
    if (kind == QLatin1String("empty")) r.world.files.plantFile(kToken, QByteArray(), QStringLiteral("root"));
    if (kind == QLatin1String("half")) r.world.files.plantFile(kToken, valid.left(valid.size() / 2), QStringLiteral("root"));
    if (kind == QLatin1String("owner")) r.world.files.plantFile(kToken, valid, QStringLiteral("process"));   // not the Config owner
    if (kind == QLatin1String("oversized")) r.world.files.plantFile(kToken, valid + QByteArray(300, ' '), QStringLiteral("root"));
    if (kind == QLatin1String("writable")) {
        r.world.files.plantFile(kToken, valid, QStringLiteral("root"));
        r.folder.writableByOthers << kToken;
    }
    if (kind == QLatin1String("link")) r.world.files.plantSymlink(kToken, QStringLiteral("elsewhere"));
    QCOMPARE(int(r.runner->start()), expected);
    if (expected == int(RunnerStart::NotActivated)) {
        QCOMPARE(exitCode(RunnerStart::NotActivated), 6);
        // v1.1 station fix: the host's one line names the token's own reason.
        const QString why = r.runner->tokenRefusal();
        QVERIFY(!why.isEmpty());
        if (kind == QLatin1String("missing")) QVERIFY2(why.contains(QLatin1String("missing")), qPrintable(why));
        if (kind == QLatin1String("revoked")) QVERIFY2(why.contains(QLatin1String("revoked")), qPrintable(why));
        const QString line = notActivatedLine(why);
        QVERIFY2(line.contains(why) && line.contains(QLatin1String("Start or Save & apply")), qPrintable(line));
    } else {
        QVERIFY(r.runner->tokenRefusal().isEmpty());
    }
    QVERIFY(!r.configRead());
    QVERIFY(!r.engine.started);
    QVERIFY(r.side.published.isEmpty());
    QCOMPARE(r.monitor.checks.load(), 0);
}

// INV-34. Kills: starting the valid subset of an invalid config.
void tst_BridgeRunner::invalidConfigStartsNothing()
{
    RunnerRig r;
    BridgeConfig c = *fromJson(configA(), nullptr);
    PortMapping bad = c.ports[0];
    bad.comPort = QStringLiteral("nope");
    bad.tcpPort = 4009;
    c.ports << bad;
    const QByteArray bytes = toJson(c);
    r.world.files.eraseEntry(kConfig);
    r.world.files.plantFile(kConfig, bytes, QStringLiteral("root"));
    QCOMPARE(r.runner->start(), RunnerStart::ConfigError);
    QCOMPARE(exitCode(RunnerStart::ConfigError), 0);
    QVERIFY(!r.engine.started);
    QVERIFY(!r.side.published.isEmpty());
    const StatusReport s = r.side.published.last();
    QVERIFY(s.configError && s.configError->contains(QStringLiteral("nope")));
    QVERIFY(s.ports.isEmpty());
    QCOMPARE(s.configSha256, sha256Hex(bytes));
    QCOMPARE(s.startToken, kTokenValue);
}

void tst_BridgeRunner::unreadableConfigStartsNothing()
{
    RunnerRig r;
    r.world.files.eraseEntry(kConfig);
    r.world.files.plantFile(kConfig, "{ not json", QStringLiteral("root"));
    QCOMPARE(r.runner->start(), RunnerStart::ConfigError);
    QVERIFY(!r.engine.started);
    QVERIFY(r.side.published.last().configError);
}

// INV-43. Kills: the service reading config.pending.json.
void tst_BridgeRunner::pendingIgnored()
{
    RunnerRig r;
    r.world.files.plantFile(kPending, configB(), QStringLiteral("root"));
    QCOMPARE(r.runner->start(), RunnerStart::Ok);
    QCOMPARE(*r.engine.started, *fromJson(configA(), nullptr));
    QVERIFY(!r.folder.reads.contains(kPending));
}

// INV-8. Kills: a status-write exception escaping (and stopping the runner).
void tst_BridgeRunner::unwritableStatusDoesNotStopRunner()
{
    RunnerRig r;
    r.side.throwOnPublish = true;
    QCOMPARE(r.runner->start(), RunnerStart::Ok);
    r.runner->publishNow();   // what the 1 s status timer calls
    r.runner->publishNow();
    QCOMPARE(r.world.log.count(QStringLiteral("side:publish")), 3);
    QCOMPARE(r.engine.stops, 0);
    QVERIFY(r.engine.started);
}

void tst_BridgeRunner::stopPublishesAFinalStatusThenCallsDone()
{
    RunnerRig r;
    QCOMPARE(r.runner->start(), RunnerStart::Ok);
    r.world.log.clear();
    bool done = false;
    r.runner->stop([&] {
        r.world.log.add(QStringLiteral("done"));
        done = true;
    });
    QVERIFY(done);
    QCOMPARE(r.world.log.all(), (QStringList{QStringLiteral("engine:stop"), QStringLiteral("side:publish"), QStringLiteral("done")}));
}

// INV-119. Kills: arming the watchdog after the listeners open.
void tst_BridgeRunner::watchdogArmedBeforeListen()
{
    RunnerRig r;
    QCOMPARE(r.runner->start(), RunnerStart::Ok);
    const int armed = r.world.log.indexOf(QStringLiteral("watchdog:arm"));
    const int checked = r.world.log.indexOf(QStringLiteral("monitor:check 0"));
    const int listening = r.world.log.indexOf(QStringLiteral("engine:start"));
    QVERIFY(armed >= 0 && armed < checked && checked < listening);
    QCOMPARE(r.watchdog.code().value_or(-1), 7);
    QCOMPARE(r.spawned, 1);
    QVERIFY(r.world.log.indexOf(QStringLiteral("monitor:threadInit")) < checked);
}

// INV-119. Kills: letting a failed (or mismatching) check extend the deadline / start the engine.
void tst_BridgeRunner::monitorFailureKillsWithin5s_data()
{
    QTest::addColumn<int>("check");
    QTest::newRow("failed") << int(IFirewallMonitor::Check::Failed);
    QTest::newRow("mismatch") << int(IFirewallMonitor::Check::Mismatch);
}

void tst_BridgeRunner::monitorFailureKillsWithin5s()
{
    QFETCH(int, check);
    RunnerRig r;
    r.monitor.results = {IFirewallMonitor::Check(check)};
    const Deadline t0 = r.world.clock.now();
    QCOMPARE(r.runner->start(), RunnerStart::StaleFirewall);
    QCOMPARE(exitCode(RunnerStart::StaleFirewall), 7);
    QVERIFY(r.watchdog.expiredNow);
    QVERIFY(!r.world.log.contains(QStringLiteral("watchdog:extend")));
    QVERIFY(r.watchdog.deadline() <= t0 + 5s + 100ms);
    QVERIFY(!r.engine.started);
}

// INV-119. A check that never returns: the deadline armed before it (arm time + 5 s) kills.
void tst_BridgeRunner::blockedMonitorKillsByDeadline()
{
    RunnerRig r;
    r.monitor.duringCheck = [&](int) { r.gate.enter(); };
    const Deadline t0 = r.world.clock.now();
    auto started = std::async(std::launch::async, [&] { return r.runner->start(); });
    Unblock unblock{[&] {
        r.world.clock.advance(1h);
        r.gate.release();
    }};
    QVERIFY(r.gate.waitEntered());
    QVERIFY(!r.watchdog.expired());
    r.world.clock.advance(5s + 1ms);
    QVERIFY(r.watchdog.expired());   // what the watchdog thread acts on: faultDie(7)
    QCOMPARE(r.watchdog.deadline(), t0 + 5s);
    QCOMPARE(started.get(), RunnerStart::StaleFirewall);   // the runner gives up at the same point
    QVERIFY(!r.engine.started);
    r.gate.release();
}

// INV-119. Kills: skipping the freshness re-check (listening on a check that ended > 1 s ago).
void tst_BridgeRunner::staleStartupCheckRechecked()
{
    RunnerRig r;
    int extends = 0;
    r.watchdog.onExtend = [&] {
        if (extends++ == 0) r.world.clock.advance(1500ms);   // a pause between the first check's end and listen
    };
    QCOMPARE(r.runner->start(), RunnerStart::Ok);
    QCOMPARE(r.monitor.checks.load(), 2);
    QVERIFY(r.world.log.indexOf(QStringLiteral("monitor:check 1")) < r.world.log.indexOf(QStringLiteral("engine:start")));
}

void tst_BridgeRunner::threeStaleChecksAreStaleFirewall()
{
    RunnerRig r;
    r.watchdog.onExtend = [&] { r.world.clock.advance(1500ms); };   // every result is 1.5 s old at listen
    QStringList said;
    bool boundedBy5s = false;
    r.runner->setLastWords([&](const QString& t) {
        said << t;
        // Each stale success extended the deadline to its start + 35 s; the line is still bounded by 5 s.
        boundedBy5s = r.watchdog.deadline() <= r.world.clock.now() + 5s;
        r.world.log.add(QStringLiteral("lastwords"));
    });
    QCOMPARE(r.runner->start(), RunnerStart::StaleFirewall);
    QVERIFY(boundedBy5s);
    QCOMPARE(r.monitor.checks.load(), 3);
    QVERIFY(r.watchdog.expiredNow);
    QVERIFY(!r.engine.started);
    // The stale exit says so, with each check's duration (v1.1 station fix: exit 7 used to name nothing).
    QCOMPARE(said.size(), 1);
    QVERIFY2(said[0].contains(QStringLiteral("matched 3 times")) &&
                 said[0].count(QStringLiteral("took 100 ms, ended 1500 ms before listen")) == 3 &&
                 said[0].contains(QStringLiteral("exit 7")),
             qPrintable(said[0]));
    QVERIFY(r.world.log.indexOf(QStringLiteral("lastwords")) < r.world.log.indexOf(QStringLiteral("watchdog:expireNow")));
}

// v1.1 station fix (2026-10-18): a check that itself takes longer than the 1 s freshness window (the
// station's ActiveStore read takes about 1.3 s; a PowerShell replica of it 1.83-1.90 s with its connect) still
// opens the listeners: freshness is measured from the check's END. Kills: measuring it from the check's start (every
// such check is stale: three re-checks, then exit 7 on every service start).
void tst_BridgeRunner::slowCheckStillListens()
{
    RunnerRig r;
    r.monitor.checkDuration = 1900ms;
    Deadline checkStart{};
    r.monitor.duringCheck = [&](int n) {
        if (n == 0) checkStart = r.world.clock.now();
    };
    QCOMPARE(r.runner->start(), RunnerStart::Ok);
    QCOMPARE(r.monitor.checks.load(), 1);
    QVERIFY(r.engine.started);
    QVERIFY(!r.watchdog.expiredNow);
    // The exposure bound is unchanged: still the check's START + 35 s (N9-1), not its end.
    QCOMPARE(r.watchdog.deadline(), checkStart + 35s);
}

// A stale success leaves its start + 35 s armed; the monitor is then held BEFORE the retry's tighten, so
// the runner's 5 s wait runs out with that deadline still armed. The give-up line must be bounded by 5 s anyway, and the
// retry's late Match must not extend the deadline again. Kills: speaking without tightening; extending after abandon.
void tst_BridgeRunner::giveUpLineBoundedAndNotReextended()
{
    RunnerRig r;
    int extends = 0;
    r.watchdog.onExtend = [&] {
        if (extends++ == 0) r.world.clock.advance(1500ms);   // the first success is stale at listen
    };
    // tighten #0 is the first check's (arm is separate); #1 is the retry's: hold the monitor there.
    r.watchdog.onTighten = [&](int n) {
        if (n == 1) r.gate.enter();
    };
    bool boundedBy5s = false;
    Deadline spokenDeadline{};
    r.runner->setLastWords([&](const QString& t) {
        spokenDeadline = r.watchdog.deadline();
        boundedBy5s = spokenDeadline <= r.world.clock.now() + 5s && t.contains(QStringLiteral("did not finish"));
    });
    auto started = std::async(std::launch::async, [&] { return r.runner->start(); });
    Unblock unblock{[&] {
        r.world.clock.advance(1h);
        r.gate.release();
    }};
    QVERIFY(r.gate.waitEntered());
    r.world.clock.advance(5s + 1ms);   // the runner's 5 s wait for the retry runs out
    QCOMPARE(started.get(), RunnerStart::StaleFirewall);
    QVERIFY(boundedBy5s);
    r.gate.release();                  // the retry now runs and matches, late
    QTRY_COMPARE(r.monitor.checks.load(), 2);
    QTest::qWait(50);
    QCOMPARE(extends, 1);              // only the first success extended
    QVERIFY(r.watchdog.deadline() <= spokenDeadline);
}

// v1.1 station fix: every exit 7 says why, once, before the process is ended (bridge.log, else the event log, in
// production). Kills: the runner (startup) or the monitor (periodic) expiring before the reason is said.
void tst_BridgeRunner::exit7SaysWhy_data()
{
    QTest::addColumn<int>("check");
    QTest::addColumn<bool>("periodic");
    QTest::newRow("startup-failed") << int(IFirewallMonitor::Check::Failed) << false;
    QTest::newRow("startup-mismatch") << int(IFirewallMonitor::Check::Mismatch) << false;
    QTest::newRow("periodic-mismatch") << int(IFirewallMonitor::Check::Mismatch) << true;
}

void tst_BridgeRunner::exit7SaysWhy()
{
    QFETCH(int, check);
    QFETCH(bool, periodic);
    RunnerRig r;
    const auto bad = IFirewallMonitor::Check(check);
    r.monitor.results = periodic ? QList<IFirewallMonitor::Check>{IFirewallMonitor::Check::Match, bad} : QList<IFirewallMonitor::Check>{bad};
    r.monitor.why = QStringLiteral("rule 'AetherSerialBridge-4e8b4c3aceca1909-1': LocalPort differs");
    std::mutex m;
    QStringList said;
    std::atomic<bool> boundedBy5s{false};
    r.runner->setLastWords([&](const QString& t) {
        boundedBy5s = r.watchdog.deadline() <= r.world.clock.now() + 5s;
        {
            std::lock_guard g(m);
            said << t;
        }
        r.world.log.add(QStringLiteral("lastwords"));
    });
    if (!periodic) {
        QCOMPARE(r.runner->start(), RunnerStart::StaleFirewall);
    } else {
        QCOMPARE(r.runner->start(), RunnerStart::Ok);
        QVERIFY(r.sleeper.waitSleeping());
        r.world.clock.advance(30s);
        r.sleeper.poke();
        QTRY_VERIFY(r.watchdog.expiredNow);
    }
    QVERIFY(r.watchdog.expiredNow);
    QVERIFY(boundedBy5s);   // every exit-7 line is said while a 5 s deadline is armed
    std::lock_guard g(m);
    QCOMPARE(said.size(), 1);
    QVERIFY2(said[0].contains(r.monitor.why) && said[0].contains(periodic ? QStringLiteral("periodic") : QStringLiteral("startup")) &&
                 said[0].contains(bad == IFirewallMonitor::Check::Failed ? QStringLiteral("failed") : QStringLiteral("mismatch")),
             qPrintable(said[0]));
    QVERIFY(r.world.log.indexOf(QStringLiteral("lastwords")) < r.world.log.indexOf(QStringLiteral("watchdog:expireNow")));
}

// INV-119 (N9-1). Kills: no per-check tighten — a stale startup success extended the deadline to
// t0 + 35 s; the hung retry must still die 5 s after IT started.
void tst_BridgeRunner::perCheckDeadlineTightened()
{
    RunnerRig r;
    Deadline retryStart{};
    int extends = 0;
    r.watchdog.onExtend = [&] {
        if (extends++ == 0) r.world.clock.advance(1500ms);   // the first success is stale at listen
    };
    r.monitor.duringCheck = [&](int n) {
        if (n == 1) {
            retryStart = r.world.clock.now();
            r.gate.enter();
        }
    };
    auto started = std::async(std::launch::async, [&] { return r.runner->start(); });
    Unblock unblock{[&] {
        r.world.clock.advance(1h);
        r.gate.release();
    }};
    QVERIFY(r.gate.waitEntered());
    QCOMPARE(r.watchdog.deadline(), retryStart + 5s);
    r.world.clock.advance(5s + 1ms);
    QVERIFY(r.watchdog.expired());
    QCOMPARE(started.get(), RunnerStart::StaleFirewall);
    r.gate.release();
}

// INV-119: after the start, a check every 30 s; success extends to start + 35 s, anything else expires.
void tst_BridgeRunner::periodicChecksExtendOnlyOnSuccess()
{
    RunnerRig r;
    r.monitor.results = {IFirewallMonitor::Check::Match, IFirewallMonitor::Check::Match, IFirewallMonitor::Check::Mismatch};
    QCOMPARE(r.runner->start(), RunnerStart::Ok);
    const Deadline afterStartup = r.watchdog.deadline();
    QVERIFY(r.sleeper.waitSleeping());
    r.world.clock.advance(30s);
    r.sleeper.poke();
    QTRY_COMPARE(r.monitor.checks.load(), 2);
    QTRY_VERIFY(r.watchdog.deadline() > afterStartup);
    QVERIFY(!r.watchdog.expiredNow);
    QVERIFY(r.sleeper.waitSleeping());
    r.world.clock.advance(30s);
    r.sleeper.poke();
    QTRY_VERIFY(r.watchdog.expiredNow);
    QCOMPARE(r.monitor.checks.load(), 3);
}

// The monitor thread uses only what it shares with the runner (the handshake, copies of the
// config, generation and sleep function) and the host's seams, so a runner destroyed while a periodic
// check is in flight leaves that check valid state. Production never destroys a runner; this pins the
// header's claim. The use-after-free it guards against is visible under AddressSanitizer.
void tst_BridgeRunner::monitorThreadOutlivesTheRunner()
{
    RunnerRig r;
    r.monitor.duringCheck = [&r](int n) {
        if (n == 1) r.gate.enter();   // the first periodic check blocks inside checkOnce
    };
    QCOMPARE(r.runner->start(), RunnerStart::Ok);
    QVERIFY(r.sleeper.waitSleeping());
    r.world.clock.advance(30s);
    r.sleeper.poke();
    QVERIFY(r.gate.waitEntered());
    const Deadline before = r.watchdog.deadline();
    r.runner.reset();   // the runner goes away mid-check
    r.gate.release();
    QTRY_VERIFY(r.watchdog.deadline() > before);   // the check finished and extended the deadline
    QCOMPARE(r.monitor.lastTcpPort.load(), 4001);   // ... against the config it was started with
    QVERIFY(!r.watchdog.expiredNow);
}

// INV-119, N9-1. The machine sleeps while the startup check is outstanding: steady time
// passes, running time does not. The runner's 5 s wait is running time, so it neither gives up nor calls
// expireNow; the check that spanned the suspend is stale (freshness is steady time: the safe direction),
// so it is checked again and the engine starts. Kills: measuring the runner's wait on the steady clock.
void tst_BridgeRunner::suspendDuringStartupCheckDoesNotExpire()
{
    RunnerRig r;
    r.monitor.duringCheck = [&](int n) {
        if (n == 0) r.gate.enter();
    };
    // start() runs on this thread (it starts the status timer); a helper suspends the machine meanwhile.
    std::atomic<bool> expiredDuringSuspend{true};
    std::thread helper([&] {
        if (r.gate.waitEntered()) {
            r.world.clock.advance(1s);      // one second of running time inside the check
            r.world.clock.suspend(10min);   // then the machine sleeps
            std::this_thread::sleep_for(200ms);   // the runner polls every 10 ms: it sees the clock after the suspend
            expiredDuringSuspend = r.watchdog.expiredNow || r.watchdog.expired();
        } else {
            r.world.clock.advance(1h);
        }
        r.gate.release();
    });
    const RunnerStart started = r.runner->start();
    helper.join();
    QVERIFY(!expiredDuringSuspend);
    QCOMPARE(started, RunnerStart::Ok);
    QCOMPARE(r.monitor.checks.load(), 2);   // the first result is stale after the suspend: checked again
    // ...and the engine starts on the SECOND check: the count alone also reaches 2 through the first periodic check,
    // which runs at once after a suspend (v1.1: kills the end-measured freshness that ignores a suspend).
    QVERIFY(r.world.log.indexOf(QStringLiteral("monitor:check 1")) < r.world.log.indexOf(QStringLiteral("engine:start")));
    QVERIFY(r.engine.started);
    QVERIFY(!r.watchdog.expiredNow);
}

// INV-119, N9-1. The machine sleeps inside a periodic check that then succeeds. The 35 s it
// grants is counted from the check's start in running time, so the suspend does not eat it: the extend
// asks for r0 + 35 s, and the next check (due at once on the steady schedule) keeps its full 5 s. Kills:
// extending to the steady t0 + 35 s, which after a long suspend is already past (the extend is ignored and
// the next check inherits what is left of the previous check's 5 s).
void tst_BridgeRunner::suspendDuringPeriodicCheckKeepsItsBudget()
{
    RunnerRig r;
    RunningTime::Duration r0{};
    r.monitor.duringCheck = [&](int n) {
        if (n == 1) {
            r0 = r.world.clock.running();   // nothing moved the clock since the check began
            r.world.clock.advance(2s);
            r.world.clock.suspend(1h);
        }
        if (n == 2) r.gate.enter();
    };
    QCOMPARE(r.runner->start(), RunnerStart::Ok);
    QVERIFY(r.sleeper.waitSleeping());
    r.world.clock.advance(30s);
    Unblock unblock{[&] {
        r.world.clock.advance(1h);
        r.gate.release();
    }};
    r.sleeper.poke();
    QVERIFY(r.gate.waitEntered());   // check 2 runs at once: its steady due time passed during the suspend
    QVERIFY(r.watchdog.lastExtend());
    QCOMPARE(r.watchdog.lastExtend()->count(), (r0 + 35s).count());
    // Check 2 began 2.1 s of running time after check 1: its own 5 s from there.
    QCOMPARE(r.watchdog.runningDeadline().count(), (r0 + 2100ms + 5s).count());
    r.world.clock.advance(4900ms);
    QVERIFY(!r.watchdog.expired());
    QVERIFY(!r.watchdog.expiredNow);
    r.world.clock.advance(200ms);
    QVERIFY(r.watchdog.expired());   // and no more than its 5 s
    r.gate.release();
}

// INV-119. Linux: a no-op monitor, no threads, no watchdog arming.
void tst_BridgeRunner::noOpMonitorOnLinux()
{
    RunnerRig r;
    r.monitor.isEnabled = false;
    QCOMPARE(r.runner->start(), RunnerStart::Ok);
    QCOMPARE(r.spawned, 0);
    QVERIFY(!r.watchdog.armed());
    QCOMPARE(r.monitor.checks.load(), 0);
    QVERIFY(r.engine.started);
}

// WS-3 AppLocations "FAILURE". Kills: substituting a fallback folder when the lookup fails.
void tst_BridgeRunner::dataFolderUnknownIsFatal()
{
    for (const QString& broken : {QString(), QStringLiteral("ProgramData"), QStringLiteral("/")}) {
        const ServiceLocations l = serviceLocations(true, [&] { return broken; });
        QVERIFY2(!l.locations, qPrintable(broken));
        QCOMPARE(l.failure, RunnerStart::DataFolderUnknown);
        QVERIFY(exitCode(l.failure) != 0);
        QCOMPARE(exitCode(l.failure), 3);
        QCOMPARE(l.error, QString::fromLatin1(kWindowsDataFolderError));
    }
}

void tst_BridgeRunner::locationsResolvedWhenTheLookupWorks()
{
    const ServiceLocations w = serviceLocations(true, [] { return QStringLiteral("D:/ProgramData"); });
    QVERIFY(w.locations);
    QCOMPARE(w.failure, RunnerStart::Ok);
    QCOMPARE(w.locations->configFile, QStringLiteral("D:/ProgramData/AetherSerialBridge/config.json"));
    const ServiceLocations l = serviceLocations(false, [] { return QString(); });   // Linux never fails
    QVERIFY(l.locations);
    QCOMPARE(l.locations->configFile, QString::fromLatin1(kLinuxConfigFile));
}

QTEST_GUILESS_MAIN(tst_BridgeRunner)
#include "tst_BridgeRunner.moc"
