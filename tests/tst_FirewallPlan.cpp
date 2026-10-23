// FirewallPlan, the pure half (WS-5; the native CIM half is WS-7's): one rule per enabled port, generation names, add-then-swap, a
// rollback that removes only this generation — without any script text (D51): every value is a typed
// field (INV-12), and an unvalidated config never reaches a plan.

#include "core/AllowedClients.h"
#include "core/ConfigJson.h"
#include "core/FirewallPlan.h"

#include <QTest>

using namespace asb;
using namespace asb::FirewallPlan;

namespace {
const FirewallGeneration kGen{QByteArrayLiteral("0123456789abcdef")};
const QString kExe = QStringLiteral(R"(C:\Program Files\AetherSerialBridge\aether-serial-bridge.exe)");

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
    c.ports = {port(QStringLiteral("Amp"), QStringLiteral("COM3"), 4532, QStringLiteral("192.168.1.10, 10.0.0.0/8")),
               port(QStringLiteral("Tuner"), QStringLiteral("COM4"), 4533),
               port(QStringLiteral("Off"), QStringLiteral("COM9"), 4534, {}, false)};
    return c;
}

Plan plan(const BridgeConfig& c = config(), const FirewallGeneration& g = kGen)
{
    QString err;
    const std::optional<Plan> p = build(c, g, kExe, &err);
    if (!p) qFatal("plan failed: %s", qPrintable(err));
    return *p;
}

int indexOf(const QList<Step>& steps, Step::Kind k, const QString& id)
{
    for (int i = 0; i < steps.size(); ++i)
        if (steps[i].kind == k && steps[i].instanceId == id) return i;
    return -1;
}
}  // namespace

class tst_FirewallPlan : public QObject {
    Q_OBJECT
private slots:
    void eachEnabledPortGetsOneRuleWithAGenerationName();
    void valuesAreTypedProperties();
    void unvalidatedConfigRefused();
    void addressesComeFromTheTokeniser();
    void trailingNewlineNeverReachesARule();
    void generationMustBeSixteenHex_data();
    void generationMustBeSixteenHex();
    void ruleNamesParseOnlyOurForm_data();
    void ruleNamesParseOnlyOurForm();
    // INV-11 (pure plan order; the native rows are WS-7's)
    void oldRemovedOnlyAfterEveryAdd();
    void eachRuleEnabledOnlyAfterItsThreeFilters();
    void rollbackRemovesOnlyThisGeneration();
    void sweepRemovesEverythingNotOfTheGeneration();
    void freshGenerationsAreValidAndDistinct();
};

void tst_FirewallPlan::eachEnabledPortGetsOneRuleWithAGenerationName()
{
    const Plan p = plan();
    QCOMPARE(p.rules.size(), 2);
    QCOMPARE(p.rules[0].instanceId, QStringLiteral("AetherSerialBridge-0123456789abcdef-0"));
    QCOMPARE(p.rules[1].instanceId, QStringLiteral("AetherSerialBridge-0123456789abcdef-1"));
    QCOMPARE(p.rules[0].localPorts, QStringList{QStringLiteral("4532")});
    QCOMPARE(p.rules[0].remoteAddresses, (QStringList{QStringLiteral("192.168.1.10"), QStringLiteral("10.0.0.0/8")}));
    QCOMPARE(p.rules[1].remoteAddresses, QStringList{QStringLiteral("Any")});
    for (const Rule& r : p.rules) {
        QCOMPARE(r.group, QStringLiteral("Aether Serial Bridge"));
        QCOMPARE(r.protocol, QStringLiteral("TCP"));
        QCOMPARE(r.program, kExe);
        QVERIFY(r.direction == Rule::Direction::Inbound && r.action == Rule::Action::Allow && r.profiles == Rule::Profiles::Any);
        QVERIFY(!r.localPorts.contains(QStringLiteral("4534")));
    }
}

// INV-12. Kills: building command/script text from configuration values (any quoting or escaping).
void tst_FirewallPlan::valuesAreTypedProperties()
{
    BridgeConfig c;
    c.ports = {port(QStringLiteral("KPA (500) - main.1"), QStringLiteral("COM3"), 4532)};
    const QString exe = QStringLiteral(R"(C:\it's\x.exe)");
    QString err;
    const std::optional<Plan> p = build(c, kGen, exe, &err);
    QVERIFY2(p, qPrintable(err));
    // Verbatim: no doubled quotes, no surrounding quotes, nothing interpreted.
    QCOMPARE(p->rules[0].displayName, QStringLiteral("Aether Serial Bridge - KPA (500) - main.1 (TCP 4532)"));
    QCOMPARE(p->rules[0].program, exe);
    QCOMPARE(p->rules[0].localPorts, QStringList{QStringLiteral("4532")});
    // Every field is its own value; none carries another's text or a command separator.
    for (const QString& v : QStringList{p->rules[0].displayName, p->rules[0].program, p->rules[0].instanceId,
                                        p->rules[0].group, p->rules[0].protocol} +
                                p->rules[0].localPorts + p->rules[0].remoteAddresses) {
        QVERIFY2(!v.contains(QStringLiteral("''")) && !v.contains(QLatin1Char(';')) && !v.contains(QStringLiteral("-Name")),
                 qPrintable(v));
    }
}

