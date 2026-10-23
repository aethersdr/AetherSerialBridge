// AceJudge (with AceKind::Unknown: an entry the adapter
// could not decode is untrusted whatever its SID and mask).

#include "core/AceJudge.h"
#include "core/DataDirectoryTrust.h"

#include <QTest>

using namespace asb;

namespace {
const QString System = QStringLiteral("S-1-5-18"), Admins = QStringLiteral("S-1-5-32-544"),
              Users = QStringLiteral("S-1-5-32-545"), Everyone = QStringLiteral("S-1-1-0"),
              AuthUsers = QStringLiteral("S-1-5-11"), CreatorOwner = QStringLiteral("S-1-3-0"),
              OwnerRights = QStringLiteral("S-1-3-4"), Me = QStringLiteral("S-1-5-21-1-2-3-500");
TrustPolicy policy() { return TrustPolicy::windows(Me); }
}  // namespace

class tst_AceJudge : public QObject {
    Q_OBJECT
private slots:
    void allowingWriteToAnyoneElseIsUntrusted_data();
    void allowingWriteToAnyoneElseIsUntrusted();
    void readDenyAndTrustedSidsAreFine_data();
    void readDenyAndTrustedSidsAreFine();
    void unknownAceIsUntrustedWhateverTheSid_data();
    void unknownAceIsUntrustedWhateverTheSid();
    void anAdminOwnedConfigWithAUsersWriteAceIsNotTrusted();
    void aFolderWhoseOwnAclLetsUsersWriteIsNotTrusted();
    void theLockedDownAclIsTrusted();
    void anUnknownAceOnAChildMakesTheFolderUntrusted();
};

// Kills: a Users write ACE judged trusted, or any narrowing of the dangerous-rights mask.
void tst_AceJudge::allowingWriteToAnyoneElseIsUntrusted_data()
{
    QTest::addColumn<QString>("sid");
    QTest::addColumn<quint32>("rights");
    QTest::newRow("Users WriteData") << Users << AceJudge::WriteData;
    QTest::newRow("Users AppendData") << Users << AceJudge::AppendData;
    QTest::newRow("Users WriteAttributes") << Users << AceJudge::WriteAttributes;
    QTest::newRow("Users WriteExtendedAttributes") << Users << AceJudge::WriteExtendedAttributes;
    QTest::newRow("Users Delete") << Users << AceJudge::Delete;
    QTest::newRow("Users DeleteChild") << Users << AceJudge::DeleteSubdirectoriesAndFiles;
    QTest::newRow("Users WRITE_DAC") << Users << AceJudge::ChangePermissions;
    QTest::newRow("Users WRITE_OWNER") << Users << AceJudge::TakeOwnership;
    QTest::newRow("Users GenericWrite") << Users << AceJudge::GenericWrite;
    QTest::newRow("Users GenericAll") << Users << AceJudge::GenericAll;
    QTest::newRow("Everyone FullControl") << Everyone << AceJudge::FullControl;
    QTest::newRow("AuthUsers Modify") << AuthUsers << AceJudge::Modify;
    QTest::newRow("CreatorOwner FullControl") << CreatorOwner << AceJudge::FullControl;
    QTest::newRow("OwnerRights WriteData") << OwnerRights << AceJudge::WriteData;
}

void tst_AceJudge::allowingWriteToAnyoneElseIsUntrusted()
{
    QFETCH(QString, sid);
    QFETCH(quint32, rights);
    QVERIFY(AceJudge::isUntrusted(AceInfo{sid, rights, AceKind::Allow}, policy()));
}

void tst_AceJudge::readDenyAndTrustedSidsAreFine_data()
{
    QTest::addColumn<QString>("sid");
    QTest::addColumn<quint32>("rights");
    QTest::addColumn<bool>("allow");
    QTest::newRow("Users read") << Users << AceJudge::ReadAndExecute << true;
    QTest::newRow("Everyone read") << Everyone << AceJudge::ReadAndExecute << true;
    QTest::newRow("Users deny full") << Users << AceJudge::FullControl << false;
    QTest::newRow("Everyone deny modify") << Everyone << AceJudge::Modify << false;
    QTest::newRow("System full") << System << AceJudge::FullControl << true;
    QTest::newRow("Admins full") << Admins << AceJudge::FullControl << true;
    QTest::newRow("the running admin") << Me << AceJudge::FullControl << true;
}

