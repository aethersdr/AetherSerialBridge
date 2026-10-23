// INV-117 (WS-7): the Windows firewall comparator, pure. The reader (CimFirewall::readGroup over FakeCim) builds
// the group exactly as the native reader does; the rows are the spec's: identical filters but the wrong
// generation, a malformed name, a duplicate index, an enabled foreign rule with an unrelated name, a disabled
// foreign rule (ignored), a missing planned rule, an address set reordered (equal), keyword vs address, a partial
// enumeration, a missing association, an empty plan with a failed enumeration — plus every planned field.

#include "core/FirewallPlan.h"
#include "platform/win/CimFirewall.h"
#include "platform/win/FirewallCompare.h"
#include "support/FakeCim.h"

#include <QTest>

using namespace asb;
using namespace asb::win;
using asb::test::FakeCim;
using Check = IFirewallMonitor::Check;
using FirewallCompare::Value;
Q_DECLARE_METATYPE(asb::win::FirewallCompare::Value)

namespace {
const FirewallGeneration kGen{QByteArrayLiteral("0123456789abcdef")};
const FirewallGeneration kOther{QByteArrayLiteral("fedcba9876543210")};
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
    c.ports = {port(QStringLiteral("Amp"), QStringLiteral("COM3"), 4532, QStringLiteral("192.168.1.10, 10.0.0.0/8, LocalSubnet")),
               port(QStringLiteral("Tuner"), QStringLiteral("COM4"), 4533)};
    return c;
}

FirewallPlan::Plan plan(const BridgeConfig& c = config(), const FirewallGeneration& g = kGen)
{
    QString err;
    const auto p = FirewallPlan::build(c, g, kExe, &err);
    if (!p) qFatal("plan failed: %s", qPrintable(err));
    return *p;
}

// The group as CimFirewall writes it for `p`, in the provider's read-back form (mask notation, reordered).
void written(FakeCim& cim, const FirewallPlan::Plan& p)
{
    for (const FirewallPlan::Rule& r : p.rules) {
        QStringList remote = *FirewallCompare::writerValues(r.remoteAddresses);
        for (QString& v : remote)
            if (v == QLatin1String("10.0.0.0/8")) v = QStringLiteral("10.0.0.0/255.0.0.0");
        std::reverse(remote.begin(), remote.end());
        cim.persistent.insert(r.instanceId,
                              FakeCim::asWritten(r.instanceId, kGroup, r.localPorts.front(), remote, r.program));
    }
}

FirewallCompare::Verdict verdict(FakeCim& cim, const FirewallPlan::Plan& p = plan())
{
    return FirewallCompare::compare(CimFirewall::readGroup(cim, cim::Store::Active), p);
}

QString id(int i, const FirewallGeneration& g = kGen)
{
    return QStringLiteral("AetherSerialBridge-%1-%2").arg(QString::fromLatin1(g.id)).arg(i);
}
}  // namespace

class tst_FirewallCompare : public QObject {
    Q_OBJECT

private slots:
    void writtenGroupMatches()
    {
        FakeCim cim;
        written(cim, plan());
        const auto v = verdict(cim);
        QCOMPARE(v.check, Check::Match);
    }

    // INV-117 mutant "compare filters but not the generation".
    void identicalFiltersWrongGeneration()
    {
        const auto other = plan(config(), kOther);
        FakeCim cim;
        written(cim, other);   // every field identical, but generation kOther
        QCOMPARE(verdict(cim).check, Check::Mismatch);
        // ... even when the planned rules are there too.
        FakeCim both;
        written(both, plan());
        for (auto it = cim.persistent.cbegin(); it != cim.persistent.cend(); ++it) both.persistent.insert(it.key(), *it);
        QCOMPARE(verdict(both).check, Check::Mismatch);
    }

