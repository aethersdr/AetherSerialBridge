// CimFirewall at the CIM seam (WS-7; INV-11, INV-114, INV-117 writer side). The provider is FakeCim (the probe's
// measured behaviour); the Applier, OperationHost, locks and service are WS-5's fakes, so exclusion is observed
// exactly as a second Applier or an engine start would observe it.
//
//  INV-11  the recipe: create disabled -> port, address, application filters -> enable, per rule; old rules
//          deleted only after every new rule is enabled; rollback deletes only this generation; sweep deletes
//          everything not of the kept generation.
//  INV-114 creates CREATE_ONLY, updates UPDATE_ONLY; no normal step after the deadline; apply.lock and engine.lock
//          stay held while the operation worker is inside a CIM call (ConnectServer included), past every
//          deadline.

#include "core/Applier.h"
#include "core/ConfigJson.h"
#include "core/OperationHost.h"
#include "platform/win/CimFirewall.h"
#include "support/FakeCim.h"
#include "support/Fakes.h"

#include <QRegularExpression>
#include <QTest>
#include <QThread>

#include <condition_variable>
#include <mutex>
#include <thread>

using namespace asb;
using namespace asb::test;
using namespace asb::win;
using Check = IFirewallMonitor::Check;

namespace {
const FirewallGeneration kGen{QByteArrayLiteral("0123456789abcdef")};
const FirewallGeneration kOld{QByteArrayLiteral("00000000000000aa")};
const QString kExe = QStringLiteral(R"(C:\Program Files\AetherSerialBridge\aether-serial-bridge.exe)");
const QString kGroup = QString::fromLatin1(FirewallPlan::kGroup);

PortMapping port(const QString& name, const QString& com, int tcp, const QString& allowed = {}, bool enabled = true)
{
    PortMapping p;
    p.name = name;
    p.comPort = com;
    p.tcpPort = tcp;
    p.bindAddress = QStringLiteral("0.0.0.0");
    p.allowedClients = allowed;
    p.enabled = enabled;
    return p;
}

BridgeConfig config()
{
    BridgeConfig c;
    c.ports = {port(QStringLiteral("Amp"), QStringLiteral("COM3"), 4532, QStringLiteral("010.0.0.5, 10.1.2.3/8, LocalSubnet")),
               port(QStringLiteral("Tuner"), QStringLiteral("COM4"), 4533),
               port(QStringLiteral("Off"), QStringLiteral("COM9"), 4534, {}, false)};
    return c;
}

QString id(const FirewallGeneration& g, int i)
{
    return QStringLiteral("AetherSerialBridge-%1-%2").arg(QString::fromLatin1(g.id)).arg(i);
}

class Clock final : public IClock {
public:
    Deadline now() const override { return t; }
    void sleepFor(std::chrono::milliseconds d) override { t += d; }
    Deadline t = Deadline{} + std::chrono::hours(1);
};

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
    void waitEntered()
    {
        std::unique_lock lk(m);
        cv.wait(lk, [&] { return entered; });
    }
    void release()
    {
        std::lock_guard g(m);
        open = true;
        cv.notify_all();
    }
};
struct OpenOnExit {
    Gate& gate;
    ~OpenOnExit() { gate.release(); }
};

struct Observer : IOperationObserver {
    QList<Interim> interims;
    QList<Result> finals;
    void interim(const Interim& i) override { interims << i; }
    void finished(const Result& r) override { finals << r; }
};

SaveRequest request(const QByteArray& bytes)
{
    SaveRequest r;
    r.config = bytes;
    r.source.kind = Source::Committed;
    r.source.sha256 = digest(configA());
    return r;
}

