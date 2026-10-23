// ServicePlan (created/configured disabled; restart 5 s / 5 s / 30 s,
// reset one day, failure flag), and INV-77's start-type durability barrier (`flushAfterStartType`,
// the WS-3 deferred row): every start-type change is followed by its flush before anything else.

#include "core/ServicePlan.h"

#include <QTest>

using namespace asb;
using namespace asb::ServicePlan;

class tst_ServicePlan : public QObject {
    Q_OBJECT
private slots:
    void windowsServiceIsCreatedDisabledWithRecovery();
    void binPathQuotesTheProgram();
    void flushAfterStartType_data();
    void flushAfterStartType();
    void blockAndAllowAreOpposites();
};

void tst_ServicePlan::windowsServiceIsCreatedDisabledWithRecovery()
{
    const ScmConfig c = windowsConfig(QStringLiteral(R"(C:\Program Files\AetherSerialBridge\aether-serial-bridge.exe)"));
    QCOMPARE(c.serviceName, QStringLiteral("AetherSerialBridge"));
    QCOMPARE(c.account, QStringLiteral("LocalSystem"));
    QVERIFY(c.startType == ScmConfig::StartType::Disabled);
    QCOMPARE(c.failureActions, (QList<FailureAction>{{FailureAction::Type::Restart, 5000},
                                                     {FailureAction::Type::Restart, 5000},
                                                     {FailureAction::Type::Restart, 30000}}));
    QCOMPARE(c.failureResetSeconds, 86400);
    QVERIFY(c.failureActionsOnNonCrashFailures);
    QVERIFY(c.description.contains(QStringLiteral("config.json")));
}

void tst_ServicePlan::binPathQuotesTheProgram()
{
    const ScmConfig c = windowsConfig(QStringLiteral(R"(C:\Program Files\AetherSerialBridge\aether-serial-bridge.exe)"));
    QCOMPARE(c.binaryPath, QStringLiteral(R"("C:\Program Files\AetherSerialBridge\aether-serial-bridge.exe" --service)"));
}

// INV-77. Kills: a start-type change (disable/mask, enable/unmask) not followed by its durability
// barrier, or any step after the last barrier.
void tst_ServicePlan::flushAfterStartType_data()
{
    QTest::addColumn<int>("os");
    QTest::addColumn<bool>("block");
    QTest::newRow("Windows block") << int(Os::Windows) << true;
    QTest::newRow("Windows allow") << int(Os::Windows) << false;
    QTest::newRow("Linux block") << int(Os::Linux) << true;
    QTest::newRow("Linux allow") << int(Os::Linux) << false;
}

void tst_ServicePlan::flushAfterStartType()
{
    QFETCH(int, os);
    QFETCH(bool, block);
    const QList<Step> steps = block ? blockStarts(Os(os)) : allowStarts(Os(os));
    QVERIFY(!steps.isEmpty());
    bool changed = false, pending = false;
    for (const Step& s : steps) {
        if (changesStartType(s.kind)) changed = pending = true;
        if (isDurabilityBarrier(s.kind)) pending = false;
    }
    QVERIFY(changed);
    QVERIFY2(!pending, "a start-type change is not followed by its durability barrier");
    QVERIFY(isDurabilityBarrier(steps.last().kind));
}

void tst_ServicePlan::blockAndAllowAreOpposites()
{
    QCOMPARE(blockStarts(Os::Windows).first().kind, Step::Kind::SetStartTypeDisabled);
    QCOMPARE(allowStarts(Os::Windows).first().kind, Step::Kind::SetStartTypeAutomatic);
    QCOMPARE(blockStarts(Os::Linux).first().kind, Step::Kind::MaskUnit);
    QCOMPARE(allowStarts(Os::Linux).first().kind, Step::Kind::UnmaskUnit);
}

QTEST_GUILESS_MAIN(tst_ServicePlan)
#include "tst_ServicePlan.moc"
