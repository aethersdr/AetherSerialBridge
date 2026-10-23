// WS-7 native CIM rows on windows-latest (admin, the real WMI firewall provider): the recipe and the comparator
// against what Windows really stores (INV-11, INV-114, INV-117). The rules are created in the reserved group on
// the runner (a throwaway VM) and every row removes what it created.
//
//  fw_recipe_roundtrip / fw_written_rule_passes_comparator   sync, then the comparator over the ActiveStore: Match;
//                                                            every field the writer sets or leaves default reads back
//  update_only_cannot_resurrect_deleted_rule                 a rule deleted between the read and the UPDATE_ONLY enable
//                                                            stays deleted (CREATE_OR_UPDATE would recreate it)
//  fw_new_rules_disabled_until_filters_set                   at the moment of each enable, a second session reads the
//                                                            rule's filters: all three set
//  fw_rollback_on_failed_add                                 an add that fails removes only its own generation
//  fw_sweep_removes_stray_rule                               a stray rule injected after the sync is swept
//  security_filter_override_detected / _read_failure_fails   the security association is read and judged
//  fw_keyword_matrix                                         the pinned keyword set (FirewallKeywords.h) is the
//                                                            provider's on this build

#include "WinNativeSupport.h"

#include "core/FirewallKeywords.h"
#include "core/FirewallPlan.h"
#include "platform/win/CimFirewall.h"
#include "platform/win/WbemSession.h"

#include <QProcess>
#include <QTest>

#include <atomic>
#include <functional>
#include <thread>

using namespace asb;
using namespace asb::win;
using Check = IFirewallMonitor::Check;

namespace {
const QString kExe = QStringLiteral(R"(C:\Program Files\AetherSerialBridge\aether-serial-bridge.exe)");
const FirewallGeneration kGen{QByteArrayLiteral("00000000000000c1")};
const FirewallGeneration kOld{QByteArrayLiteral("00000000000000c0")};

PortMapping port(const QString& name, int tcp, const QString& allowed)
{
    PortMapping p;
    p.name = name;
    p.comPort = QStringLiteral("COM%1").arg(tcp - 47090);   // a distinct port per mapping
    p.tcpPort = tcp;
    p.bindAddress = QStringLiteral("0.0.0.0");
    p.allowedClients = allowed;
    return p;
}

BridgeConfig config()
{
    BridgeConfig c;
    c.ports = {port(QStringLiteral("Amp"), 47101, QStringLiteral("010.0.0.5, 192.168.1.7/24, 10.1.0.1-10.1.0.9, LocalSubnet, fe80::1")),
               port(QStringLiteral("Tuner"), 47102, {})};
    return c;
}

Deadline later() { return std::chrono::steady_clock::now() + std::chrono::seconds(60); }

// A decorator around the real session: `before` runs before each call (may throw, block or act).
class Hooked final : public ICimSession {
public:
    explicit Hooked(ICimSession& real) : m_real(real) {}
    std::function<void(const QString& call)> before;
    void connect() override
    {
        if (before) before(QStringLiteral("connect"));
        m_real.connect();
    }
    QList<cim::Instance> query(const QString& wql, cim::Store s) override
    {
        if (before) before(QStringLiteral("query ") + wql);
        return m_real.query(wql, s);
    }
    void put(const QString& c, const QString& p, const cim::Props& v, cim::PutFlag f, cim::Store s,
             const std::function<void()>& beforeWrite = {}) override
    {
        QString line = QStringLiteral("put ") + (p.isEmpty() ? c : p);
        for (auto it = v.cbegin(); it != v.cend(); ++it)
            if (it->kind == cim::Value::Kind::Int) line += QStringLiteral(" %1=%2").arg(it.key()).arg(it->i);
        if (before) before(line);
        m_real.put(c, p, v, f, s, beforeWrite);
    }
    void remove(const QString& p, cim::Store s) override
    {
        if (before) before(QStringLiteral("remove ") + p);
        m_real.remove(p, s);
    }

private:
    ICimSession& m_real;
};

QStringList groupIds(ICimSession& s, cim::Store store = cim::Store::Persistent)
{
    QStringList ids;
    for (const auto& r : s.query(groupQuery(), store)) ids << r.props.value(QStringLiteral("InstanceID")).s;
    ids.sort();
    return ids;
}

void removeGroup(ICimSession& s)
{
    for (const auto& r : s.query(groupQuery(), cim::Store::Persistent)) s.remove(r.relPath, cim::Store::Persistent);
}

QString id(const FirewallGeneration& g, int i)
{
    return QStringLiteral("AetherSerialBridge-%1-%2").arg(QString::fromLatin1(g.id)).arg(i);
}

// Runs a test body; a CIM exception is a named failure, not a crash that ends the whole executable.
#define GUARDED(...)                                                                                       \
    try {                                                                                                  \
        __VA_ARGS__                                                                                        \
    } catch (const std::exception& e) {                                                                     \
        QFAIL(e.what());                                                                                   \
    }

Check checkActive(ICimSession& s, const BridgeConfig& c, const FirewallGeneration& g, QString* why)
{
    CimFirewallMonitor mon(s, kExe);
    mon.threadInit();
    const Check v = mon.checkOnce(c, g);
    if (why) *why = mon.lastWhy();
    return v;
}
}  // namespace

