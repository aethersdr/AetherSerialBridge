// DataDirectoryTrust::decide (with INV-63: the bridge's known file names, and our temps, must be regular
// files). Recreation is only Repair's rename-aside (S2), tested in tst_TrustedConfig and tst_Applier.

#include "core/DataDirectoryTrust.h"

#include <QTest>

using namespace asb;

namespace {
const QString System = QStringLiteral("S-1-5-18"), Admins = QStringLiteral("S-1-5-32-544"),
              SomeUser = QStringLiteral("S-1-5-21-1111111111-2222222222-3333333333-1001"),
              Users = QStringLiteral("S-1-5-32-545");

FsEntry dir(std::optional<QString> owner, FsEntry::Kind kind = FsEntry::Kind::Directory)
{
    return FsEntry{QStringLiteral("AetherSerialBridge"), owner, kind, false, {}};
}
FsEntry child(const QString& name, std::optional<QString> owner, FsEntry::Kind kind = FsEntry::Kind::File)
{
    return FsEntry{name, owner, kind, false, {}};
}
DirAction decide(const std::optional<FsEntry>& d, const QList<FsEntry>& children = {},
                 const TrustPolicy& p = TrustPolicy::windows())
{
    return DataDirectoryTrust::decide(d, children, p).action;
}
}  // namespace

class tst_DataDirectoryTrust : public QObject {
    Q_OBJECT
private slots:
    void missingDirectoryIsCreated();
    void directoryOwnedBySystemOrAdministratorsIsTrusted_data();
    void directoryOwnedBySystemOrAdministratorsIsTrusted();
    void directoryOwnedByAnyoneElseIsRecreated_data();
    void directoryOwnedByAnyoneElseIsRecreated();
    void reparsePointOrFileAtThePathIsRefused_data();
    void reparsePointOrFileAtThePathIsRefused();
    void childOwnedByAUserForcesRecreate();
    void aChildThatVanishedIsNotEvidence();
    void aVanishedChildDoesNotHideAnUntrustedOne();
    void childWithUnreadableOwnerForcesRecreate();
    void reparsePointChildForcesRecreateEvenWhenOwnedByAdmins();
    void theRunningAdministratorsOwnFilesAreTrustedButNobodyElses();
    void onlySystemAndAdministratorsAreTrustedOwners();
    // INV-63
    void knownNamesMustBeRegularFiles_data();
    void knownNamesMustBeRegularFiles();
    void anOrdinarySubdirectoryIsNotAKnownName();
    void knownNamesMatchCaseInsensitively();
};

void tst_DataDirectoryTrust::missingDirectoryIsCreated()
{
    QCOMPARE(decide(std::nullopt), DirAction::Create);
}

void tst_DataDirectoryTrust::directoryOwnedBySystemOrAdministratorsIsTrusted_data()
{
    QTest::addColumn<QString>("owner");
    QTest::newRow("SYSTEM") << System;
    QTest::newRow("Administrators") << Admins;
}

void tst_DataDirectoryTrust::directoryOwnedBySystemOrAdministratorsIsTrusted()
{
    QFETCH(QString, owner);
    QCOMPARE(decide(dir(owner), {child(QStringLiteral("config.json"), Admins), child(QStringLiteral("bridge.log"), System)}),
             DirAction::Trust);
}

void tst_DataDirectoryTrust::directoryOwnedByAnyoneElseIsRecreated_data()
{
    QTest::addColumn<QString>("owner");   // empty = unreadable
    QTest::newRow("some user") << SomeUser;
    QTest::newRow("Users") << Users;
    QTest::newRow("unreadable") << QString();
}

void tst_DataDirectoryTrust::directoryOwnedByAnyoneElseIsRecreated()
{
    QFETCH(QString, owner);
    QCOMPARE(decide(dir(owner.isEmpty() ? std::nullopt : std::optional<QString>(owner))), DirAction::Recreate);
}

void tst_DataDirectoryTrust::reparsePointOrFileAtThePathIsRefused_data()
{
    QTest::addColumn<int>("kind");
    QTest::addColumn<QString>("owner");
    QTest::newRow("junction, SYSTEM") << int(FsEntry::Kind::Link) << System;
    QTest::newRow("junction, user") << int(FsEntry::Kind::Link) << SomeUser;
    QTest::newRow("file") << int(FsEntry::Kind::File) << Admins;
}

void tst_DataDirectoryTrust::reparsePointOrFileAtThePathIsRefused()
{
    QFETCH(int, kind);
    QFETCH(QString, owner);
    QCOMPARE(decide(dir(owner, FsEntry::Kind(kind))), DirAction::Refuse);
}

