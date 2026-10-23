// WS-6: the Windows data-folder adapter on the recording NtSeams fake (INV-16, 17, 19, 40, 62, 74, 116), and the
// raw security-descriptor decoding it rests on (INV-19). The same adapter runs natively on windows-latest in
// tests/win/integration/tst_WinDataFolderNative.
#include "core/AtomicJson.h"
#include "core/ConfigFiles.h"
#include "core/StartToken.h"
#include "core/TrustedConfig.h"
#include "platform/win/WindowsDataFolder.h"
#include "support/WinFakeFixture.h"

#include <QTest>

using namespace asb;
using namespace asb::win;
using namespace asb::test;
using namespace asb::test::win6;

class tst_WindowsDataFolder : public QObject {
    Q_OBJECT

    struct World {
        FakeNtSeams fs;
        WindowsDataFolder folder{fs, kData, [] { return QStringLiteral("20261014T120000Z-0000abcd"); }};
        World() { setUpMachine(fs); }
        // A folder as the Applier makes it, with config.json, status.json (service) and start.token.
        void makeTrusted()
        {
            folder.createLockedDown();
            fs.plantFile(kData + QStringLiteral("\\config.json"), "{\"version\":1,\"ports\":[]}", sd::ownerOnlyDescriptor(kBa));
            fs.plantFile(kData + QStringLiteral("\\status.json"), "{}", sd::ownerOnlyDescriptor(kSy));
        }
        Inspection inspect(TrustMode m = TrustMode::Mutating) { return folder.inspect(m); }
    };

private slots:
    // ---- INV-19: the raw descriptor decoder -----------------------------------------------------
    void rawDescriptorDecoding_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<bool>("decodes");
        QTest::addColumn<bool>("trusted");   // owner + AceJudge, Windows policy