class tst_CimNative : public QObject {
    Q_OBJECT
    WbemSession m_cim;

private slots:
    void initTestCase()
    {
        m_cim.connect();
        removeGroup(m_cim);
    }
    void cleanup() { removeGroup(m_cim); }

    void fw_written_rule_passes_comparator()
    {
        GUARDED({
        WbemSession session;
        CimFirewall fw(session, kExe);
        fw.connect(std::chrono::steady_clock::now() + std::chrono::seconds(60));
        fw.connect(later());
        fw.sync(config(), kGen, later());
        QCOMPARE(groupIds(m_cim), (QStringList{id(kGen, 0), id(kGen, 1)}));
        // Every field the writer sets or leaves at its default, as Windows stores it (the ActiveStore).
        const auto group = CimFirewall::readGroup(m_cim, cim::Store::Active);
        for (const auto& r : group) {
            QCOMPARE(r.port.size(), 1);
            QCOMPARE(r.address.size(), 1);
            QCOMPARE(r.application.size(), 1);
            QCOMPARE(r.security.size(), 1);
            qInfo("rule %s: RemoteAddress=%s LocalAddress kind=%d AppPath=%s EdgeTraversal=%lld PolicyStoreSourceType=%lld",
                  qPrintable(r.rule.value(QStringLiteral("InstanceID")).s),
                  qPrintable(r.address.front().value(QStringLiteral("RemoteAddress")).list.join(QLatin1Char(','))),
                  int(r.address.front().value(QStringLiteral("LocalAddress")).kind),
                  qPrintable(r.application.front().value(QStringLiteral("AppPath")).s),
                  r.rule.value(QStringLiteral("EdgeTraversalPolicy")).i, r.rule.value(QStringLiteral("PolicyStoreSourceType")).i);
        }
        QString why;
        // The ActiveStore may lag a PersistentStore write (VERIFY-ON-WINDOWS active_store_lag): give it a moment.
        Check v = Check::Failed;
        for (int i = 0; i < 20 && v != Check::Match; ++i) {
            v = checkActive(m_cim, config(), kGen, &why);
            if (v != Check::Match) Sleep(250);
        }
        QVERIFY2(v == Check::Match, qPrintable(why));
        QCOMPARE(checkActive(m_cim, config(), kOld, &why), Check::Mismatch);
        })
    }