    void malformedName_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("short hex") << QStringLiteral("AetherSerialBridge-0123456789abcde-0");
        QTest::newRow("upper hex") << QStringLiteral("AetherSerialBridge-0123456789ABCDEF-0");
        QTest::newRow("32-hex generation") << QStringLiteral("AetherSerialBridge-0123456789abcdef0123456789abcdef-0");
        QTest::newRow("leading zero index") << QStringLiteral("AetherSerialBridge-0123456789abcdef-01");
        QTest::newRow("4-digit index") << QStringLiteral("AetherSerialBridge-0123456789abcdef-1000");
        QTest::newRow("suffix") << QStringLiteral("AetherSerialBridge-0123456789abcdef-0x");
    }
    void malformedName()
    {
        QFETCH(QString, name);
        FakeCim cim;
        written(cim, plan());
        cim.persistent.insert(name, FakeCim::asWritten(name, kGroup, QStringLiteral("4532"), {QStringLiteral("Any")}, kExe));
        QCOMPARE(verdict(cim).check, Check::Mismatch);
    }

    void duplicateIndex()
    {
        // Two enabled rules claim index 0: the planned one and a second under the same name in another store.
        FakeCim cim;
        written(cim, plan());
        FakeCim::Rule dup = cim.persistent.value(id(0));
        dup.rule[QStringLiteral("PolicyStoreSourceType")] = Value::integer(1);
        cim.gpoRules.insert(id(0), dup);
        QCOMPARE(verdict(cim).check, Check::Mismatch);
    }

    // INV-117 mutant "ignore enabled foreign rules".
    void foreignEnabledRuleUnrelatedName()
    {
        FakeCim cim;
        written(cim, plan());
        cim.persistent.insert(QStringLiteral("{8c1b-admin-rule}"),
                              FakeCim::asWritten(QStringLiteral("{8c1b-admin-rule}"), kGroup, QStringLiteral("3389"),
                                                 {QStringLiteral("Any")}, QStringLiteral("System")));
        QCOMPARE(verdict(cim).check, Check::Mismatch);
    }

    void disabledForeignRuleIgnored()
    {
        FakeCim cim;
        written(cim, plan());
        cim.persistent.insert(QStringLiteral("anything"), FakeCim::ruleWithDefaults(QStringLiteral("anything"), kGroup, 2));
        cim.persistent.insert(id(5, kOther), FakeCim::ruleWithDefaults(id(5, kOther), kGroup, 2));
        QCOMPARE(verdict(cim).check, Check::Match);
    }

    void missingPlannedRule()
    {
        FakeCim cim;
        written(cim, plan());
        cim.persistent.remove(id(1));
        QCOMPARE(verdict(cim).check, Check::Mismatch);
    }

    void disabledPlannedRuleIsMissing()
    {
        FakeCim cim;
        written(cim, plan());
        cim.persistent[id(1)].rule[QStringLiteral("Enabled")] = Value::integer(2);
        QCOMPARE(verdict(cim).check, Check::Mismatch);
    }

    void unplannedIndexOfTheGeneration()
    {
        FakeCim cim;
        written(cim, plan());
        cim.persistent.insert(id(7), FakeCim::asWritten(id(7), kGroup, QStringLiteral("4532"), {QStringLiteral("Any")}, kExe));
        QCOMPARE(verdict(cim).check, Check::Mismatch);
    }

    // INV-117 mutant "compare addresses as ordered lists".
    void addressSetReorderedEqual()
    {
        FakeCim cim;
        written(cim, plan());   // written() reverses the list and uses the mask form
        QCOMPARE(verdict(cim).check, Check::Match);
        auto& a = cim.persistent[id(0)].address[QStringLiteral("RemoteAddress")];
        QStringList shuffled = a.list;
        std::rotate(shuffled.begin(), shuffled.begin() + 1, shuffled.end());
        a.list = shuffled;
        QCOMPARE(verdict(cim).check, Check::Match);
        a.list << a.list.front();   // a duplicate is still the same set
        QCOMPARE(verdict(cim).check, Check::Match);
    }

    void keywordVsAddress()
    {
        FakeCim cim;
        written(cim, plan());
        auto& a = cim.persistent[id(0)].address[QStringLiteral("RemoteAddress")];
        a.list.replace(a.list.indexOf(QStringLiteral("LocalSubnet")), QStringLiteral("192.168.1.0/24"));
        QCOMPARE(verdict(cim).check, Check::Mismatch);
        // the keyword in another case is the same keyword (the provider canonicalises it)
        FakeCim cim2;
        written(cim2, plan());
        auto& b = cim2.persistent[id(0)].address[QStringLiteral("RemoteAddress")];
        b.list.replace(b.list.indexOf(QStringLiteral("LocalSubnet")), QStringLiteral("localsubnet"));
        QCOMPARE(verdict(cim2).check, Check::Match);
        // Any added to a restricted rule is wider
        FakeCim cim3;
        written(cim3, plan());
        cim3.persistent[id(0)].address[QStringLiteral("RemoteAddress")].list << QStringLiteral("Any");
        QCOMPARE(verdict(cim3).check, Check::Mismatch);
        // an unrestricted planned rule reads back as null (Any)
        FakeCim cim4;
        written(cim4, plan());
        QVERIFY(!cim4.persistent[id(1)].address.contains(QStringLiteral("RemoteAddress")));
        QCOMPARE(verdict(cim4).check, Check::Match);
        cim4.persistent[id(1)].address[QStringLiteral("RemoteAddress")] = Value::strings({QStringLiteral("10.0.0.1")});
        QCOMPARE(verdict(cim4).check, Check::Mismatch);
    }

    void fieldDiffers_data()
    {
        QTest::addColumn<QString>("which");
        QTest::addColumn<QString>("prop");
        QTest::addColumn<Value>("value");
        auto add = [](const char* row, const char* which, const char* prop, const Value& v) {
            QTest::newRow(row) << QString::fromLatin1(which) << QString::fromLatin1(prop) << v;
        };
        add("direction outbound", "rule", "Direction", Value::integer(2));
        add("action block", "rule", "Action", Value::integer(4));
        add("profiles public", "rule", "Profiles", Value::integer(4));
        add("edge traversal allow", "rule", "EdgeTraversalPolicy", Value::integer(1));
        add("from group policy", "rule", "PolicyStoreSourceType", Value::integer(2));
        add("enabled null", "rule", "Enabled", Value::null());
        add("protocol udp", "port", "Protocol", Value::string(QStringLiteral("UDP")));
        add("protocol any", "port", "Protocol", Value::string(QStringLiteral("Any")));
        add("local port other", "port", "LocalPort", Value::strings({QStringLiteral("4533")}));
        add("local port extra", "port", "LocalPort", Value::strings({QStringLiteral("4532"), QStringLiteral("4540")}));
        add("local port any", "port", "LocalPort", Value::null());
        add("remote port set", "port", "RemotePort", Value::strings({QStringLiteral("80")}));
        add("local address set (bind address)", "address", "LocalAddress", Value::strings({QStringLiteral("192.168.1.5")}));
        add("remote address narrower", "address", "RemoteAddress", Value::strings({QStringLiteral("192.168.1.10")}));
        add("remote address any", "address", "RemoteAddress", Value::null());
        add("program other", "application", "AppPath", Value::string(QStringLiteral(R"(C:\Windows\System32\cmd.exe)")));
        add("program any", "application", "AppPath", Value::null());
        add("override block rules", "security", "OverrideBlockRules", Value::integer(1));
    }
    void fieldDiffers()
    {
        QFETCH(QString, which);
        QFETCH(QString, prop);
        QFETCH(Value, value);
        FakeCim cim;
        written(cim, plan());
        FakeCim::Rule& r = cim.persistent[id(0)];
        win::cim::Props& p = which == QLatin1String("rule") ? r.rule
                             : which == QLatin1String("port")      ? r.port
                             : which == QLatin1String("address")   ? r.address
                             : which == QLatin1String("application") ? r.application
                                                                     : r.security;
        p[prop] = value;
        QCOMPARE(verdict(cim).check, Check::Mismatch);
    }

    void programComparedCaseInsensitivelyWithNormalisedSeparators()
    {
        FakeCim cim;
        written(cim, plan());
        cim.persistent[id(0)].application[QStringLiteral("AppPath")] =
            Value::string(QStringLiteral("c:/program files/aetherserialbridge/AETHER-SERIAL-BRIDGE.EXE"));
        QCOMPARE(verdict(cim).check, Check::Match);
    }

    void missingAssociation()
    {
        // A rule whose association read returns no object: Failed (not a match, not merely a mismatch).
        FakeCim cim;
        written(cim, plan());
        QList<FirewallCompare::RuleRead> g = CimFirewall::readGroup(cim, cim::Store::Active);
        g[0].security.clear();
        QCOMPARE(FirewallCompare::compare(g, plan()).check, Check::Failed);
        g = CimFirewall::readGroup(cim, cim::Store::Active);
        g[1].port << g[1].port.front();   // an extra filter
        QCOMPARE(FirewallCompare::compare(g, plan()).check, Check::Failed);
        // ... a disabled rule with a broken association too: the read was not complete
        g = CimFirewall::readGroup(cim, cim::Store::Active);
        g[0].rule[QStringLiteral("Enabled")] = Value::integer(2);
        g[0].application.clear();
        QCOMPARE(FirewallCompare::compare(g, plan()).check, Check::Failed);
    }

    // INV-117 mutant "treat a partial enumeration as an empty success".
    void partialEnumerationFails()
    {
        FakeCim cim;
        written(cim, plan());
        int assocs = 0;
        cim.hook = [&](const QString& call) {
            if (call.contains(QLatin1String("ASSOCIATORS OF")) && ++assocs == 3)
                throw win::cim::CimError(0x80041001, QStringLiteral("Generic failure"));
        };
        CimFirewallMonitor mon(cim, kExe);
        mon.threadInit();
        QCOMPARE(mon.checkOnce(config(), kGen), Check::Failed);
        // a timed-out enumeration is the same
        cim.hook = [](const QString& call) {
            if (call.contains(QLatin1String("RuleGroup"))) throw win::cim::CimError(win::cim::kWbemTimedOut, QStringLiteral("timed out"));
        };
        QCOMPARE(mon.checkOnce(config(), kGen), Check::Failed);
    }

    void emptyPlanFailedEnumerationFails()
    {
        BridgeConfig empty;
        FakeCim cim;
        CimFirewallMonitor mon(cim, kExe);
        mon.threadInit();
        QCOMPARE(mon.checkOnce(empty, kGen), Check::Match);   // a completed, empty read
        cim.hook = [](const QString& call) {
            if (call.startsWith(QLatin1String("query"))) throw win::cim::CimError(0x80041001, QStringLiteral("Generic failure"));
        };
        QCOMPARE(mon.checkOnce(empty, kGen), Check::Failed);
    }

    void emptyGroupMismatchesANonEmptyPlan()
    {
        FakeCim cim;
        QCOMPARE(verdict(cim).check, Check::Mismatch);
    }

    void monitorReadsTheActiveStore()
    {
        FakeCim cim;
        written(cim, plan());
        CimFirewallMonitor mon(cim, kExe);
        mon.threadInit();
        QCOMPARE(mon.checkOnce(config(), kGen), Check::Match);
        const QStringList queries = cim.calls(QStringLiteral("query"));
        QVERIFY(!queries.isEmpty());
        for (const QString& q : queries) QVERIFY2(q.startsWith(QLatin1String("query ActiveStore ")), qPrintable(q));
        // a rule visible only in the ActiveStore (group policy) in the reserved group is seen
        cim.gpoRules.insert(QStringLiteral("gpo-rule"),
                            FakeCim::asWritten(QStringLiteral("gpo-rule"), kGroup, QStringLiteral("23"), {QStringLiteral("Any")}, kExe));
        QCOMPARE(mon.checkOnce(config(), kGen), Check::Mismatch);
    }

    void monitorConnectFailureFails()
    {
        FakeCim cim;
        written(cim, plan());
        cim.hook = [](const QString& call) {
            if (call == QLatin1String("connect")) throw win::cim::CimError(0x80070005, QStringLiteral("Access denied"));
        };
        CimFirewallMonitor mon(cim, kExe);
        mon.threadInit();
        QCOMPARE(mon.checkOnce(config(), kGen), Check::Failed);
    }

    // ---- RemoteAddress normalisation ----
    void writerValues_data()
    {
        QTest::addColumn<QString>("token");
        QTest::addColumn<QString>("written");
        QTest::newRow("address") << QStringLiteral("192.168.1.10") << QStringLiteral("192.168.1.10");
        QTest::newRow("octal group (engine semantics)") << QStringLiteral("010.0.0.1") << QStringLiteral("8.0.0.1");
        QTest::newRow("prefix masked") << QStringLiteral("10.1.2.3/8") << QStringLiteral("10.0.0.0/8");
        QTest::newRow("full-length prefix") << QStringLiteral("10.1.2.3/32") << QStringLiteral("10.1.2.3");
        QTest::newRow("range") << QStringLiteral("10.0.0.1-10.0.0.9") << QStringLiteral("10.0.0.1-10.0.0.9");
        QTest::newRow("range of one") << QStringLiteral("10.0.0.1-10.0.0.1") << QStringLiteral("10.0.0.1");
        QTest::newRow("v6") << QStringLiteral("2001:DB8:0:0::1") << QStringLiteral("2001:db8::1");
        QTest::newRow("v6 prefix") << QStringLiteral("2001:db8:1::/32") << QStringLiteral("2001:db8::/32");
        // WS-13: a /0 as the whole range (the provider stores "0.0.0.0/0" as the address 0.0.0.0).
        QTest::newRow("v4 /0 as the whole range") << QStringLiteral("0.0.0.0/0") << QStringLiteral("0.0.0.0-255.255.255.255");
        QTest::newRow("v4 /0 of another address") << QStringLiteral("10.1.2.3/0") << QStringLiteral("0.0.0.0-255.255.255.255");
        QTest::newRow("v6 /0 as the whole range") << QStringLiteral("::/0")
                                                 << QStringLiteral("::-ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff");
        QTest::newRow("v4 /1 stays a prefix") << QStringLiteral("128.0.0.0/1") << QStringLiteral("128.0.0.0/1");
        QTest::newRow("keyword canonical case") << QStringLiteral("localsubnet") << QStringLiteral("LocalSubnet");
        QTest::newRow("keyword LocalSubnet4") << QStringLiteral("LOCALSUBNET4") << QStringLiteral("LocalSubnet4");
        QTest::newRow("Any") << QStringLiteral("any") << QStringLiteral("Any");
        QTest::newRow("not accepted") << QStringLiteral("PlayToTargets") << QStringLiteral("");
        QTest::newRow("garbage") << QStringLiteral("10.0.0") << QStringLiteral("");
    }
    void writerValues()
    {
        QFETCH(QString, token);
        QFETCH(QString, written);
        const auto v = FirewallCompare::writerValue(token);
        if (written.isEmpty()) QVERIFY(!v);
        else QCOMPARE(v.value_or(QStringLiteral("<none>")), written);
    }

    void addressKeys()
    {
        using FirewallCompare::addressKey;
        QCOMPARE(addressKey(QStringLiteral("192.168.1.0/255.255.255.0")), addressKey(QStringLiteral("192.168.1.0/24")));
        QCOMPARE(addressKey(QStringLiteral("10.0.0.5/255.255.255.255")), addressKey(QStringLiteral("10.0.0.5")));
        QVERIFY(!addressKey(QStringLiteral("10.0.0.0/255.0.255.0")));   // not a contiguous mask
        QVERIFY(addressKey(QStringLiteral("10.0.0.5")) != addressKey(QStringLiteral("10.0.0.5/31")));
        QCOMPARE(addressKey(QStringLiteral("PlayToTargets")), std::optional<QString>(QStringLiteral("K:playtotargets")));
        // WS-13: the whole-family range and the /0 prefix are one set; neither is Any (Any also admits the other family),
        // and the single address 0.0.0.0 (what the provider made of "0.0.0.0/0") is not it either.
        QCOMPARE(addressKey(QStringLiteral("0.0.0.0-255.255.255.255")), addressKey(QStringLiteral("0.0.0.0/0")));
        QCOMPARE(addressKey(QStringLiteral("::-ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff")), addressKey(QStringLiteral("::/0")));
        QVERIFY(addressKey(QStringLiteral("0.0.0.0/0")) != addressKey(QStringLiteral("0.0.0.0")));
        QVERIFY(addressKey(QStringLiteral("0.0.0.0/0")) != addressKey(QStringLiteral("Any")));
        QVERIFY(addressKey(QStringLiteral("0.0.0.1-255.255.255.255")) != addressKey(QStringLiteral("0.0.0.0/0")));
        QCOMPARE(*FirewallCompare::addressSet({}, true), QStringList{QStringLiteral("K:any")});
        QCOMPARE(*FirewallCompare::addressSet({QStringLiteral("10.0.0.1"), QStringLiteral("Any")}, false),
                 QStringList{QStringLiteral("K:any")});
    }
};

QTEST_GUILESS_MAIN(tst_FirewallCompare)
#include "tst_FirewallCompare.moc"