void tst_AceJudge::readDenyAndTrustedSidsAreFine()
{
    QFETCH(QString, sid);
    QFETCH(quint32, rights);
    QFETCH(bool, allow);
    QVERIFY(!AceJudge::isUntrusted(AceInfo{sid, rights, allow ? AceKind::Allow : AceKind::Deny}, policy()));
}

// Kills: decoding an unknown ACE type as Allow/Deny by its mask, or trusting it for a trusted SID.
void tst_AceJudge::unknownAceIsUntrustedWhateverTheSid_data()
{
    QTest::addColumn<QString>("sid");
    QTest::addColumn<quint32>("rights");
    QTest::newRow("System, read only") << System << AceJudge::ReadAndExecute;
    QTest::newRow("Admins, no rights") << Admins << quint32(0);
    QTest::newRow("Users, full") << Users << AceJudge::FullControl;
}

void tst_AceJudge::unknownAceIsUntrustedWhateverTheSid()
{
    QFETCH(QString, sid);
    QFETCH(quint32, rights);
    QVERIFY(AceJudge::isUntrusted(AceInfo{sid, rights, AceKind::Unknown}, policy()));
}

void tst_AceJudge::anAdminOwnedConfigWithAUsersWriteAceIsNotTrusted()
{
    FsEntry dir{QStringLiteral("AetherSerialBridge"), Admins, FsEntry::Kind::Directory, false, {}};
    FsEntry child{QStringLiteral("config.json"), Admins, FsEntry::Kind::File, false,
                  {AceInfo{Users, AceJudge::Modify, AceKind::Allow}}};
    QCOMPARE(DataDirectoryTrust::decide(dir, {child}, TrustPolicy::windows()).action, DirAction::Recreate);
}

void tst_AceJudge::aFolderWhoseOwnAclLetsUsersWriteIsNotTrusted()
{
    FsEntry dir{QStringLiteral("AetherSerialBridge"), Admins, FsEntry::Kind::Directory, false,
                {AceInfo{Users, AceJudge::WriteData | AceJudge::AppendData, AceKind::Allow}}};
    QCOMPARE(DataDirectoryTrust::decide(dir, {}, TrustPolicy::windows()).action, DirAction::Recreate);
}

void tst_AceJudge::theLockedDownAclIsTrusted()
{
    const QList<AceInfo> locked = {AceInfo{System, AceJudge::FullControl, AceKind::Allow},
                                   AceInfo{Admins, AceJudge::FullControl, AceKind::Allow},
                                   AceInfo{Users, AceJudge::ReadAndExecute, AceKind::Allow}};
    FsEntry dir{QStringLiteral("AetherSerialBridge"), Admins, FsEntry::Kind::Directory, false, locked};
    FsEntry child{QStringLiteral("config.json"), Admins, FsEntry::Kind::File, false, locked};
    QCOMPARE(DataDirectoryTrust::decide(dir, {child}, TrustPolicy::windows()).action, DirAction::Trust);
}

void tst_AceJudge::anUnknownAceOnAChildMakesTheFolderUntrusted()
{
    FsEntry dir{QStringLiteral("AetherSerialBridge"), Admins, FsEntry::Kind::Directory, false, {}};
    FsEntry child{QStringLiteral("config.json"), Admins, FsEntry::Kind::File, false,
                  {AceInfo{System, AceJudge::ReadAndExecute, AceKind::Unknown}}};
    QCOMPARE(DataDirectoryTrust::decide(dir, {child}, TrustPolicy::windows()).action, DirAction::Recreate);
}

QTEST_GUILESS_MAIN(tst_AceJudge)
#include "tst_AceJudge.moc"