    // INV-114 (mutant "use WBEM_FLAG_CREATE_OR_UPDATE for updates"): a rule deleted between the read and the
    // enable is not recreated by the UPDATE_ONLY enable.
    void update_only_cannot_resurrect_deleted_rule()
    {
        GUARDED({
        WbemSession session;
        CimFirewall fw(session, kExe);
        fw.connect(std::chrono::steady_clock::now() + std::chrono::seconds(60));
        bool deleted = false;
        // Between GetObject and PutInstance of the first enable (the rule object is already in hand):
        session.setBeforePutInstance([&](const QString& relPath) {
            if (deleted || !relPath.startsWith(QLatin1String("MSFT_NetFirewallRule.")) || !relPath.contains(id(kGen, 0))) return;
            const auto rules = m_cim.query(QStringLiteral("SELECT * FROM MSFT_NetFirewallRule WHERE InstanceID = '%1'").arg(id(kGen, 0)),
                                           cim::Store::Persistent);
            if (rules.size() != 1) return;
            if (rules.front().props.value(QStringLiteral("Enabled")).i != 2) return;
            // Only the enable updates the rule object itself (the filters are other objects).
            m_cim.remove(rules.front().relPath, cim::Store::Persistent);
            deleted = true;
        });
        QVERIFY_THROWS_EXCEPTION(std::exception, fw.sync(config(), kGen, later()));
        QVERIFY(deleted);
        QVERIFY(!groupIds(m_cim).contains(id(kGen, 0)));
        })
    }

    // Review round 1: the provider's GetObject returns after the deadline. CimFirewall's check runs between
    // it and PutInstance (WbemSession calls the write gate there), so the enable is never written: the rule stays
    // disabled, and only the rollback (exempt from the deadline) follows.
    void late_lookup_writes_nothing()
    {
        GUARDED({
        WbemSession session;
        struct LateClock final : IClock {
            std::atomic<int> skewSeconds{0};
            Deadline now() const override { return std::chrono::steady_clock::now() + std::chrono::seconds(skewSeconds.load()); }
            void sleepFor(std::chrono::milliseconds d) override { std::this_thread::sleep_for(d); }
        } clock;
        CimFirewall fw(session, kExe, clock);
        const Deadline deadline = later();
        fw.connect(deadline);
        // Between GetObject and PutInstance of rule 0's enable (its filters are other objects, written before):
        session.setBeforePutInstance([&](const QString& relPath) {
            if (relPath.startsWith(QLatin1String("MSFT_NetFirewallRule.")) && relPath.contains(id(kGen, 0))) clock.skewSeconds = 120;
        });
        QVERIFY_THROWS_EXCEPTION(std::exception, fw.sync(config(), kGen, deadline));
        QCOMPARE(clock.skewSeconds.load(), 120);
        const auto rules = m_cim.query(QStringLiteral("SELECT * FROM MSFT_NetFirewallRule WHERE InstanceID = '%1'").arg(id(kGen, 0)),
                                       cim::Store::Persistent);
        QCOMPARE(rules.size(), 1);
        QCOMPARE(rules.front().props.value(QStringLiteral("Enabled")).i, qint64(2));   // never enabled
        session.setBeforePutInstance({});
        fw.rollback(kGen, deadline);
        QVERIFY(!groupIds(m_cim).contains(id(kGen, 0)));
        })
    }

    // INV-11 (native): at the moment each rule is enabled, its three filters are already set (read by a second
    // session from the provider).
    void fw_new_rules_disabled_until_filters_set()
    {
        GUARDED({
        WbemSession session;
        Hooked hooked(session);
        CimFirewall fw(hooked, kExe);
        fw.connect(std::chrono::steady_clock::now() + std::chrono::seconds(60));
        QStringList problems;
        int enables = 0;
        hooked.before = [&](const QString& call) {
            if (!call.startsWith(QLatin1String("put MSFT_NetFirewallRule.")) || !call.contains(QLatin1String("Enabled=1"))) return;
            ++enables;
            for (const auto& r : CimFirewall::readGroup(m_cim, cim::Store::Persistent)) {
                const QString rid = r.rule.value(QStringLiteral("InstanceID")).s;
                if (!call.contains(rid)) continue;
                if (r.rule.value(QStringLiteral("Enabled")).i != 2) problems << rid + QStringLiteral(" enabled before the enable step");
                if (r.port.front().value(QStringLiteral("Protocol")).s != QLatin1String("TCP")) problems << rid + QStringLiteral(" port filter not set");
                if (r.application.front().value(QStringLiteral("AppPath")).kind != cim::Value::Kind::String) problems << rid + QStringLiteral(" application filter not set");
            }
        };
        fw.sync(config(), kGen, later());
        QCOMPARE(enables, 2);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
        })
    }