        const QByteArray basic = descriptor(kBa, {allow(kSy, sd::kFullControl), allow(kBa, sd::kFullControl),
                                                  allow(kBu, sd::kReadExecute)});
        QTest::newRow("basic allow entries") << basic << true << true;
        QTest::newRow("deny for Users (never widens)")
            << descriptor(kBa, {allow(kBa, sd::kFullControl), sd::Ace{sd::kAccessDenied, 0, sd::kFullControl, kBu}}) << true << true;
        QTest::newRow("Users write") << descriptor(kBa, {allow(kBu, 0x2)}) << true << false;
        QTest::newRow("Users write, inherit-only (counted)")
            << descriptor(kBa, {allow(kBu, 0x2, sd::kObjectInherit | sd::kInheritOnly)}) << true << false;
        QTest::newRow("Users generic write") << descriptor(kBa, {allow(kBu, 0x40000000)}) << true << false;
        QTest::newRow("CREATOR OWNER full, inherit-only (not special-cased)")
            << descriptor(kBa, {allow(QString::fromLatin1(sd::kCreatorOwner), 0x10000000, sd::kObjectInherit | sd::kInheritOnly)})
            << true << false;
        // Undecodable ACE kinds: untrusted whatever the SID (here SYSTEM) and mask (here read only).
        const QByteArray sysRead = descriptor(kBa, {allow(kSy, sd::kReadExecute)});
        QTest::newRow("compound ACE (4)") << withAceType(sysRead, 0, 4) << true << false;
        QTest::newRow("object allow ACE (5)") << withAceType(sysRead, 0, 5) << true << false;
        QTest::newRow("object deny ACE (6)") << withAceType(sysRead, 0, 6) << true << false;
        QTest::newRow("callback allow ACE (9)") << withAceType(sysRead, 0, 9) << true << false;
        QTest::newRow("callback deny ACE (10)") << withAceType(sysRead, 0, 10) << true << false;
        QTest::newRow("callback object ACE (11)") << withAceType(sysRead, 0, 11) << true << false;
        QTest::newRow("mandatory label (0x11) in a DACL") << withAceType(sysRead, 0, 0x11) << true << false;
        QTest::newRow("audit ACE (2) in a DACL") << withAceType(sysRead, 0, 2) << true << false;
        QTest::newRow("unknown second ACE after a good one")
            << withAceType(descriptor(kBa, {allow(kSy, sd::kFullControl), allow(kBa, sd::kReadExecute)}), 1, 0x13) << true << false;
        // DACL absent or NULL: everyone has every right.
        QTest::newRow("NULL DACL") << [] {
            QByteArray b = sd::build(kBa);
            b[2] = char(quint8(b[2]) | sd::kDaclPresent);   // present, offset 0
            return b;
        }() << true << false;
        QTest::newRow("DACL absent") << sd::build(kBa) << true << false;
        QTest::newRow("no owner") << sd::build(QString(), parsed(basic)) << true << false;
        QTest::newRow("owner is a user") << descriptor(kUser, {allow(kSy, sd::kFullControl)}) << true << false;
        // Malformed: the whole descriptor is refused (the adapter treats it as unreadable: untrusted).
        QTest::newRow("truncated header") << basic.left(12) << false << false;
        QTest::newRow("revision 2") << [&] { QByteArray b = basic; b[0] = 2; return b; }() << false << false;
        QTest::newRow("not self-relative") << [&] { QByteArray b = basic; b[3] = char(quint8(b[3]) & 0x7f); return b; }() << false << false;
        QTest::newRow("truncated ACL") << basic.left(basic.size() - 3) << false << false;
        QTest::newRow("owner offset out of bounds") << [&] { QByteArray b = basic; b[4] = '\xF0'; return b; }() << false << false;
        QTest::newRow("owner SID with 16 sub-authorities") << [&] {
            QByteArray b = basic;
            b[20 + 1] = char(16);
            return b;
        }() << false << false;
        QTest::newRow("ACE size larger than the ACL") << [&] {
            QByteArray b = basic;
            const qsizetype dacl = quint8(b[16]);
            b[dacl + 8 + 2] = '\xFF';
            return b;
        }() << false << false;
    }
    void rawDescriptorDecoding()
    {
        QFETCH(QByteArray, bytes);
        QFETCH(bool, decodes);
        QFETCH(bool, trusted);
        const std::optional<sd::Descriptor> d = sd::parse(bytes);
        QCOMPARE(d.has_value(), decodes);
        if (!d) return;
        const TrustPolicy policy = TrustPolicy::windows();
        const bool judged = policy.isTrustedOwner(d->owner) && !AceJudge::anyUntrusted(sd::toAceInfos(*d), policy);
        QCOMPARE(judged, trusted);
    }

    void unknownKindsAreUnknownWhateverTheSid()
    {
        for (quint8 type : {quint8(2), quint8(4), quint8(5), quint8(6), quint8(9), quint8(10), quint8(11), quint8(0x11)}) {
            const std::optional<sd::Descriptor> d = sd::parse(withAceType(descriptor(kBa, {allow(kSy, 0x1)}), 0, type));
            QVERIFY(d);
            QCOMPARE(d->dacl.size(), 1);
            QCOMPARE(d->dacl[0].kind(), AceKind::Unknown);
            QCOMPARE(sd::toAceInfos(*d)[0].kind, AceKind::Unknown);
        }
    }

    void sidRoundTrip_data()
    {
        QTest::addColumn<QString>("sid");
        QTest::newRow("SYSTEM") << QStringLiteral("S-1-5-18");
        QTest::newRow("Administrators") << QStringLiteral("S-1-5-32-544");
        QTest::newRow("a user") << QStringLiteral("S-1-5-21-1111111111-2222222222-3333333333-500");
        QTest::newRow("TrustedInstaller") << QString::fromLatin1(sd::kTrustedInstaller);
        QTest::newRow("Everyone") << QStringLiteral("S-1-1-0");
        QTest::newRow("48-bit authority") << QStringLiteral("S-1-0x123456789ABC-7");
    }
    void sidRoundTrip()
    {
        QFETCH(QString, sid);
        const std::optional<QByteArray> b = sd::sidToBinary(sid);
        QVERIFY(b);
        QCOMPARE(sd::sidFromBinary(*b).value_or(QString()), sid);
    }

    void malformedSidStringsRefused()
    {
        for (const char* s : {"", "S-1", "S-2-5-18", "X-1-5-18", "S-1-5-", "S-1-5-18x", "S-1-5-4294967296",
                              "S-1-5-1-2-3-4-5-6-7-8-9-10-11-12-13-14-15-16", "S-1--5"})
            QVERIFY2(!sd::sidToBinary(QString::fromLatin1(s)), s);
    }

    void descriptorsWeCreate()
    {
        const sd::Descriptor folder = parsed(sd::dataFolderDescriptor());
        QCOMPARE(folder.owner.value_or(QString()), kBa);
        QVERIFY(folder.daclPresent && folder.daclProtected);
        const quint8 oici = sd::kObjectInherit | sd::kContainerInherit;
        QCOMPARE(show(folder.dacl), show({allow(kSy, sd::kFullControl, oici), allow(kBa, sd::kFullControl, oici),
                                          allow(kBu, sd::kReadExecute, oici)}));
        const sd::Descriptor lock = parsed(sd::lockFileDescriptor());
        QString why;
        QVERIFY2(sd::isLockFileSecure(lock, &why), qPrintable(why));
        QCOMPARE(show(lock.dacl), show({allow(kSy, sd::kFullControl), allow(kBa, sd::kFullControl)}));
        const sd::Descriptor owner = parsed(sd::ownerOnlyDescriptor(kSy));
        QCOMPARE(owner.owner.value_or(QString()), kSy);
        QVERIFY(!owner.daclPresent);
    }

    void inheritedEntriesOfAFile()
    {
        const QList<sd::Ace> got = sd::inheritedForFile(parsed(sd::dataFolderDescriptor()).dacl, kBa);
        QCOMPARE(show(got), show({allow(kSy, sd::kFullControl, sd::kInherited), allow(kBa, sd::kFullControl, sd::kInherited),
                                  allow(kBu, sd::kReadExecute, sd::kInherited)}));
        // Container-only entries do not reach files; generic rights are mapped; CREATOR OWNER becomes the owner.
        const QList<sd::Ace> mixed = sd::inheritedForFile(
            {allow(kBu, 0x2, sd::kContainerInherit), allow(QString::fromLatin1(sd::kCreatorOwner), 0x10000000, sd::kObjectInherit | sd::kInheritOnly)},
            kUser);
        QCOMPARE(show(mixed), show({allow(kUser, sd::kFullControl, sd::kInherited)}));
    }

    // ---- INV-16: created locked down in one call -------------------------------------------------
    void createDirectoryReceivesProtectedSa()
    {
        World w;
        w.folder.createLockedDown();
        const QList<FakeNtSeams::Call> creates = w.fs.callsOf(QStringLiteral("createDirectory"));
        QCOMPARE(creates.size(), 1);
        QCOMPARE(creates[0].path, kData);
        QCOMPARE(creates[0].sd, sd::dataFolderDescriptor());   // owner + protected DACL in the create call itself
        QCOMPARE(w.fs.callCount(QStringLiteral("setSecurity")), 0);   // never "create, then secure"
        QCOMPARE(w.fs.node(kData)->sd, sd::dataFolderDescriptor());
        QCOMPARE(w.inspect().action, DirAction::Trust);
    }

    void createRefusesAFolderSomeoneElseMadeFirst()
    {
        World w;
        w.fs.mkdir(kData);
        QVERIFY_THROWS_EXCEPTION(PlatformError, w.folder.createLockedDown());
    }

    // ---- INV-17: a link or file at the path is refused, untouched --------------------------------
    void linkOrFileAtPathRefused_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::newRow("junction") << QStringLiteral("junction");
        QTest::newRow("directory symlink") << QStringLiteral("dirlink");
        QTest::newRow("file") << QStringLiteral("file");
    }
    void linkOrFileAtPathRefused()
    {
        QFETCH(QString, kind);
        World w;
        const QString target = QStringLiteral("C:\\ProgramData\\elsewhere");
        w.fs.mkdir(target, descriptor(kUser, {allow(QStringLiteral("S-1-1-0"), sd::kFullControl, 3)}));
        w.fs.plantFile(target + QStringLiteral("\\config.json"), "TARGET");
        if (kind == QLatin1String("file")) w.fs.plantFile(kData, "a file");
        else w.fs.plantLink(kData, target, true);
        const QByteArray targetSd = w.fs.node(target)->sd;
        for (TrustMode m : {TrustMode::ReadOnly, TrustMode::Mutating, TrustMode::Repair, TrustMode::Service})
            QCOMPARE(w.folder.inspect(m).action, DirAction::Refuse);
        w.fs.calls.clear();
        const OpenedFolder repaired = TrustedConfig::open(w.folder, TrustMode::Repair);
        QVERIFY(!repaired.usable);
        for (const char* op : {"createDirectory", "setSecurity", "rename", "dispose", "write"})
            QCOMPARE(w.fs.callCount(QString::fromLatin1(op)), 0);
        // Nothing behind the link was opened by the inspection either.
        for (const FakeNtSeams::Call& c : w.fs.callsOf(QStringLiteral("openRelative"))) QVERIFY(!c.path.contains(QLatin1String("elsewhere")));
        QCOMPARE(w.fs.node(target)->sd, targetSd);
        QCOMPARE(w.fs.node(target + QStringLiteral("\\config.json"))->data, QByteArray("TARGET"));
    }

    void folderOpenedWithoutFollowingReparse()
    {
        World w;
        w.inspect();
        w.makeTrusted();
        const Inspection i = w.inspect(TrustMode::Repair);
        w.folder.normalise(i);
        w.folder.renameAside(i);
        int folderOpens = 0;
        for (const FakeNtSeams::Call& c : w.fs.callsOf(QStringLiteral("createFile"))) {
            if (c.path != kData) continue;   // the parent is opened only to ask for its volume
            ++folderOpens;
            QVERIFY(c.flags & nt::kFlagOpenReparsePoint);
        }
        QVERIFY(folderOpens >= 4);
    }

    // ---- INV-19: DACLs of the folder and of every child are judged -------------------------------
    void childExplicitUsersWriteDetected_data()
    {
        QTest::addColumn<QByteArray>("childSd");
        QTest::addColumn<bool>("trusted");
        QTest::newRow("inherited only") << sd::ownerOnlyDescriptor(kBa) << true;
        QTest::newRow("explicit Users write") << descriptor(kBa, {allow(kBu, 0x2)}) << false;
        QTest::newRow("explicit Users full, protected") << descriptor(kBa, {allow(kBu, sd::kFullControl)}, true) << false;
        QTest::newRow("explicit Everyone delete") << descriptor(kBa, {allow(QStringLiteral("S-1-1-0"), 0x10000)}) << false;
        QTest::newRow("explicit Users read") << descriptor(kBa, {allow(kBu, sd::kReadExecute)}) << true;
        QTest::newRow("callback ACE for SYSTEM") << withAceType(descriptor(kBa, {allow(kSy, 0x1)}), 0, 9) << false;
        QTest::newRow("NULL DACL") << [] {
            QByteArray b = sd::build(kBa);
            b[2] = char(quint8(b[2]) | sd::kDaclPresent);
            return b;
        }() << false;
        QTest::newRow("owned by a user") << sd::ownerOnlyDescriptor(kUser) << false;
    }
    void childExplicitUsersWriteDetected()
    {
        QFETCH(QByteArray, childSd);
        QFETCH(bool, trusted);
        World w;
        w.makeTrusted();
        w.fs.plantFile(kData + QStringLiteral("\\notes.txt"), "x", childSd);
        const Inspection i = w.inspect();
        QCOMPARE(i.action, trusted ? DirAction::Trust : DirAction::Recreate);
        if (!trusted) QVERIFY2(i.reason.contains(QLatin1String("notes.txt")), qPrintable(i.reason));
    }

    void folderDaclJudged_data()
    {
        QTest::addColumn<QByteArray>("folderSd");
        QTest::addColumn<bool>("trusted");
        const quint8 oici = sd::kObjectInherit | sd::kContainerInherit;
        QTest::newRow("ours") << sd::dataFolderDescriptor() << true;
        QTest::newRow("Users may add files (inherit-only on children too)")
            << descriptor(kBa, {allow(kSy, sd::kFullControl, oici), allow(kBu, 0x2, sd::kContainerInherit | sd::kInheritOnly)}, true) << false;
        QTest::newRow("owner is a user") << descriptor(kUser, {allow(kSy, sd::kFullControl, oici)}, true) << false;
        QTest::newRow("object ACE") << withAceType(sd::dataFolderDescriptor(), 2, 5) << false;
    }
    void folderDaclJudged()
    {
        QFETCH(QByteArray, folderSd);
        QFETCH(bool, trusted);
        World w;
        w.fs.mkdir(kData, folderSd);
        QCOMPARE(w.inspect().action, trusted ? DirAction::Trust : DirAction::Recreate);
    }

    // ---- INV-40: a vanished child is not evidence; an unreadable one is untrusted -----------------
    void errorClassification_data()
    {
        QTest::addColumn<QString>("op");
        QTest::addColumn<QString>("target");   // the call's name/path must end with this
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("action");
        using K = win::Status::Kind;
        auto row = [](const char* tag, const char* op, const char* target, K k, DirAction a) {
            QTest::newRow(tag) << QString::fromLatin1(op) << QString::fromLatin1(target) << int(k) << int(a);
        };
        row("child open: not found", "openRelative", "status.json", K::NotFound, DirAction::Trust);
        row("child open: delete pending", "openRelative", "status.json", K::DeletePending, DirAction::Trust);
        row("child open: access denied", "openRelative", "status.json", K::AccessDenied, DirAction::Recreate);
        row("child open: sharing violation", "openRelative", "status.json", K::SharingViolation, DirAction::Recreate);
        row("child open: other error", "openRelative", "status.json", K::Other, DirAction::Recreate);
        row("child facts: delete pending", "facts", "status.json", K::DeletePending, DirAction::Trust);
        row("child facts: access denied", "facts", "status.json", K::AccessDenied, DirAction::Recreate);
        row("child security: not found", "getSecurity", "status.json", K::NotFound, DirAction::Trust);
        row("child security: access denied", "getSecurity", "status.json", K::AccessDenied, DirAction::Recreate);
        row("child security: other", "getSecurity", "status.json", K::Other, DirAction::Recreate);
        row("folder security: access denied", "getSecurity", "AetherSerialBridge", K::AccessDenied, DirAction::Recreate);
        row("folder listing fails", "list", "AetherSerialBridge", K::AccessDenied, DirAction::Recreate);
        row("folder open: access denied", "createFile", "AetherSerialBridge", K::AccessDenied, DirAction::Recreate);
        row("folder open: not found", "createFile", "AetherSerialBridge", K::NotFound, DirAction::Create);
        row("folder volume unknown", "volume", "AetherSerialBridge", K::AccessDenied, DirAction::Refuse);
    }
    void errorClassification()
    {
        QFETCH(QString, op);
        QFETCH(QString, target);
        QFETCH(int, kind);
        QFETCH(int, action);
        World w;
        w.makeTrusted();
        QCOMPARE(w.inspect().action, DirAction::Trust);
        w.fs.inject = [&](const FakeNtSeams::Call& c) -> std::optional<win::Status> {
            if (c.op == op && (c.path.endsWith(target) || c.name == target))
                return win::Status::fail(win::Status::Kind(kind), 1, QStringLiteral("injected"));
            return std::nullopt;
        };
        QCOMPARE(int(w.inspect().action), action);
    }

    // ---- INV-62: mutation handles are identity-checked -------------------------------------------
    void identitySwapAborts_data()
    {
        QTest::addColumn<QString>("swap");   // what is replaced between the inspection and the change
        QTest::addColumn<QString>("change");
        QTest::newRow("folder, normalise") << QStringLiteral("folder") << QStringLiteral("normalise");
        QTest::newRow("child, normalise") << QStringLiteral("child") << QStringLiteral("normalise");
        QTest::newRow("folder, rename aside") << QStringLiteral("folder") << QStringLiteral("renameAside");
    }
    void identitySwapAborts()
    {
        QFETCH(QString, swap);
        QFETCH(QString, change);
        World w;
        w.makeTrusted();
        // Something to normalise: an explicit trusted entry on the child, and an unprotected folder.
        w.fs.setSd(kData + QStringLiteral("\\config.json"), descriptor(kBa, {allow(kBa, sd::kFullControl)}));
        w.fs.setSd(kData, descriptor(kBa, {allow(kSy, sd::kFullControl, 3), allow(kBa, sd::kFullControl, 3)}));
        const Inspection i = w.inspect(TrustMode::Repair);
        QCOMPARE(i.action, DirAction::Trust);
        FakeNtSeams::NodeP intruder;
        if (swap == QLatin1String("folder")) {
            intruder = w.fs.newDir(descriptor(kUser, {allow(QStringLiteral("S-1-1-0"), sd::kFullControl, 3)}));
            w.fs.replace(kData, intruder);
        } else {
            intruder = w.fs.plantFile(kData + QStringLiteral("\\elsewhere.txt"), "x", descriptor(kUser, {allow(kBu, sd::kFullControl)}));
            w.fs.replace(kData + QStringLiteral("\\config.json"), intruder);
        }
        const QByteArray intruderSd = intruder->sd;
        w.fs.calls.clear();
        if (change == QLatin1String("normalise")) QVERIFY_THROWS_EXCEPTION(PlatformError, w.folder.normalise(i));
        else QVERIFY_THROWS_EXCEPTION(PlatformError, w.folder.renameAside(i));
        QCOMPARE(w.fs.callCount(QStringLiteral("setSecurity")), 0);   // nothing changed, anywhere
        QCOMPARE(w.fs.callCount(QStringLiteral("rename")), 0);
        QCOMPARE(intruder->sd, intruderSd);
    }

    void staleInspectionRefusedForReadsAndChanges()
    {
        World w;
        w.makeTrusted();
        const Inspection first = w.inspect();
        Inspection other = first;
        other.folderId.id[0] = quint8(other.folderId.id[0] ^ 0xFFu);   // an inspection of some other object
        QString err;
        QVERIFY(!w.folder.readFile(other, QStringLiteral("config.json"), 1024, &err));
        QVERIFY(!err.isEmpty() && err != fileNotFoundError());
        QVERIFY_THROWS_EXCEPTION(PlatformError, w.folder.normalise(other));
        QVERIFY_THROWS_EXCEPTION(PlatformError, w.folder.renameAside(other));
    }

    // ---- readFile re-checks the file on the handle it reads from ----------------------------------
    void readFileChecksTheHandle_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("setup");
        QTest::addColumn<bool>("ok");
        QTest::newRow("config.json") << QStringLiteral("config.json") << QString() << true;
        QTest::newRow("status.json owned by SYSTEM") << QStringLiteral("status.json") << QString() << true;
        QTest::newRow("config.json owned by SYSTEM (not the Config owner)") << QStringLiteral("config.json") << QStringLiteral("owner-sy") << false;
        QTest::newRow("config.json owned by a user") << QStringLiteral("config.json") << QStringLiteral("owner-user") << false;
        QTest::newRow("config.json gained Users write after the inspection") << QStringLiteral("config.json") << QStringLiteral("users-write") << false;
        QTest::newRow("config.json replaced by a symlink") << QStringLiteral("config.json") << QStringLiteral("symlink") << false;
        QTest::newRow("config.json replaced by a directory") << QStringLiteral("config.json") << QStringLiteral("directory") << false;
        QTest::newRow("larger than the cap") << QStringLiteral("config.json") << QStringLiteral("big") << false;
        QTest::newRow("start.token inherited") << QStringLiteral("start.token") << QStringLiteral("token") << true;
        QTest::newRow("start.token with its own (trusted) DACL") << QStringLiteral("start.token") << QStringLiteral("token-own-dacl") << false;
        QTest::newRow("start.token protected") << QStringLiteral("start.token") << QStringLiteral("token-protected") << false;
    }
    void readFileChecksTheHandle()
    {
        QFETCH(QString, name);
        QFETCH(QString, setup);
        QFETCH(bool, ok);
        World w;
        w.makeTrusted();
        const QString path = kData + QLatin1Char('\\') + name;
        if (setup.startsWith(QLatin1String("token"))) w.fs.plantFile(path, "{\"revoked\":true}", sd::ownerOnlyDescriptor(kBa));
        const Inspection i = w.inspect();
        QCOMPARE(i.action, DirAction::Trust);
        if (setup == QLatin1String("owner-sy")) w.fs.setSd(path, sd::build(kSy, parsed(w.fs.node(path)->sd)));
        if (setup == QLatin1String("owner-user")) w.fs.setSd(path, sd::build(kUser, parsed(w.fs.node(path)->sd)));
        if (setup == QLatin1String("users-write")) w.fs.setSd(path, descriptor(kBa, {allow(kBu, 0x6)}));
        if (setup == QLatin1String("symlink")) w.fs.replace(path, w.fs.plantLink(kData + QStringLiteral("\\l"), kData + QStringLiteral("\\status.json"), false));
        if (setup == QLatin1String("directory")) w.fs.replace(path, w.fs.newDir());
        if (setup == QLatin1String("big")) w.fs.node(path)->data = QByteArray(2048, 'x');
        if (setup == QLatin1String("token-own-dacl")) w.fs.setSd(path, descriptor(kBa, {allow(kSy, sd::kFullControl), allow(kBa, sd::kFullControl)}));
        if (setup == QLatin1String("token-protected"))
            w.fs.setSd(path, descriptor(kBa, sd::inheritedForFile(parsed(sd::dataFolderDescriptor()).dacl, kBa), true));
        QString err;
        const std::optional<QByteArray> got = w.folder.readFile(i, name, 1024, &err);
        QVERIFY2(got.has_value() == ok, qPrintable(err));
        if (!ok) QVERIFY(!err.isEmpty() && err != fileNotFoundError());
        // Only READ_CONTROL | FILE_READ_ATTRIBUTES | FILE_READ_DATA, never through a reparse point.
        const FakeNtSeams::Call open = w.fs.callsOf(QStringLiteral("openRelative")).constLast();
        QCOMPARE(open.access, nt::kReadControl | nt::kReadAttributes | nt::kReadData);
        QVERIFY(open.flags & nt::kOpenReparsePoint);
    }

    void readFileMissingIsFileNotFound()
    {
        World w;
        w.makeTrusted();
        const Inspection i = w.inspect();
        QString err;
        QVERIFY(!w.folder.readFile(i, QStringLiteral("config.pending.json"), 1024, &err));
        QCOMPARE(err, fileNotFoundError());
    }

    // Child inspection asks for no data right, so share modes never apply: a log held exclusively by the
    // service cannot make the folder look untrusted.
    void inspectionIgnoresShareModes()
    {
        World w;
        w.makeTrusted();
        w.fs.plantFile(kData + QStringLiteral("\\bridge.log"), "log", sd::ownerOnlyDescriptor(kSy));
        Handle d = 0, held = 0;
        QVERIFY(w.fs.createFile(kData, nt::kListDirectory, nt::kShareAll, nt::kOpenExisting, nt::kFlagBackupSemantics, {}, &d).ok());
        QVERIFY(w.fs.openRelative(d, QStringLiteral("bridge.log"), nt::kGenericWrite, 0, nt::kFileOpen, 0, {}, &held).ok());
        QCOMPARE(w.inspect(TrustMode::Service).action, DirAction::Trust);
        for (const FakeNtSeams::Call& c : w.fs.callsOf(QStringLiteral("openRelative")))
            if (c.handle != d) QCOMPARE(c.access & (nt::kReadData | nt::kWriteData | nt::kDelete | nt::kGenericRead), 0u);
        w.fs.close(held);
        w.fs.close(d);
    }

    // ---- normalise ----------------------------------------------------------------------------------
    void normaliseResetsTheFolderAndItsFiles()
    {
        World w;
        w.makeTrusted();
        const QString cfg = kData + QStringLiteral("\\config.json");
        w.fs.setSd(kData, descriptor(kBa, {allow(kSy, sd::kFullControl, 3), allow(kBa, sd::kFullControl, 3)}));   // not protected
        w.fs.setSd(cfg, descriptor(kBa, {allow(kSy, sd::kFullControl), allow(kBa, sd::kFullControl)}, true));       // own, protected
        w.fs.mkdir(kData + QStringLiteral("\\sub"));
        const QByteArray subSd = w.fs.node(kData + QStringLiteral("\\sub"))->sd;
        Inspection i = w.inspect();
        QCOMPARE(i.action, DirAction::Trust);
        const QStringList changes = w.folder.normalise(i);
        QCOMPARE(changes.size(), 2);
        QCOMPARE(w.fs.node(kData)->sd, sd::dataFolderDescriptor());
        const sd::Descriptor c = parsed(w.fs.node(cfg)->sd);
        QCOMPARE(c.owner.value_or(QString()), kBa);   // owner kept
        QVERIFY(!c.daclProtected);
        QCOMPARE(show(c.dacl), show(sd::inheritedForFile(parsed(sd::dataFolderDescriptor()).dacl, kBa)));
        QCOMPARE(w.fs.node(kData + QStringLiteral("\\sub"))->sd, subSd);   // directories are not "regular children"
        // Every change went through a handle that is the inspected object, never by name propagation.
        for (const FakeNtSeams::Call& s : w.fs.callsOf(QStringLiteral("setSecurity"))) QVERIFY(s.handle != 0);
        i = w.inspect();
        QVERIFY(w.folder.normalise(i).isEmpty());   // idempotent
    }

    // An ACL change that is made stays reported when a later step of the same
    // normalisation fails. Through TrustedConfig, so both halves (the adapter's list, the core's capture) count.
    void normaliseFailureReportsTheChangesAlreadyMade_data()
    {
        QTest::addColumn<bool>("folderNormal");     // the folder already has its final descriptor
        QTest::addColumn<QString>("op");            // the call that fails ...
        QTest::addColumn<QString>("target");        // ... on this object
        QTest::addColumn<bool>("afterFirstSet");    // only once a setSecurity has been made (inspect reads the same files)
        QTest::addColumn<QStringList>("reported");
        const QString secured = QStringLiteral("secured the data folder (owner Administrators, protected permissions)");
        const QString resetCfg = QStringLiteral("reset the permissions of config.json to the folder's");
        QTest::newRow("the folder's own change fails: nothing reported")
            << false << QStringLiteral("setSecurity") << QStringLiteral("AetherSerialBridge") << false << QStringList{};
        QTest::newRow("folder secured, then a file's permissions cannot be read")
            << false << QStringLiteral("getSecurity") << QStringLiteral("config.json") << true << QStringList{secured};
        QTest::newRow("folder secured, then a file cannot be reset")
            << false << QStringLiteral("setSecurity") << QStringLiteral("config.json") << false << QStringList{secured};
        QTest::newRow("folder secured, one file reset, then the next fails")
            << false << QStringLiteral("setSecurity") << QStringLiteral("status.json") << false << QStringList{secured, resetCfg};
        QTest::newRow("one file reset, then the next fails")
            << true << QStringLiteral("setSecurity") << QStringLiteral("status.json") << false << QStringList{resetCfg};
    }
    void normaliseFailureReportsTheChangesAlreadyMade()
    {
        QFETCH(bool, folderNormal);
        QFETCH(QString, op);
        QFETCH(QString, target);
        QFETCH(bool, afterFirstSet);
        QFETCH(QStringList, reported);
        World w;
        w.makeTrusted();
        const QString cfg = kData + QStringLiteral("\\config.json");
        const QString status = kData + QStringLiteral("\\status.json");
        if (!folderNormal)
            w.fs.setSd(kData, descriptor(kBa, {allow(kSy, sd::kFullControl, 3), allow(kBa, sd::kFullControl, 3)}));   // not protected
        // Both files carry entries of their own (trusted, so the folder is still Trust): both need a reset.
        const QByteArray own = descriptor(kBa, {allow(kSy, sd::kFullControl), allow(kBa, sd::kFullControl)}, true);
        w.fs.setSd(cfg, own);
        w.fs.setSd(status, descriptor(kSy, {allow(kSy, sd::kFullControl), allow(kBa, sd::kFullControl)}, true));
        QCOMPARE(w.inspect().action, DirAction::Trust);
        bool armed = !afterFirstSet;
        w.fs.inject = [&](const FakeNtSeams::Call& c) -> std::optional<win::Status> {
            if (c.op == QLatin1String("setSecurity") && afterFirstSet && !armed) {
                armed = true;
                return std::nullopt;
            }
            if (armed && c.op == op && c.path.endsWith(target))
                return win::Status::fail(win::Status::Kind::AccessDenied, 5, QStringLiteral("injected"));
            return std::nullopt;
        };
        const OpenedFolder f = TrustedConfig::open(w.folder, TrustMode::Mutating);
        QVERIFY(!f.usable);
        QVERIFY2(f.error.contains(QLatin1String("injected")), qPrintable(f.error));
        QCOMPARE(f.fileChanges, reported);
        // What is reported is what is on disk, and what is on disk is reported.
        QCOMPARE(w.fs.node(kData)->sd == sd::dataFolderDescriptor(), folderNormal || reported.contains(
                     QStringLiteral("secured the data folder (owner Administrators, protected permissions)")));
        QCOMPARE(w.fs.node(cfg)->sd != own, reported.contains(QStringLiteral("reset the permissions of config.json to the folder's")));
    }

    // ---- INV-74: Repair renames aside, deletes nothing -------------------------------------------
    void renameAsideMovesEverythingByHandle()
    {
        World w;
        w.fs.mkdir(kData, descriptor(kUser, {allow(QStringLiteral("S-1-1-0"), sd::kFullControl, 3)}));
        w.fs.mkdir(kData + QStringLiteral("\\sub"));
        w.fs.plantFile(kData + QStringLiteral("\\config.json"), "OLD");
        w.fs.mkdir(QStringLiteral("C:\\ProgramData\\target"));
        w.fs.plantLink(kData + QStringLiteral("\\junction"), QStringLiteral("C:\\ProgramData\\target"), true);
        const FakeNtSeams::NodeP folderNode = w.fs.node(kData);
        const Inspection i = w.inspect(TrustMode::Repair);
        QCOMPARE(i.action, DirAction::Recreate);
        w.fs.calls.clear();
        const QString aside = w.folder.renameAside(i);
        QCOMPARE(aside, QStringLiteral("C:\\ProgramData\\AetherSerialBridge.untrusted-20261014T120000Z-0000abcd"));
        QVERIFY(w.fs.node(aside) == folderNode);   // the same object, moved
        QVERIFY(!w.fs.node(kData));
        QVERIFY(w.fs.node(aside + QStringLiteral("\\config.json")));
        QCOMPARE(w.fs.node(aside + QStringLiteral("\\config.json"))->data, QByteArray("OLD"));
        QVERIFY(w.fs.node(aside + QStringLiteral("\\sub")));
        QVERIFY(w.fs.node(aside + QStringLiteral("\\junction")));
        QVERIFY(w.fs.node(QStringLiteral("C:\\ProgramData\\target")));
        const QList<FakeNtSeams::Call> renames = w.fs.callsOf(QStringLiteral("rename"));
        QCOMPARE(renames.size(), 1);
        QCOMPARE(renames[0].disposition, 0u);   // no replace
        for (const char* op : {"dispose", "setSecurity", "write", "openRelative"}) QCOMPARE(w.fs.callCount(QString::fromLatin1(op)), 0);
    }

    void repairThroughTrustedConfig()
    {
        World w;
        w.fs.mkdir(kData, descriptor(kUser, {allow(kBu, sd::kFullControl, 3)}));
        w.fs.plantFile(kData + QStringLiteral("\\config.json"), "OLD");
        const OpenedFolder mutating = TrustedConfig::open(w.folder, TrustMode::Mutating);
        QVERIFY(!mutating.usable);   // Mutating never repairs
        QCOMPARE(w.fs.callCount(QStringLiteral("rename")), 0);
        const OpenedFolder repaired = TrustedConfig::open(w.folder, TrustMode::Repair);
        QVERIFY2(repaired.usable, qPrintable(repaired.error));
        QCOMPARE(w.fs.node(QStringLiteral("C:\\ProgramData\\AetherSerialBridge.untrusted-20261014T120000Z-0000abcd\\config.json"))->data,
                 QByteArray("OLD"));
        QCOMPARE(w.fs.node(kData)->sd, sd::dataFolderDescriptor());
    }

    void renameAsideFailureChangesNothing()
    {
        World w;
        w.fs.mkdir(kData, descriptor(kUser, {allow(kBu, sd::kFullControl, 3)}));
        const Inspection i = w.inspect(TrustMode::Repair);
        w.fs.inject = [](const FakeNtSeams::Call& c) -> std::optional<win::Status> {
            if (c.op == QLatin1String("rename")) return win::Status::fail(win::Status::Kind::AccessDenied, 5, QStringLiteral("denied"));
            return std::nullopt;
        };
        try {
            w.folder.renameAside(i);
            QFAIL("renameAside must throw");
        } catch (const PlatformError& e) {
            const QString m = QString::fromLocal8Bit(e.what());
            QVERIFY2(m.contains(QLatin1String("takeown")) && m.contains(QLatin1String("icacls")), qPrintable(m));
        }
        QVERIFY(w.fs.node(kData));
    }

    // ---- INV-116: NTFS/ReFS, not remote ----------------------------------------------------------
    void unsupportedVolumeRefused_data()
    {
        QTest::addColumn<QString>("fs");
        QTest::addColumn<bool>("remote");
        QTest::addColumn<bool>("trusted");
        QTest::newRow("NTFS") << QStringLiteral("NTFS") << false << true;
        QTest::newRow("ReFS") << QStringLiteral("ReFS") << false << true;
        QTest::newRow("FAT32") << QStringLiteral("FAT32") << false << false;
        QTest::newRow("exFAT") << QStringLiteral("exFAT") << false << false;
        QTest::newRow("NTFS over SMB") << QStringLiteral("NTFS") << true << false;
    }
    void unsupportedVolumeRefused()
    {
        QFETCH(QString, fs);
        QFETCH(bool, remote);
        QFETCH(bool, trusted);
        World w;
        w.makeTrusted();
        w.fs.volumeInfo = {fs, remote};
        const Inspection i = w.inspect(TrustMode::Repair);
        QCOMPARE(i.action, trusted ? DirAction::Trust : DirAction::Refuse);
    }

    void missingFolderOnAnUnsupportedVolumeIsNeverCreated()
    {
        World w;
        w.fs.volumeInfo = {QStringLiteral("FAT32"), false};
        QCOMPARE(w.inspect().action, DirAction::Refuse);
        const OpenedFolder f = TrustedConfig::open(w.folder, TrustMode::Mutating);
        QVERIFY(!f.usable);
        QCOMPARE(w.fs.callCount(QStringLiteral("createDirectory")), 0);
        QVERIFY(!w.fs.node(kData));
    }

    // INV-116 fails closed: a parent that cannot be opened leaves the volume unknown, so the
    // missing folder is refused before anything is created, not created and refused afterwards.
    void missingFolderOnAnUnknownVolumeIsNeverCreated_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("deny");   // the exact path whose open is refused ("" = none)
        QTest::newRow("parent open denied") << kData << QStringLiteral("C:\\ProgramData");
        QTest::newRow("path has no parent") << QStringLiteral("AetherSerialBridge") << QString();
    }
    void missingFolderOnAnUnknownVolumeIsNeverCreated()
    {
        QFETCH(QString, path);
        QFETCH(QString, deny);
        FakeNtSeams fs;
        setUpMachine(fs);
        WindowsDataFolder folder(fs, path);
        fs.inject = [&](const FakeNtSeams::Call& c) -> std::optional<win::Status> {
            if (!deny.isEmpty() && c.op == QLatin1String("createFile") && c.path == deny)
                return win::Status::fail(win::Status::Kind::AccessDenied, 5, QStringLiteral("injected"));
            return std::nullopt;
        };
        const Inspection i = folder.inspect(TrustMode::Mutating);
        QCOMPARE(i.action, DirAction::Refuse);
        QVERIFY2(i.reason.contains(QLatin1String("cannot be determined")), qPrintable(i.reason));
        const OpenedFolder f = TrustedConfig::open(folder, TrustMode::Repair);
        QVERIFY(!f.usable);
        QVERIFY(f.fileChanges.isEmpty());
        QCOMPARE(fs.callCount(QStringLiteral("createDirectory")), 0);
        QCOMPARE(fs.callCount(QStringLiteral("rename")), 0);
        QVERIFY(!fs.node(kData));
    }

    // ---- trusted owners per mode -----------------------------------------------------------------
    void onlyScratchTrustsTheRunningUser()
    {
        World w;
        w.makeTrusted();
        w.fs.plantFile(kData + QStringLiteral("\\mine.txt"), "x", sd::ownerOnlyDescriptor(kUser));
        w.fs.userSid = kUser;
        for (TrustMode m : {TrustMode::ReadOnly, TrustMode::Mutating, TrustMode::Repair, TrustMode::Service})
            QCOMPARE(w.folder.inspect(m).action, DirAction::Recreate);
        QCOMPARE(w.folder.inspect(TrustMode::Scratch).action, DirAction::Trust);
    }

    // ---- end to end with the core ----------------------------------------------------------------
    void startTokenAndConfigThroughTheCore()
    {
        World w;
        const OpenedFolder f = TrustedConfig::open(w.folder, TrustMode::Mutating);
        QVERIFY2(f.usable, qPrintable(f.error));
        QVERIFY(f.fileChanges.contains(QStringLiteral("created the data folder")));
        WinFileIo io(w.fs, CreateSecurity::dataFolder());
        const WriteResult r = ConfigFiles::writePending(io, w.folder.handle(), "{\"version\":1,\"ports\":[]}");
        QVERIFY2(r.replaced && r.durable, qPrintable(r.error));
        FileStartToken token(w.folder, io);
        const IStartToken::Written t = token.writeFresh(FirewallGeneration{"0123456789abcdef"}, {});
        QVERIFY2(t.replaced && t.durable, qPrintable(t.error));
        QString why;
        const std::optional<IStartToken::Token> read = token.read(&why);
        QVERIFY2(read, qPrintable(why));
        QCOMPARE(read->token, t.token);
        QCOMPARE(w.inspect(TrustMode::Service).action, DirAction::Trust);   // every file we created is trusted (INV-75)
        QString err;
        QCOMPARE(token.revoke({}, &err), IStartToken::Revocation::Done);
        QVERIFY(!w.fs.node(kData + QStringLiteral("\\start.token")));
        QCOMPARE(token.revoke({}, &err), IStartToken::Revocation::Absent);
    }
};

QTEST_GUILESS_MAIN(tst_WindowsDataFolder)
#include "tst_WindowsDataFolder.moc"