void tst_DataDirectoryTrust::childOwnedByAUserForcesRecreate()
{
    QCOMPARE(decide(dir(Admins), {child(QStringLiteral("config.json"), SomeUser)}), DirAction::Recreate);
}

void tst_DataDirectoryTrust::aChildThatVanishedIsNotEvidence()
{
    QCOMPARE(decide(dir(Admins), {child(QStringLiteral("config.json"), Admins), FsEntry::gone(QStringLiteral("status.json.tmp")),
                                  FsEntry::gone(QStringLiteral("bridge.log"))}),
             DirAction::Trust);
}

void tst_DataDirectoryTrust::aVanishedChildDoesNotHideAnUntrustedOne()
{
    QCOMPARE(decide(dir(Admins), {FsEntry::gone(QStringLiteral("bridge.log")), child(QStringLiteral("config.json"), SomeUser)}),
             DirAction::Recreate);
}

void tst_DataDirectoryTrust::childWithUnreadableOwnerForcesRecreate()
{
    QCOMPARE(decide(dir(Admins), {child(QStringLiteral("status.json"), std::nullopt)}), DirAction::Recreate);
}

void tst_DataDirectoryTrust::reparsePointChildForcesRecreateEvenWhenOwnedByAdmins()
{
    QCOMPARE(decide(dir(Admins), {child(QStringLiteral("config.json.tmp"), Admins, FsEntry::Kind::Link)}), DirAction::Recreate);
}

void tst_DataDirectoryTrust::theRunningAdministratorsOwnFilesAreTrustedButNobodyElses()
{
    const QString me = QStringLiteral("S-1-5-21-1111111111-2222222222-3333333333-500");
    QCOMPARE(decide(dir(Admins), {child(QStringLiteral("config.json"), me)}, TrustPolicy::windows(me)), DirAction::Trust);
    QCOMPARE(decide(dir(Admins), {child(QStringLiteral("config.json"), SomeUser)}, TrustPolicy::windows(me)), DirAction::Recreate);
    QVERIFY(!TrustPolicy::windows().isTrustedOwner(std::nullopt));
    QVERIFY(!TrustPolicy::windows(QString()).isTrustedOwner(QString()));
}

void tst_DataDirectoryTrust::onlySystemAndAdministratorsAreTrustedOwners()
{
    const TrustPolicy p = TrustPolicy::windows();
    QVERIFY(p.isTrustedOwner(System));
    QVERIFY(p.isTrustedOwner(Admins));
    QVERIFY(!p.isTrustedOwner(Users));
    QVERIFY(!p.isTrustedOwner(SomeUser));
    QVERIFY(!p.isTrustedOwner(std::nullopt));
    QVERIFY(!p.isTrustedOwner(QString()));
}

// INV-63. Kills: accepting a directory (or anything but a regular file) at a known name.
void tst_DataDirectoryTrust::knownNamesMustBeRegularFiles_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<int>("kind");
    for (const char* n : {"config.json", "config.pending.json", "start.token", "status.json", "bridge.log", "bridge.log.1",
                          "config.json.0123456789abcdef0123456789abcdef.asbtmp"}) {
        QTest::addRow("%s directory", n) << QString::fromLatin1(n) << int(FsEntry::Kind::Directory);
        QTest::addRow("%s other", n) << QString::fromLatin1(n) << int(FsEntry::Kind::Other);
    }
}

void tst_DataDirectoryTrust::knownNamesMustBeRegularFiles()
{
    QFETCH(QString, name);
    QFETCH(int, kind);
    const DataDirectoryTrust::Decision d =
        DataDirectoryTrust::decide(dir(Admins), {child(name, Admins, FsEntry::Kind(kind))}, TrustPolicy::windows());
    QCOMPARE(d.action, DirAction::Recreate);
    QVERIFY2(d.reason.contains(name), qPrintable(d.reason));
    QCOMPARE(decide(dir(Admins), {child(name, Admins, FsEntry::Kind::File)}), DirAction::Trust);
}

void tst_DataDirectoryTrust::anOrdinarySubdirectoryIsNotAKnownName()
{
    QCOMPARE(decide(dir(Admins), {child(QStringLiteral("old"), Admins, FsEntry::Kind::Directory)}), DirAction::Trust);
}

void tst_DataDirectoryTrust::knownNamesMatchCaseInsensitively()
{
    QVERIFY(DataDirectoryTrust::isKnownFileName(QStringLiteral("Config.JSON")));
    QVERIFY(DataDirectoryTrust::isKnownFileName(QStringLiteral("START.TOKEN")));
    QVERIFY(!DataDirectoryTrust::isKnownFileName(QStringLiteral("config.json.bak")));
}

QTEST_GUILESS_MAIN(tst_DataDirectoryTrust)
#include "tst_DataDirectoryTrust.moc"