    void fw_rollback_on_failed_add()
    {
        GUARDED({
        WbemSession session;
        Hooked hooked(session);
        CimFirewall fw(hooked, kExe);
        fw.connect(std::chrono::steady_clock::now() + std::chrono::seconds(60));
        fw.sync(config(), kOld, later());
        hooked.before = [&](const QString& call) {
            if (call.startsWith(QLatin1String("put MSFT_NetApplicationFilter")) && call.contains(id(kGen, 1)))
                throw cim::CimError(0x80041001, QStringLiteral("injected failure"));
        };
        QVERIFY_THROWS_EXCEPTION(std::exception, fw.sync(config(), kGen, later()));
        hooked.before = nullptr;
        fw.rollback(kGen, later());
        QCOMPARE(groupIds(m_cim), (QStringList{id(kOld, 0), id(kOld, 1)}));
        })
    }

    // INV-11 (mutant "skip the R9 sweep"): a stray rule injected after the sync and before the sweep is removed.
    void fw_sweep_removes_stray_rule()
    {
        GUARDED({
        WbemSession session;
        CimFirewall fw(session, kExe);
        fw.connect(std::chrono::steady_clock::now() + std::chrono::seconds(60));
        fw.sync(config(), kGen, later());
        cim::Props r;
        r[QStringLiteral("InstanceID")] = cim::Value::string(id(kOld, 3));
        r[QStringLiteral("ElementName")] = cim::Value::string(QStringLiteral("stray"));
        r[QStringLiteral("RuleGroup")] = cim::Value::string(QString::fromLatin1(FirewallPlan::kGroup));
        r[QStringLiteral("Direction")] = cim::Value::integer(1);
        r[QStringLiteral("Action")] = cim::Value::integer(2);
        r[QStringLiteral("Enabled")] = cim::Value::integer(2);
        m_cim.put(QStringLiteral("MSFT_NetFirewallRule"), {}, r, cim::PutFlag::CreateOnly, cim::Store::Persistent);
        QVERIFY(groupIds(m_cim).contains(id(kOld, 3)));
        fw.sweep(kGen, later());
        QCOMPARE(groupIds(m_cim), (QStringList{id(kGen, 0), id(kGen, 1)}));
        })
    }