// INV-12. Kills: passing an unvalidated name (or allow-list) into the plan.
void tst_FirewallPlan::unvalidatedConfigRefused()
{
    for (const QString& name : {QStringLiteral("x'; Remove-Item C:\\"), QStringLiteral("a\"b")}) {
        BridgeConfig c;
        c.ports = {port(name, QStringLiteral("COM3"), 4532)};
        QVERIFY(!c.validate().isEmpty());
        QString err;
        QVERIFY(!build(c, kGen, kExe, &err));
        QVERIFY(!err.isEmpty());
    }
    BridgeConfig c;
    c.ports = {port(QStringLiteral("Amp"), QStringLiteral("COM3"), 4532, QStringLiteral("10.0.0.1'; x"))};
    QString err;
    QVERIFY(!build(c, kGen, kExe, &err));
}

// INV-12/INV-14: the remote addresses are exactly AllowedClients::tokens(); blank is the only "Any".
void tst_FirewallPlan::addressesComeFromTheTokeniser()
{
    for (const QString& list : {QStringLiteral(" 10.0.0.1 ,192.168.1.0/24,  LocalSubnet"), QStringLiteral("10.0.0.1\n"),
                                QStringLiteral(" , 10.0.0.2, ")}) {
        BridgeConfig c;
        c.ports = {port(QStringLiteral("Amp"), QStringLiteral("COM3"), 4532, list)};
        QString err;
        const std::optional<Plan> p = build(c, kGen, kExe, &err);
        const std::optional<QStringList> tokens = AllowedClients::tokens(list);
        QCOMPARE(p.has_value(), tokens.has_value());
        if (p) QCOMPARE(p->rules[0].remoteAddresses, tokens->isEmpty() ? QStringList{QStringLiteral("Any")} : *tokens);
    }
    BridgeConfig c;
    c.ports = {port(QStringLiteral("Amp"), QStringLiteral("COM3"), 4532, QStringLiteral("10.0.0.1, nonsense"))};
    QString err;
    QVERIFY(!build(c, kGen, kExe, &err));   // invalid: refused, never "Any"
}

void tst_FirewallPlan::trailingNewlineNeverReachesARule()
{
    BridgeConfig c;
    c.ports = {port(QStringLiteral("KAT500\n"), QStringLiteral("COM3"), 4532),
               port(QStringLiteral(""), QStringLiteral("COM4\n"), 4533)};
    QString err;
    const std::optional<Plan> p = build(c, kGen, kExe, &err);
    QVERIFY2(p, qPrintable(err));
    QCOMPARE(p->rules[0].displayName, QStringLiteral("Aether Serial Bridge - KAT500 (TCP 4532)"));
    QCOMPARE(p->rules[1].displayName, QStringLiteral("Aether Serial Bridge - COM4 (TCP 4533)"));
    for (const Rule& r : p->rules) QVERIFY(!r.displayName.contains(QLatin1Char('\n')));
}

void tst_FirewallPlan::generationMustBeSixteenHex_data()
{
    QTest::addColumn<QByteArray>("id");
    QTest::newRow("quote") << QByteArray("x'; Remove-Item C:\\");
    QTest::newRow("upper") << QByteArray("0123456789ABCDEF");
    QTest::newRow("short") << QByteArray("0123");
    QTest::newRow("32 hex") << QByteArray("0123456789abcdef0123456789abcdef");
    QTest::newRow("empty") << QByteArray();
}

void tst_FirewallPlan::generationMustBeSixteenHex()
{
    QFETCH(QByteArray, id);
    QString err;
    QVERIFY(!build(config(), FirewallGeneration{id}, kExe, &err));
}

