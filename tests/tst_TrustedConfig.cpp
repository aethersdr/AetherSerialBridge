// TrustedConfig: the mode table of master §4.4, on FakeFolder (recreation is Repair's, which renames aside
// and deletes nothing). INV-22 (Service never
// mutates), INV-44 (ReadOnly never writes), INV-21 (recreation only in Repair; notice after success),
// INV-74 (rename aside, nothing deleted).

#include "core/ConfigFiles.h"
#include "core/TrustedConfig.h"
#include "support/Fakes.h"

#include <QTest>

using namespace asb;
using namespace asb::test;

namespace {
const QString kConfig = QString::fromLatin1(ConfigFiles::kConfig);

struct Folder {
    FakeWorld world;
    FakeFolder folder{world};
    Folder() { world.checkLock = false; }
    void writeConfig(const QByteArray& bytes, const QString& owner = QStringLiteral("root"))
    {
        world.files.plantFile(kConfig, bytes, owner);
    }
};

enum class State { Trust, UntrustedOwner, UntrustedChild, Link, Missing };

void setState(Folder& f, State s)
{
    switch (s) {
    case State::Trust:
        break;
    case State::UntrustedOwner:
        f.folder.owner = QStringLiteral("attacker");
        break;
    case State::UntrustedChild:
        f.world.files.plantFile(QStringLiteral("planted.txt"), "x");   // owned by "attacker"
        break;
    case State::Link:
        f.folder.state = FakeFolder::DirState::Link;
        break;
    case State::Missing:
        f.folder.state = FakeFolder::DirState::Missing;
        break;
    }
}

void addStateRows()
{
    QTest::addColumn<int>("state");
    QTest::addColumn<bool>("hasConfig");
    QTest::newRow("trusted") << int(State::Trust) << true;
    QTest::newRow("untrusted owner, config") << int(State::UntrustedOwner) << true;
    QTest::newRow("untrusted owner, no config") << int(State::UntrustedOwner) << false;
    QTest::newRow("untrusted child") << int(State::UntrustedChild) << true;
    QTest::newRow("link at the path") << int(State::Link) << true;
    QTest::newRow("missing") << int(State::Missing) << false;
}
}  // namespace

class tst_TrustedConfig : public QObject {
    Q_OBJECT
private slots:
    void aTrustedFolderLoadsInEveryMode_data();
    void aTrustedFolderLoadsInEveryMode();
    void serviceModeNeverMutates_data();
    void serviceModeNeverMutates();
    void readOnlyNeverMutates_data();
    void readOnlyNeverMutates();
    void mutatingCreatesAMissingFolderAndNormalises();
    void mutatingRefusesAnUntrustedFolderWithoutRecreating();
    void aLinkIsRefusedInEveryModeEvenRepair();
    void repairRenamesAsideAndDeletesNothing();
    void repairNoticeOnlyAfterRename();
    void repairOfATrustedFolderOnlyNormalises();
    void normaliseChangesAreReported();
    void normaliseChangesReportedWhenALaterStepFails();
    void readsGoThroughTheInspectedFolder();
    void aFolderTamperedAfterTheConfigWasWrittenIsRefused();
};

void tst_TrustedConfig::aTrustedFolderLoadsInEveryMode_data()
{
    QTest::addColumn<int>("mode");
    for (TrustMode m : {TrustMode::ReadOnly, TrustMode::Mutating, TrustMode::Repair, TrustMode::Service, TrustMode::Scratch})
        QTest::addRow("mode %d", int(m)) << int(m);
}

void tst_TrustedConfig::aTrustedFolderLoadsInEveryMode()
{
    QFETCH(int, mode);
    Folder f;
    f.writeConfig(configA());
    const OpenedFolder o = TrustedConfig::open(f.folder, TrustMode(mode));
    QVERIFY2(o.usable, qPrintable(o.error));
    const ConfigFiles::Loaded l = TrustedConfig::readCommitted(f.folder, o.inspection);
    QVERIFY(l.config);
    QCOMPARE(l.config->ports.value(0).name, QStringLiteral("Amp"));
    QCOMPARE(f.folder.renames, 0);
    QCOMPARE(f.folder.creates, 0);
}