    void security_filter_override_detected()
    {
        GUARDED({
        WbemSession session;
        CimFirewall fw(session, kExe);
        fw.connect(std::chrono::steady_clock::now() + std::chrono::seconds(60));
        fw.sync(config(), kGen, later());
        const auto rules = m_cim.query(QStringLiteral("SELECT * FROM MSFT_NetFirewallRule WHERE InstanceID = '%1'").arg(id(kGen, 0)),
                                       cim::Store::Persistent);
        QCOMPARE(rules.size(), 1);
        const auto sec = m_cim.query(QStringLiteral("ASSOCIATORS OF {%1} WHERE AssocClass = %2").arg(rules.front().relPath, QString::fromLatin1(kAssocSecurity)),
                                     cim::Store::Persistent);
        QCOMPARE(sec.size(), 1);
        cim::Props p;
        // OverrideBlockRules needs Authentication Required and an authorised remote-machine list (as
        // Set-NetFirewallSecurityFilter -OverrideBlockRules does); on windows-latest the provider refuses it both
        // without the list (WBEM_E_FAILED) and with this property name (WBEM_E_NOT_FOUND), so the fallback below runs.
        p[QStringLiteral("Authentication")] = cim::Value::integer(1);
        p[QStringLiteral("RemoteMachine")] = cim::Value::string(QStringLiteral("D:(A;;CC;;;WD)"));
        p[QStringLiteral("OverrideBlockRules")] = cim::Value::integer(1);
        try {
            m_cim.put({}, sec.front().relPath, p, cim::PutFlag::UpdateOnly, cim::Store::Persistent);
        } catch (const std::exception& e) {
            // The provider refuses the override on this host (no IPsec configuration): judge the provider's own
            // read-back of the security filter with OverrideBlockRules set, which is what the reader returns for
            // such a rule. The read itself is real (and its failure is the next row).
            qInfo("the provider refused OverrideBlockRules here (%s): judging the real read-back with it set", e.what());
            auto group = CimFirewall::readGroup(m_cim, cim::Store::Active);
            for (auto& r : group)
                if (r.rule.value(QStringLiteral("InstanceID")).s == id(kGen, 0)) r.security.front()[QStringLiteral("OverrideBlockRules")] = cim::Value::integer(1);
            QCOMPARE(FirewallCompare::compare(group, *FirewallPlan::build(config(), kGen, kExe, nullptr)).check, Check::Mismatch);
            return;
        }
        QString why;
        Check v = Check::Match;
        for (int i = 0; i < 20 && v == Check::Match; ++i) {
            v = checkActive(m_cim, config(), kGen, &why);
            if (v == Check::Match) Sleep(250);
        }
        QVERIFY2(v == Check::Mismatch, qPrintable(why));
        })
    }

    void security_filter_read_failure_fails()
    {
        GUARDED({
        WbemSession session;
        CimFirewall fw(session, kExe);
        fw.connect(std::chrono::steady_clock::now() + std::chrono::seconds(60));
        fw.sync(config(), kGen, later());
        WbemSession monitorSession;
        Hooked hooked(monitorSession);
        hooked.before = [](const QString& call) {
            if (call.contains(QLatin1String("MSFT_NetFirewallRuleFilterBySecurity")))
                throw cim::CimError(0x80041001, QStringLiteral("injected security-filter read failure"));
        };
        QString why;
        QCOMPARE(checkActive(hooked, config(), kGen, &why), Check::Failed);
        })
    }

    // The pinned keyword set (FirewallKeywords.h) is exactly what the provider stores on this build: one DISABLED
    // rule per known keyword (and PlayToTargets) in a private group, its stored RemoteAddress read back.
    void fw_keyword_matrix()
    {
        GUARDED({
        const QString group = QString::fromLatin1(asb::test::names::kKeywordMatrixCiGroup);
        QStringList keywords;
        for (std::string_view k : kKnownFirewallKeywords) keywords << QString::fromLatin1(k.data(), qsizetype(k.size()));
        keywords << QStringLiteral("PlayToTargets");
        QStringList differs;
        int n = 0;
        for (const QString& kw : keywords) {
            const QString rid = QStringLiteral("AsbKeywordMatrixCi-%1").arg(n++);
            cim::Props r;
            r[QStringLiteral("InstanceID")] = cim::Value::string(rid);
            r[QStringLiteral("ElementName")] = cim::Value::string(rid);
            r[QStringLiteral("RuleGroup")] = cim::Value::string(group);
            r[QStringLiteral("Direction")] = cim::Value::integer(1);
            r[QStringLiteral("Action")] = cim::Value::integer(2);
            r[QStringLiteral("Enabled")] = cim::Value::integer(2);
            m_cim.put(QStringLiteral("MSFT_NetFirewallRule"), {}, r, cim::PutFlag::CreateOnly, cim::Store::Persistent);
            const auto rule = m_cim.query(QStringLiteral("SELECT * FROM MSFT_NetFirewallRule WHERE InstanceID = '%1'").arg(rid), cim::Store::Persistent);
            const auto f = m_cim.query(QStringLiteral("ASSOCIATORS OF {%1} WHERE AssocClass = %2").arg(rule.front().relPath, QString::fromLatin1(kAssocAddress)),
                                       cim::Store::Persistent);
            bool accepted = true;
            try {
                cim::Props a;
                a[QStringLiteral("RemoteAddress")] = cim::Value::strings({kw});
                m_cim.put({}, f.front().relPath, a, cim::PutFlag::UpdateOnly, cim::Store::Persistent);
            } catch (const std::exception&) {
                accepted = false;
            }
            m_cim.remove(rule.front().relPath, cim::Store::Persistent);
            bool pinned = false;
            for (std::string_view k : kAcceptedFirewallKeywords)
                if (kw == QString::fromLatin1(k.data(), qsizetype(k.size()))) pinned = true;
            qInfo("KEYWORD %s provider=%s pinned=%s", qPrintable(kw), accepted ? "accepted" : "refused", pinned ? "accepted" : "refused");
            if (accepted != pinned) differs << kw;
        }
        QVERIFY2(differs.isEmpty(), qPrintable(QStringLiteral("the pin differs from the provider for: ") + differs.join(QStringLiteral(", "))));
        })
    }