void tst_FirewallPlan::ruleNamesParseOnlyOurForm_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<bool>("ours");
    QTest::newRow("ours") << QStringLiteral("AetherSerialBridge-0123456789abcdef-0") << true;
    QTest::newRow("index 999") << QStringLiteral("AetherSerialBridge-0123456789abcdef-999") << true;
    QTest::newRow("leading zero") << QStringLiteral("AetherSerialBridge-0123456789abcdef-01") << false;
    QTest::newRow("index 1000") << QStringLiteral("AetherSerialBridge-0123456789abcdef-1000") << false;
    QTest::newRow("32-hex generation") << QStringLiteral("AetherSerialBridge-0123456789abcdef0123456789abcdef-0") << false;
    QTest::newRow("upper hex") << QStringLiteral("AetherSerialBridge-0123456789ABCDEF-0") << false;
    QTest::newRow("foreign") << QStringLiteral("Allow 4532") << false;
    QTest::newRow("suffix") << QStringLiteral("AetherSerialBridge-0123456789abcdef-0\n") << false;
}

void tst_FirewallPlan::ruleNamesParseOnlyOurForm()
{
    QFETCH(QString, name);
    QFETCH(bool, ours);
    QCOMPARE(parseRuleName(name).has_value(), ours);
}

// INV-11. Kills: removing old rules before every new one is
// enabled.
void tst_FirewallPlan::oldRemovedOnlyAfterEveryAdd()
{
    const QStringList existing = {QStringLiteral("AetherSerialBridge-ffffffffffffffff-0"), QStringLiteral("Stray rule")};
    const QList<Step> steps = syncSteps(plan(), existing);
    int lastEnable = -1, firstDelete = int(steps.size());
    for (int i = 0; i < steps.size(); ++i) {
        if (steps[i].kind == Step::Kind::Enable) lastEnable = i;
        if (steps[i].kind == Step::Kind::Delete) firstDelete = qMin(firstDelete, i);
    }
    QVERIFY(lastEnable >= 0);
    QVERIFY2(lastEnable < firstDelete, "an old rule is deleted before every new rule is enabled");
    QVERIFY(indexOf(steps, Step::Kind::Delete, existing[0]) >= 0);
    QVERIFY(indexOf(steps, Step::Kind::Delete, existing[1]) >= 0);
}

void tst_FirewallPlan::eachRuleEnabledOnlyAfterItsThreeFilters()
{
    const Plan p = plan();
    const QList<Step> steps = syncSteps(p, {});
    for (const Rule& r : p.rules) {
        const int create = indexOf(steps, Step::Kind::CreateDisabled, r.instanceId);
        const int enable = indexOf(steps, Step::Kind::Enable, r.instanceId);
        QVERIFY(create >= 0 && enable > create);
        for (Step::Kind k : {Step::Kind::SetPortFilter, Step::Kind::SetAddressFilter, Step::Kind::SetApplicationFilter}) {
            const int f = indexOf(steps, k, r.instanceId);
            QVERIFY(f > create && f < enable);
        }
    }
}

void tst_FirewallPlan::rollbackRemovesOnlyThisGeneration()
{
    const QStringList existing = {QStringLiteral("AetherSerialBridge-ffffffffffffffff-0"),
                                  QStringLiteral("AetherSerialBridge-0123456789abcdef-0"),
                                  QStringLiteral("AetherSerialBridge-0123456789abcdef-1"), QStringLiteral("Stray")};
    const QList<Step> steps = rollbackSteps(kGen, existing);
    QCOMPARE(steps.size(), 2);
    QCOMPARE(steps[0].instanceId, existing[1]);
    QCOMPARE(steps[1].instanceId, existing[2]);
}

void tst_FirewallPlan::sweepRemovesEverythingNotOfTheGeneration()
{
    const QStringList existing = {QStringLiteral("AetherSerialBridge-ffffffffffffffff-0"),
                                  QStringLiteral("AetherSerialBridge-0123456789abcdef-0"), QStringLiteral("Stray")};
    const QList<Step> steps = sweepSteps(kGen, existing);
    QCOMPARE(steps.size(), 2);
    QCOMPARE(steps[0].instanceId, existing[0]);
    QCOMPARE(steps[1].instanceId, existing[2]);
    QCOMPARE(removeAllSteps(existing).size(), 3);
}

void tst_FirewallPlan::freshGenerationsAreValidAndDistinct()
{
    const FirewallGeneration a = FirewallGeneration::fresh(), b = FirewallGeneration::fresh();
    QVERIFY(FirewallGeneration::isValidId(a.id));
    QVERIFY(FirewallGeneration::isValidId(b.id));
    QVERIFY(a.id != b.id);
}

QTEST_GUILESS_MAIN(tst_FirewallPlan)
#include "tst_FirewallPlan.moc"