// INV-22. Kills: Service mode recreating, creating or securing anything.
void tst_TrustedConfig::serviceModeNeverMutates_data()
{
    addStateRows();
}

void tst_TrustedConfig::serviceModeNeverMutates()
{
    QFETCH(int, state);
    QFETCH(bool, hasConfig);
    for (TrustMode mode : {TrustMode::Service, TrustMode::Scratch}) {
        Folder f;
        if (hasConfig) f.writeConfig(configA());
        setState(f, State(state));
        const QStringList before = f.world.files.log;
        const OpenedFolder o = TrustedConfig::open(f.folder, mode);
        QCOMPARE(o.usable, State(state) == State::Trust);
        QCOMPARE(f.folder.mutations(), 0);
        QCOMPARE(f.world.files.log, before);   // no file operation either
        QVERIFY(o.fileChanges.isEmpty());
        if (!o.usable) QVERIFY(o.error.contains(QStringLiteral("No ports are bridged")));
    }
}

// INV-44. Kills: ReadOnly normalising (or creating) anything.
void tst_TrustedConfig::readOnlyNeverMutates_data()
{
    addStateRows();
}

void tst_TrustedConfig::readOnlyNeverMutates()
{
    QFETCH(int, state);
    QFETCH(bool, hasConfig);
    Folder f;
    if (hasConfig) f.writeConfig(configA());
    setState(f, State(state));
    const QStringList before = f.world.files.log;
    const OpenedFolder o = TrustedConfig::open(f.folder, TrustMode::ReadOnly);
    QCOMPARE(f.folder.mutations(), 0);
    QCOMPARE(f.world.files.log, before);
    QCOMPARE(o.usable, State(state) == State::Trust);
    QCOMPARE(o.missing, State(state) == State::Missing);
    if (o.missing) QVERIFY(o.error.contains(QStringLiteral("not been created yet")));
}

void tst_TrustedConfig::mutatingCreatesAMissingFolderAndNormalises()
{
    Folder f;
    setState(f, State::Missing);
    const OpenedFolder o = TrustedConfig::open(f.folder, TrustMode::Mutating);
    QVERIFY2(o.usable, qPrintable(o.error));
    QCOMPARE(f.folder.creates, 1);
    QCOMPARE(f.folder.normalises, 1);
    QCOMPARE(f.folder.renames, 0);
    QVERIFY(o.fileChanges.contains(QStringLiteral("created the data folder")));
}

// INV-21. Kills: recreating (renaming aside) outside Repair.
void tst_TrustedConfig::mutatingRefusesAnUntrustedFolderWithoutRecreating()
{
    for (State s : {State::UntrustedOwner, State::UntrustedChild}) {
        Folder f;
        f.writeConfig(configA());
        setState(f, s);
        const OpenedFolder o = TrustedConfig::open(f.folder, TrustMode::Mutating);
        QVERIFY(!o.usable);
        QVERIFY2(o.error.contains(QStringLiteral("Run Repair")), qPrintable(o.error));
        QCOMPARE(f.folder.mutations(), 0);
        QVERIFY(f.world.files.exists(kConfig));
    }
}

void tst_TrustedConfig::aLinkIsRefusedInEveryModeEvenRepair()
{
    for (TrustMode m : {TrustMode::ReadOnly, TrustMode::Mutating, TrustMode::Repair, TrustMode::Service}) {
        Folder f;
        setState(f, State::Link);
        const OpenedFolder o = TrustedConfig::open(f.folder, m);
        QVERIFY(!o.usable);
        QCOMPARE(f.folder.mutations(), 0);
    }
}