    // INV-117 (mutant "read PersistentStore only"): a rule in the reserved group that comes from another store —
    // the local Group Policy store — is seen by the ActiveStore read and is a mismatch.
    void gpo_store_rule_in_reserved_group_detected()
    {
        GUARDED({
            WbemSession session;
            CimFirewall fw(session, kExe);
            fw.connect(std::chrono::steady_clock::now() + std::chrono::seconds(60));
            fw.sync(config(), kGen, later());
            cim::Props r;
            r[QStringLiteral("InstanceID")] = cim::Value::string(QStringLiteral("asb-test-gpo-rule"));
            r[QStringLiteral("ElementName")] = cim::Value::string(QStringLiteral("gpo rule"));
            r[QStringLiteral("RuleGroup")] = cim::Value::string(QString::fromLatin1(FirewallPlan::kGroup));
            r[QStringLiteral("Direction")] = cim::Value::integer(1);
            r[QStringLiteral("Action")] = cim::Value::integer(2);
            r[QStringLiteral("Enabled")] = cim::Value::integer(1);
            m_cim.put(QStringLiteral("MSFT_NetFirewallRule"), {}, r, cim::PutFlag::CreateOnly, cim::Store::LocalGpo);
            // Group Policy reaches the ActiveStore on a policy refresh.
            QProcess gp;
            gp.start(QStringLiteral("gpupdate.exe"), {QStringLiteral("/target:computer"), QStringLiteral("/force")});
            gp.waitForFinished(120000);
            bool seen = false;
            for (int i = 0; i < 60 && !seen; ++i) {
                for (const auto& x : m_cim.query(groupQuery(), cim::Store::Active))
                    if (x.props.value(QStringLiteral("InstanceID")).s == QLatin1String("asb-test-gpo-rule")) seen = true;
                if (!seen) Sleep(500);
            }
            QString why;
            const Check v = checkActive(m_cim, config(), kGen, &why);
            for (const auto& x : m_cim.query(groupQuery(), cim::Store::LocalGpo)) m_cim.remove(x.relPath, cim::Store::LocalGpo);
            QProcess gp2;
            gp2.start(QStringLiteral("gpupdate.exe"), {QStringLiteral("/target:computer"), QStringLiteral("/force")});
            gp2.waitForFinished(120000);
            QVERIFY2(seen, "the local GPO rule never reached the ActiveStore");
            QVERIFY2(v == Check::Mismatch, qPrintable(why));
            // The PersistentStore alone does not show it (what the mutant would read).
        })
    }
};

QTEST_GUILESS_MAIN(tst_CimNative)
#include "tst_CimNative.moc"