void spinUntil(const std::function<bool()>& done)
{
    for (int i = 0; i < 5000 && !done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

// Index of the first log line containing every needle, from `from`; -1 if none.
int find(const QStringList& log, const QStringList& needles, int from = 0)
{
    for (int i = from; i < log.size(); ++i) {
        bool all = true;
        for (const QString& n : needles) all = all && log[i].contains(n);
        if (all) return i;
    }
    return -1;
}
}  // namespace

class tst_CimFirewall : public QObject {
    Q_OBJECT

private slots:
    // v1.1 INV-129 (WS-17): ConnectServer is bounded. WbemSession::connect passes exactly connectServerCall(): the
    // namespace and WBEM_FLAG_CONNECT_USE_MAX_WAIT (0x80, "returns within 2 minutes"); v1.0 passed 0 (no bound).
    void connectUsesMaxWait()
    {
        const cim::ConnectServerCall c = cim::connectServerCall();
        QCOMPARE(c.ns, QStringLiteral("ROOT\\StandardCimv2"));
        QCOMPARE(c.flags, qint32(0x80));
        QCOMPARE(cim::kWbemConnectUseMaxWait, qint32(0x80));
    }

    void recipeWritesWhatTheComparatorAccepts()
    {
        FakeCim cim;
        Clock clock;
        CimFirewall fw(cim, kExe, clock);
        fw.connect(clock.now() + std::chrono::seconds(60));
        fw.sync(config(), kGen, clock.now() + std::chrono::seconds(60));
        QCOMPARE(cim.ids(), (QStringList{id(kGen, 0), id(kGen, 1)}));
        // The writer's values: canonical addresses (octal read as the engine reads it), masked prefix, keyword.
        const auto& a = cim.persistent[id(kGen, 0)].address[QStringLiteral("RemoteAddress")];
        QCOMPARE(a.list, (QStringList{QStringLiteral("8.0.0.5"), QStringLiteral("10.0.0.0/8"), QStringLiteral("LocalSubnet")}));
        QCOMPARE(cim.persistent[id(kGen, 0)].rule[QStringLiteral("ElementName")].s,
                 QStringLiteral("Aether Serial Bridge - Amp (TCP 4532)"));
        QCOMPARE(cim.persistent[id(kGen, 0)].rule[QStringLiteral("RuleGroup")].s, kGroup);
        QCOMPARE(cim.persistent[id(kGen, 0)].application[QStringLiteral("AppPath")].s, kExe);
        // Round trip: what the writer wrote is a Match for the comparator (fw_written_rule_passes_comparator's twin).
        CimFirewallMonitor mon(cim, kExe);
        mon.threadInit();
        QCOMPARE(mon.checkOnce(config(), kGen), Check::Match);
        QCOMPARE(mon.checkOnce(config(), kOld), Check::Mismatch);
    }

    // INV-114 mutant "use WBEM_FLAG_CREATE_OR_UPDATE for updates".
    void flagsCreateOnlyUpdateOnly()
    {
        FakeCim cim;
        Clock clock;
        CimFirewall fw(cim, kExe, clock);
        fw.connect(clock.now() + std::chrono::seconds(60));
        fw.sync(config(), kGen, clock.now() + std::chrono::seconds(60));
        const QStringList puts = cim.calls(QStringLiteral("put "));
        QCOMPARE(puts.size(), 2 * 5);   // per rule: one create, then port, address, application and enable updates
        for (const QString& p : puts) {
            QVERIFY2(p.startsWith(QLatin1String("put PersistentStore ")), qPrintable(p));
            if (p.section(QLatin1Char(' '), 3, 3) == QLatin1String("MSFT_NetFirewallRule"))   // a new instance
                QVERIFY2(p.contains(QLatin1String(" CREATE_ONLY ")), qPrintable(p));
            else
                QVERIFY2(p.contains(QLatin1String(" UPDATE_ONLY ")), qPrintable(p));
        }
        // ... and the provider honours them: an update can never resurrect a rule deleted under it.
        cim.hook = [&](const QString& call) {
            if (call.contains(QLatin1String("Enabled=1"))) {
                std::lock_guard g(cim.m);
                cim.persistent.remove(id(kGen, 0));   // deleted between the read and the enable
            }
        };
        cim.persistent.clear();
        QVERIFY_THROWS_EXCEPTION(std::exception, fw.sync(config(), kGen, clock.now() + std::chrono::seconds(60)));
        QVERIFY(!cim.persistent.contains(id(kGen, 0)));
    }

    // INV-11: each rule is created disabled and enabled only after its three filters were set.
    void newRulesDisabledUntilFiltersSet()
    {
        FakeCim cim;
        Clock clock;
        CimFirewall fw(cim, kExe, clock);
        fw.connect(clock.now() + std::chrono::seconds(60));
        QStringList problems;
        int creates = 0;
        cim.hook = [&](const QString& call) {
            if (call.section(QLatin1Char(' '), 3, 3) == QLatin1String("MSFT_NetFirewallRule")) ++creates;
            if (call.section(QLatin1Char(' '), 3, 3) == QLatin1String("MSFT_NetFirewallRule") && !call.contains(QLatin1String("Enabled=2")))
                problems << QStringLiteral("created enabled: ") + call;
            if (!call.contains(QLatin1String("Enabled=1"))) return;
            std::lock_guard g(cim.m);
            for (auto it = cim.persistent.cbegin(); it != cim.persistent.cend(); ++it) {
                if (!call.contains(it.key())) continue;
                if (it->port.value(QStringLiteral("Protocol")).s != QLatin1String("TCP") ||
                    !it->address.contains(QStringLiteral("RemoteAddress")) ||
                    !it->application.contains(QStringLiteral("AppPath")))
                    problems << QStringLiteral("enabled before its filters: ") + it.key();
            }
        };
        fw.sync(config(), kGen, clock.now() + std::chrono::seconds(60));
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QLatin1Char('\n'))));
        QCOMPARE(creates, 2);
        QCOMPARE(cim.calls(QStringLiteral("put")).filter(QStringLiteral("Enabled=1")).size(), 2);
    }

    // INV-11 mutant "remove old before add".
    void oldRemovedOnlyAfterEveryAdd()
    {
        FakeCim cim;
        Clock clock;
        cim.persistent.insert(id(kOld, 0), FakeCim::asWritten(id(kOld, 0), kGroup, QStringLiteral("4532"), {QStringLiteral("Any")}, kExe));
        cim.persistent.insert(QStringLiteral("stray"), FakeCim::ruleWithDefaults(QStringLiteral("stray"), kGroup, 2));
        CimFirewall fw(cim, kExe, clock);
        fw.connect(clock.now() + std::chrono::seconds(60));
        fw.sync(config(), kGen, clock.now() + std::chrono::seconds(60));
        const QStringList log = cim.log;
        int lastEnable = -1;
        for (int i = 0; i < log.size(); ++i)
            if (log[i].contains(QLatin1String("Enabled=1"))) lastEnable = i;
        const int firstRemove = find(log, {QStringLiteral("remove ")});
        QVERIFY(lastEnable >= 0);
        QVERIFY(firstRemove > lastEnable);
        QCOMPARE(cim.ids(), (QStringList{id(kGen, 0), id(kGen, 1)}));
    }

    void failedAddRollsBackOnlyThisGeneration()
    {
        FakeCim cim;
        Clock clock;
        cim.persistent.insert(id(kOld, 0), FakeCim::asWritten(id(kOld, 0), kGroup, QStringLiteral("4532"), {QStringLiteral("Any")}, kExe));
        CimFirewall fw(cim, kExe, clock);
        fw.connect(clock.now() + std::chrono::seconds(60));
        cim.hook = [](const QString& call) {
            if (call.contains(QLatin1String("MSFT_NetApplicationFilter")) && call.contains(QLatin1String("-1\"")))
                throw win::cim::CimError(0x80041001, QStringLiteral("Generic failure"));
        };
        QVERIFY_THROWS_EXCEPTION(std::exception, fw.sync(config(), kGen, clock.now() + std::chrono::seconds(60)));
        QVERIFY(cim.persistent.contains(id(kGen, 0)));   // created and enabled before the failure
        QVERIFY(cim.persistent.contains(id(kOld, 0)));   // the old rules keep working
        cim.hook = nullptr;
        fw.rollback(kGen, clock.now() + std::chrono::seconds(60));
        QCOMPARE(cim.ids(), QStringList{id(kOld, 0)});
    }

    void sweepAndRemoveAll()
    {
        FakeCim cim;
        Clock clock;
        CimFirewall fw(cim, kExe, clock);
        fw.connect(clock.now() + std::chrono::seconds(60));
        fw.sync(config(), kGen, clock.now() + std::chrono::seconds(60));
        cim.persistent.insert(QStringLiteral("stray-after-sync"), FakeCim::ruleWithDefaults(QStringLiteral("stray-after-sync"), kGroup, 1));
        cim.persistent.insert(QStringLiteral("other-group"), FakeCim::ruleWithDefaults(QStringLiteral("other-group"), QStringLiteral("Someone Else"), 1));
        fw.sweep(kGen, clock.now() + std::chrono::seconds(60));
        QCOMPARE(cim.ids(), (QStringList{id(kGen, 0), id(kGen, 1), QStringLiteral("other-group")}));
        fw.removeAll(clock.now() + std::chrono::seconds(60));
        QCOMPARE(cim.ids(), QStringList{QStringLiteral("other-group")});   // never outside the reserved group
    }

    void writerUsesThePlanTokensOnly()
    {
        // A plan token the writer cannot express (not on the accepted keyword list) fails the sync before the rule
        // is enabled: never "Any" by accident.
        FakeCim cim;
        Clock clock;
        CimFirewall fw(cim, kExe, clock);
        fw.connect(clock.now() + std::chrono::seconds(60));
        BridgeConfig c;
        c.ports = {port(QStringLiteral("Amp"), QStringLiteral("COM3"), 4532, QStringLiteral("10.0.0.0/8"))};
        fw.sync(c, kGen, clock.now() + std::chrono::seconds(60));
        QCOMPARE(cim.persistent[id(kGen, 0)].address[QStringLiteral("RemoteAddress")].list, QStringList{QStringLiteral("10.0.0.0/8")});
    }

    // INV-114: once the deadline has passed no further normal step starts (only the rollback runs).
    void noNormalStepAfterDeadline()
    {
        FakeCim cim;
        Clock clock;
        CimFirewall fw(cim, kExe, clock);
        fw.connect(clock.now() + std::chrono::seconds(60));
        const Deadline deadline = clock.now() + std::chrono::seconds(60);
        int puts = 0;
        cim.hook = [&](const QString& call) {
            if (call.startsWith(QLatin1String("put")) && ++puts == 2) clock.t += std::chrono::seconds(61);   // returns late
        };
        QVERIFY_THROWS_EXCEPTION(std::exception, fw.sync(config(), kGen, deadline));
        QCOMPARE(cim.calls(QStringLiteral("put")).size(), 2);   // the late call returned; nothing after it
        QVERIFY_THROWS_EXCEPTION(std::exception, fw.sweep(kGen, deadline));
        QVERIFY_THROWS_EXCEPTION(std::exception, fw.removeAll(deadline));
        QCOMPARE(cim.calls(QStringLiteral("remove")).size(), 0);
        try {
            fw.rollback(kGen, deadline);   // the cleanup of this generation still runs
        } catch (const std::exception& e) {
            QFAIL(qPrintable(QStringLiteral("the rollback was refused after the deadline: ") + QString::fromUtf8(e.what())));
        }
        QVERIFY(cim.ids().isEmpty());
    }

    // Review round 1: a lookup that returns after the deadline is not followed by its write. The rule
    // query, the filter association query and the provider's own GetObject (FakeCim's "get") all block; whichever
    // returns late, no put or remove follows it in the normal steps, and the rollback still cleans up.
    void lateLookupWritesNothing_data()
    {
        QTest::addColumn<QString>("op");     // sync or removeAll
        QTest::addColumn<QString>("late");   // the lookup (log text) that returns after the deadline
        QTest::newRow("rule query before a filter write") << "sync" << "WHERE InstanceID";
        QTest::newRow("filter query before a filter write") << "sync" << "ASSOCIATORS OF";
        QTest::newRow("GetObject before the rule is created") << "sync" << "get PersistentStore MSFT_NetFirewallRule";
        QTest::newRow("GetObject of a filter before its write") << "sync" << "get PersistentStore MSFT_NetProtocolPortFilter";
        QTest::newRow("rule query before a delete") << "removeAll" << "WHERE InstanceID";
    }
    void lateLookupWritesNothing()
    {
        QFETCH(QString, op);
        QFETCH(QString, late);
        FakeCim cim;
        Clock clock;
        if (op == QLatin1String("removeAll"))
            cim.persistent.insert(id(kOld, 0), FakeCim::asWritten(id(kOld, 0), kGroup, QStringLiteral("4532"), {QStringLiteral("Any")}, kExe));
        CimFirewall fw(cim, kExe, clock);
        fw.connect(clock.now() + std::chrono::seconds(60));
        const Deadline deadline = clock.now() + std::chrono::seconds(60);
        int lateAt = -1;
        cim.hook = [&](const QString& call) {
            if (lateAt < 0 && call.contains(late)) {
                lateAt = int(cim.log.size()) - 1;
                clock.t += std::chrono::seconds(61);   // the lookup returns after the deadline
            }
        };
        if (op == QLatin1String("removeAll")) QVERIFY_THROWS_EXCEPTION(std::exception, fw.removeAll(deadline));
        else QVERIFY_THROWS_EXCEPTION(std::exception, fw.sync(config(), kGen, deadline));
        QVERIFY2(lateAt >= 0, "the late lookup never happened");
        const QStringList after = cim.log.mid(lateAt + 1);
        QVERIFY2(after.filter(QRegularExpression(QStringLiteral("^(put|remove) "))).isEmpty(), qPrintable(after.join(QLatin1Char('\n'))));
        if (op == QLatin1String("removeAll")) {
            QVERIFY(cim.persistent.contains(id(kOld, 0)));   // not deleted after the deadline
            return;
        }
        cim.hook = nullptr;
        try {
            fw.rollback(kGen, deadline);   // cleanup: exempt from the deadline
        } catch (const std::exception& e) {
            QFAIL(qPrintable(QStringLiteral("the rollback was refused after the deadline: ") + QString::fromUtf8(e.what())));
        }
        QVERIFY(!cim.ids().contains(id(kGen, 0)));
        QVERIFY(!cim.ids().contains(id(kGen, 1)));
    }

    // WS-5 round 4: every operation connects exactly once, first; a call while unconnected is a thrown step
    // failure (never a crash, never a connection of its own).
    void callsWhileUnconnectedThrow()
    {
        FakeCim cim;
        Clock clock;
        CimFirewall fw(cim, kExe, clock);
        const Deadline d = clock.now() + std::chrono::seconds(60);
        QVERIFY_THROWS_EXCEPTION(PlatformError, fw.sync(config(), kGen, d));
        QVERIFY_THROWS_EXCEPTION(PlatformError, fw.rollback(kGen, d));
        QVERIFY_THROWS_EXCEPTION(PlatformError, fw.sweep(kGen, d));
        QVERIFY_THROWS_EXCEPTION(PlatformError, fw.removeAll(d));
        QCOMPARE(cim.connects, 0);
        QVERIFY(cim.log.isEmpty());
        fw.connect(d);
        fw.connect(d);   // once per operation; a repeat changes nothing
        QCOMPARE(cim.connects, 1);
    }

    void connectAfterDeadlineRefused()
    {
        FakeCim cim;
        Clock clock;
        CimFirewall fw(cim, kExe, clock);
        QVERIFY_THROWS_EXCEPTION(std::exception, fw.connect(clock.now() - std::chrono::seconds(1)));
        QCOMPARE(cim.connects, 0);
    }

    // INV-114 mutant "release apply.lock at the deadline while the worker is still blocked": a CIM call blocks in
    // the provider far past the firewall deadline; apply.lock and engine.lock stay held (a second Applier waits,
    // an engine start cannot take engine.lock) until the call returns; then only failClosed runs.
    void exclusionHeldWhileWorkerBlocked()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCim cim;
        CimFirewall fw(cim, kExe, r.world.clock);
        Platform p{r.folder, r.service, fw, r.world.applyLock, r.engineLock, *r.token, r.status, r.io};
        Gate gate;
        cim.hook = [&](const QString& call) {
            if (!call.startsWith(QLatin1String("put"))) return;
            r.world.clock.advance(std::chrono::seconds(600));   // ten minutes in the provider
            gate.enter();
        };
        OperationHost host(p, r.world.clock, std::chrono::milliseconds(5));
        OpenOnExit open{gate};
        Observer o;
        QVERIFY(host.submitRun(request(configB()), false, false, o));
        gate.waitEntered();
        QTRY_COMPARE(o.interims.size(), 1);
        QTest::qWait(50);
        QVERIFY(r.world.applyLock.held());
        QVERIFY(r.engineLock.appliersHold());
        LockResult why = LockResult::Acquired;
        QVERIFY(!r.engineLock.tryAcquire(&why));   // an engine start cannot take engine.lock
        std::thread other([&] { Applier(p, r.world.clock).stop(); });   // a second Applier
        spinUntil([&] { return r.world.applyLock.waiting.load() == 1; });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        QCOMPARE(r.world.applyLock.waiting.load(), 1);
        QCOMPARE(o.finals.size(), 0);
        const int putsBefore = cim.calls(QStringLiteral("put")).size();
        cim.hook = nullptr;
        gate.release();
        other.join();
        QTRY_COMPARE(o.finals.size(), 1);
        QCOMPARE(o.finals[0].outcome, Outcome::StateUnknown);
        QCOMPARE(cim.calls(QStringLiteral("put")).size(), putsBefore);   // no normal step after the deadline
    }

    // INV-114: the same while ConnectServer itself hangs (COM setup and ConnectServer run on the worker too).
    void blockedConnectServerHoldsExclusion()
    {
        Rig r;
        r.installedRunning(configA());
        FakeCim cim;
        CimFirewall fw(cim, kExe, r.world.clock);
        Platform p{r.folder, r.service, fw, r.world.applyLock, r.engineLock, *r.token, r.status, r.io};
        Gate gate;
        cim.hook = [&](const QString& call) {
            if (call != QLatin1String("connect")) return;
            r.world.clock.advance(std::chrono::seconds(600));
            gate.enter();
        };
        OperationHost host(p, r.world.clock, std::chrono::milliseconds(5));
        OpenOnExit open{gate};
        Observer o;
        QVERIFY(host.submitRun(request(configB()), false, false, o));
        gate.waitEntered();
        QTRY_COMPARE(o.interims.size(), 1);
        QVERIFY(r.world.applyLock.held());
        QVERIFY(r.engineLock.appliersHold());
        std::thread other([&] { Applier(p, r.world.clock).stop(); });   // a second Applier waits
        spinUntil([&] { return r.world.applyLock.waiting.load() == 1; });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        QCOMPARE(r.world.applyLock.waiting.load(), 1);
        QCOMPARE(o.finals.size(), 0);
        cim.hook = nullptr;
        gate.release();
        other.join();
        QTRY_COMPARE(o.finals.size(), 1);
        QCOMPARE(o.finals[0].outcome, Outcome::StateUnknown);
        QVERIFY(cim.calls(QStringLiteral("put")).isEmpty());
        QVERIFY(!r.world.applyLock.held());
    }
};

QTEST_GUILESS_MAIN(tst_CimFirewall)
#include "tst_CimFirewall.moc"