// INV-74 (pure half). Kills: Repair deleting, or rewriting, the untrusted folder's contents.
void tst_TrustedConfig::repairRenamesAsideAndDeletesNothing()
{
    Folder f;
    f.writeConfig(configB(), QStringLiteral("attacker"));
    f.world.files.plantFile(QStringLiteral("start.token"), "{\"revoked\":true}", QStringLiteral("root"));
    const OpenedFolder o = TrustedConfig::open(f.folder, TrustMode::Repair);
    QVERIFY2(o.usable, qPrintable(o.error));
    QCOMPARE(f.folder.renames, 1);
    QCOMPARE(f.folder.creates, 1);
    QCOMPARE(f.folder.aside.value(kConfig), configB());              // moved, intact
    QVERIFY(f.folder.aside.contains(QStringLiteral("start.token")));  // the token went with it
    QVERIFY(f.world.files.names().isEmpty());                         // the new folder starts empty
    QVERIFY(o.fileChanges.join(QLatin1Char(' ')).contains(f.folder.asidePath));
    QVERIFY(!f.world.files.log.join(QLatin1Char(' ')).contains(QStringLiteral("remove")));
}

// INV-21. Kills: announcing the move before the rename succeeded.
void tst_TrustedConfig::repairNoticeOnlyAfterRename()
{
    Folder f;
    f.writeConfig(configB(), QStringLiteral("attacker"));
    f.folder.failRenameAside = QStringLiteral("access denied");
    const OpenedFolder o = TrustedConfig::open(f.folder, TrustMode::Repair);
    QVERIFY(!o.usable);
    QVERIFY(o.error.contains(QStringLiteral("access denied")));
    QVERIFY2(!o.fileChanges.join(QLatin1Char(' ')).contains(QStringLiteral("moved")), qPrintable(o.fileChanges.join(';')));
    QCOMPARE(f.folder.creates, 0);
    QCOMPARE(f.world.files.contents(kConfig), configB());
}

void tst_TrustedConfig::repairOfATrustedFolderOnlyNormalises()
{
    Folder f;
    f.writeConfig(configA());
    const OpenedFolder o = TrustedConfig::open(f.folder, TrustMode::Repair);
    QVERIFY(o.usable);
    QCOMPARE(f.folder.renames, 0);
    QCOMPARE(f.folder.creates, 0);
    QCOMPARE(f.folder.normalises, 1);
}

void tst_TrustedConfig::normaliseChangesAreReported()
{
    Folder f;
    f.folder.normaliseChanges = {QStringLiteral("reset the permissions of config.json")};
    const OpenedFolder o = TrustedConfig::open(f.folder, TrustMode::Mutating);
    QVERIFY(o.usable);
    QCOMPARE(o.fileChanges, f.folder.normaliseChanges);
}

// Changes made before a failure stay on disk, so they are reported with it.
void tst_TrustedConfig::normaliseChangesReportedWhenALaterStepFails()
{
    Folder f;
    f.folder.normaliseChanges = {QStringLiteral("secured the data folder"), QStringLiteral("reset the permissions of config.json")};
    f.folder.failNormalise = QStringLiteral("could not reset the permissions of status.json");
    for (TrustMode m : {TrustMode::Mutating, TrustMode::Repair}) {
        const OpenedFolder o = TrustedConfig::open(f.folder, m);
        QVERIFY(!o.usable);
        QVERIFY2(o.error.contains(QStringLiteral("status.json")), qPrintable(o.error));
        QCOMPARE(o.fileChanges, f.folder.normaliseChanges);
    }
}

void tst_TrustedConfig::readsGoThroughTheInspectedFolder()
{
    Folder f;
    f.writeConfig(configA());
    f.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configB(), QStringLiteral("root"));
    const OpenedFolder o = TrustedConfig::open(f.folder, TrustMode::ReadOnly);
    QCOMPARE(TrustedConfig::readCommitted(f.folder, o.inspection).bytes, configA());
    QCOMPARE(TrustedConfig::readPending(f.folder, o.inspection).bytes, configB());
    QCOMPARE(f.folder.reads, (QStringList{kConfig, QString::fromLatin1(ConfigFiles::kPending)}));
}

// Kills: reading config.json without the trusted check.
void tst_TrustedConfig::aFolderTamperedAfterTheConfigWasWrittenIsRefused()
{
    Folder f;
    f.writeConfig(configA());
    f.folder.owner = QStringLiteral("attacker");
    const OpenedFolder o = TrustedConfig::open(f.folder, TrustMode::Mutating);
    QVERIFY(!o.usable);
    QVERIFY(f.folder.reads.isEmpty());
}

QTEST_GUILESS_MAIN(tst_TrustedConfig)
#include "tst_TrustedConfig.moc"
