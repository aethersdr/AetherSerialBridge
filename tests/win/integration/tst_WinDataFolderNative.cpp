// WS-6 native rows on windows-latest (admin), label win-integration: the Windows data folder against NTFS, real
// links, real accounts and the object-creator owner policy (INV-16, 17, 19, 40, 62, 63, 74, 75, 116).
#include "WinNativeSupport.h"

#include "core/AtomicJson.h"
#include "core/ConfigFiles.h"
#include "core/StartToken.h"
#include "core/TrustedConfig.h"
#include "platform/win/NativeSeams.h"
#include "platform/win/SecurityDescriptor.h"
#include "platform/win/WinLogFile.h"
#include "platform/win/WindowsDataFolder.h"

#include <QTest>

using namespace asb;
using namespace asb::win;
using namespace asb::test::native;

namespace {
const QString kOurs = QStringLiteral("O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)");
// SetKernelObjectSecurity and a create both leave SE_DACL_AUTO_INHERITED clear (probed): no "AI".
const QString kInheritedFile = QStringLiteral("D:(A;ID;FA;;;SY)(A;ID;FA;;;BA)(A;ID;0x1200a9;;;BU)");

QString ownerOf(const QString& path)
{
    const QString s = sddlOf(path, OWNER_SECURITY_INFORMATION);
    return s.startsWith(QLatin1String("O:")) ? s.mid(2) : s;
}

bool inCi()
{
    return qEnvironmentVariableIsSet("CI");
}
}  // namespace

class tst_WinDataFolderNative : public QObject {
    Q_OBJECT

    QString m_root, m_data;
    NtSeams& seams() { return nativeSeams(); }

    void refusedUntouched(const QString& kind)
    {
        const QString target = m_root + QStringLiteral("\\target");
        QVERIFY(CreateDirectoryW(W(target), nullptr));
        QVERIFY(setSddl(target, QStringLiteral("D:P(A;OICI;FA;;;WD)"), DACL_SECURITY_INFORMATION));
        QVERIFY(writeFile(target + QStringLiteral("\\config.json"), "TARGET"));
        if (kind == QLatin1String("junction")) QVERIFY(makeJunction(m_data, target));
        if (kind == QLatin1String("symlink")) QVERIFY2(makeSymlink(m_data, target, true), qPrintable(QString::number(GetLastError())));
        if (kind == QLatin1String("file")) QVERIFY(writeFile(m_data, "a file"));
        const QString targetSd = sddlOf(target);
        WindowsDataFolder folder(seams(), m_data);
        for (TrustMode m : {TrustMode::ReadOnly, TrustMode::Mutating, TrustMode::Repair, TrustMode::Service}) {
            const Inspection i = folder.inspect(m);
            QCOMPARE(i.action, DirAction::Refuse);
        }
        QVERIFY(!TrustedConfig::open(folder, TrustMode::Repair).usable);
        QVERIFY(!TrustedConfig::open(folder, TrustMode::Mutating).usable);
        QCOMPARE(isReparse(m_data), kind != QLatin1String("file"));   // the link itself is still there
        QCOMPARE(sddlOf(target), targetSd);
        QCOMPARE(readFile(target + QStringLiteral("\\config.json")).value_or("<missing>"), QByteArray("TARGET"));
        QCOMPARE(QDir(m_root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden).size(), 2);
    }

    Inspection makeTrusted(WindowsDataFolder& folder)
    {
        const OpenedFolder f = TrustedConfig::open(folder, TrustMode::Mutating);
        if (!f.usable) qWarning("not usable: %s", qPrintable(f.error));
        WinFileIo io(seams(), CreateSecurity::dataFolder());
        const WriteResult r = atomicWrite(io, folder.handle(), QStringLiteral("config.json"), "{\"version\":1,\"ports\":[]}", OwnerPolicy::Config);
        if (!r.replaced) qWarning("write failed: %s", qPrintable(r.error));
        return folder.inspect(TrustMode::Mutating);
    }

private slots:
    void init()
    {
        m_root = scratch(QStringLiteral("df"));
        m_data = m_root + QStringLiteral("\\AetherSerialBridge");
    }
    void cleanup() { removeTree(m_root); }

    // ---- the pure codec against Windows' own descriptors (INV-19) --------------------------------
    void rawDescriptorMatchesWindows_data()
    {
        QTest::addColumn<QString>("sddlText");
        QTest::addColumn<QString>("decoded");   // sd::toDisplay of our decode
        QTest::newRow("ours") << kOurs
                              << QStringLiteral("O:S-1-5-32-544 D:P(0;0x3;0x1f01ff;S-1-5-18)(0;0x3;0x1f01ff;S-1-5-32-544)(0;0x3;0x1200a9;S-1-5-32-545)");
        QTest::newRow("deny, inherited, Everyone") << QStringLiteral("O:SYD:(D;;FA;;;BU)(A;ID;0x1200a9;;;WD)")
                                                   << QStringLiteral("O:S-1-5-18 D:(1;0x0;0x1f01ff;S-1-5-32-545)(0;0x10;0x1200a9;S-1-1-0)");
        QTest::newRow("inherit-only CREATOR OWNER") << QStringLiteral("O:BAD:(A;OICIIO;GA;;;CO)")
                                                    << QStringLiteral("O:S-1-5-32-544 D:(0;0xb;0x10000000;S-1-3-0)");
        QTest::newRow("object ACE") << QStringLiteral("O:BAD:(OA;;FA;bf967aba-0de6-11d0-a285-00aa003049e2;;BU)")
                                    << QStringLiteral("O:S-1-5-32-544 D:(5;0x0;0x1f01ff;?)");
        QTest::newRow("conditional (callback) ACE") << QStringLiteral("O:BAD:(XA;;FA;;;WD;(Member_of {SID(BA)}))")
                                                    << QStringLiteral("O:S-1-5-32-544 D:(9;0x0;0x1f01ff;?)");
        QTest::newRow("NULL DACL") << QStringLiteral("O:BAD:NO_ACCESS_CONTROL") << QStringLiteral("O:S-1-5-32-544 D:<null>");
    }
    void rawDescriptorMatchesWindows()
    {
        QFETCH(QString, sddlText);
        QFETCH(QString, decoded);
        const QByteArray bytes = sddl(sddlText);
        QVERIFY2(!bytes.isEmpty(), qPrintable(sddlText));
        const std::optional<sd::Descriptor> d = sd::parse(bytes);
        QVERIFY(d);
        QCOMPARE(sd::toDisplay(*d), decoded);
    }

    void descriptorsWeBuildAreValidForWindows()
    {
        QByteArray folder = sd::dataFolderDescriptor();
        QVERIFY(IsValidSecurityDescriptor(folder.data()));
        QCOMPARE(toSddl(folder), kOurs);
        QByteArray lock = sd::lockFileDescriptor();
        QVERIFY(IsValidSecurityDescriptor(lock.data()));
        QCOMPARE(toSddl(lock), QStringLiteral("O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)"));
        QByteArray owner = sd::ownerOnlyDescriptor(QString::fromLatin1(sd::kSystem));
        QVERIFY(IsValidSecurityDescriptor(owner.data()));
        QCOMPARE(toSddl(owner, OWNER_SECURITY_INFORMATION), QStringLiteral("O:SY"));
    }

    // ---- INV-16 ------------------------------------------------------------------------------------
    void createdLockedDownInOneCall()
    {
        WindowsDataFolder folder(seams(), m_data);
        QCOMPARE(folder.inspect(TrustMode::Mutating).action, DirAction::Create);
        folder.createLockedDown();
        QCOMPARE(sddlOf(m_data), kOurs);
        for (TrustMode m : {TrustMode::ReadOnly, TrustMode::Mutating, TrustMode::Repair, TrustMode::Service, TrustMode::Scratch})
            QCOMPARE(folder.inspect(m).action, DirAction::Trust);
        QVERIFY_THROWS_EXCEPTION(PlatformError, folder.createLockedDown());   // never "already there, fine"
    }

    // ---- INV-17 ------------------------------------------------------------------------------------
    void junction_at_path_refused() { refusedUntouched(QStringLiteral("junction")); }
    void symlink_at_path_refused() { refusedUntouched(QStringLiteral("symlink")); }
    void file_at_path_refused() { refusedUntouched(QStringLiteral("file")); }

    // ---- INV-19 ------------------------------------------------------------------------------------
    void child_explicit_users_write_detected()
    {
        WindowsDataFolder folder(seams(), m_data);
        QCOMPARE(makeTrusted(folder).action, DirAction::Trust);
        const QString cfg = m_data + QStringLiteral("\\config.json");
        QCOMPARE(sddlOf(cfg, DACL_SECURITY_INFORMATION), kInheritedFile);
        QVERIFY(setSddl(cfg, QStringLiteral("D:AI(A;;0x2;;;BU)(A;ID;FA;;;SY)(A;ID;FA;;;BA)(A;ID;0x1200a9;;;BU)"), DACL_SECURITY_INFORMATION));
        Inspection i = folder.inspect(TrustMode::Service);
        QCOMPARE(i.action, DirAction::Recreate);
        QVERIFY2(i.reason.contains(QLatin1String("config.json")), qPrintable(i.reason));
        // A conditional (callback) ACE, even for SYSTEM and read only, is untrusted.
        QVERIFY(setSddl(cfg, QStringLiteral("D:AI(XA;;0x1;;;SY;(Member_of {SID(BA)}))(A;ID;FA;;;SY)(A;ID;FA;;;BA)"), DACL_SECURITY_INFORMATION));
        QCOMPARE(folder.inspect(TrustMode::Service).action, DirAction::Recreate);
        QVERIFY(setSddl(cfg, kInheritedFile, DACL_SECURITY_INFORMATION));
        QCOMPARE(folder.inspect(TrustMode::Service).action, DirAction::Trust);
        // An inherit-only Users write entry on the folder (what new children would receive) counts too.
        QVERIFY(setSddl(m_data, QStringLiteral("D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)(A;OICIIO;0x2;;;BU)"),
                        DACL_SECURITY_INFORMATION));
        QCOMPARE(folder.inspect(TrustMode::Service).action, DirAction::Recreate);
    }

    // ---- INV-40 ------------------------------------------------------------------------------------
    void unreadable_child_untrusted()
    {
        WindowsDataFolder folder(seams(), m_data);
        QCOMPARE(makeTrusted(folder).action, DirAction::Trust);
        {
            Privilege restore(SE_RESTORE_NAME);
            QVERIFY(restore.ok());
            // Owner SYSTEM, only SYSTEM may read its security: an administrator cannot read it (no backup intent).
            QVERIFY(writeFile(m_data + QStringLiteral("\\sealed.txt"), "x", QStringLiteral("O:SYD:P(A;;FA;;;SY)")));
        }
        const Inspection i = folder.inspect(TrustMode::Mutating);
        QCOMPARE(i.action, DirAction::Recreate);
        QVERIFY2(i.reason.contains(QLatin1String("sealed.txt")), qPrintable(i.reason));
    }

    // ---- INV-62 ------------------------------------------------------------------------------------
    void identity_swap_aborts_native()
    {
        WindowsDataFolder folder(seams(), m_data);
        QCOMPARE(makeTrusted(folder).action, DirAction::Trust);
        QVERIFY(setSddl(m_data, QStringLiteral("D:AI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"), DACL_SECURITY_INFORMATION));   // something to normalise
        const Inspection i = folder.inspect(TrustMode::Repair);
        QCOMPARE(i.action, DirAction::Trust);
        // Swap: the inspected folder moves away and a different one takes its path.
        QVERIFY(MoveFileExW(W(m_data), W(m_data + QStringLiteral(".moved")), 0));
        QVERIFY(CreateDirectoryW(W(m_data), nullptr));
        const QString movedSd = sddlOf(m_data + QStringLiteral(".moved")), newSd = sddlOf(m_data);
        QVERIFY_THROWS_EXCEPTION(PlatformError, folder.normalise(i));
        QVERIFY_THROWS_EXCEPTION(PlatformError, folder.renameAside(i));
        QCOMPARE(sddlOf(m_data + QStringLiteral(".moved")), movedSd);
        QCOMPARE(sddlOf(m_data), newSd);
        QVERIFY(GetFileAttributesW(W(m_data)) != INVALID_FILE_ATTRIBUTES);
    }

    void normalise_native()
    {
        WindowsDataFolder folder(seams(), m_data);
        QCOMPARE(makeTrusted(folder).action, DirAction::Trust);
        const QString cfg = m_data + QStringLiteral("\\config.json");
        QVERIFY(setSddl(cfg, QStringLiteral("D:P(A;;FA;;;SY)(A;;FA;;;BA)"), DACL_SECURITY_INFORMATION));
        // (SetKernelObjectSecurity keeps an all-explicit DACL protected, so the folder differs by an entry instead.)
        QVERIFY(setSddl(m_data, QStringLiteral("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"), DACL_SECURITY_INFORMATION));
        const Inspection i = folder.inspect(TrustMode::Mutating);
        QCOMPARE(i.action, DirAction::Trust);
        const QStringList changes = folder.normalise(i);
        QVERIFY2(changes.size() == 2, qPrintable(changes.join(QStringLiteral(" | "))));
        QCOMPARE(sddlOf(m_data), kOurs);
        QCOMPARE(sddlOf(cfg), QStringLiteral("O:BA") + kInheritedFile);   // owner kept, DACL = exactly the inherited entries
        QVERIFY(folder.normalise(folder.inspect(TrustMode::Mutating)).isEmpty());
    }

    // ---- INV-74 (WS-5 deferred row) -------------------------------------------------------------------
    void repair_preserves_untrusted_contents()
    {
        const QString outside = m_root + QStringLiteral("\\outside");
        QVERIFY(CreateDirectoryW(W(outside), nullptr));
        QVERIFY(writeFile(outside + QStringLiteral("\\canary.txt"), "CANARY"));
        QVERIFY(CreateDirectoryW(W(m_data), nullptr));
        QVERIFY(setSddl(m_data, QStringLiteral("D:PAI(A;OICI;FA;;;WD)"), DACL_SECURITY_INFORMATION));
        QVERIFY(CreateDirectoryW(W(m_data + QStringLiteral("\\sub")), nullptr));
        QVERIFY(writeFile(m_data + QStringLiteral("\\sub\\inner.txt"), "INNER"));
        QVERIFY(writeFile(m_data + QStringLiteral("\\config.json"), "OLD"));
        QVERIFY(makeJunction(m_data + QStringLiteral("\\j"), outside));
        WindowsDataFolder folder(seams(), m_data);
        QCOMPARE(folder.inspect(TrustMode::Repair).action, DirAction::Recreate);
        const OpenedFolder f = TrustedConfig::open(folder, TrustMode::Repair);
        QVERIFY2(f.usable, qPrintable(f.error));
        QCOMPARE(sddlOf(m_data), kOurs);
        QVERIFY(QDir(m_data).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden).isEmpty());
        const QStringList aside = QDir(m_root).entryList({QStringLiteral("AetherSerialBridge.untrusted-*")}, QDir::Dirs | QDir::System | QDir::Hidden);
        QCOMPARE(aside.size(), 1);
        const QString a = m_root + QLatin1Char('\\') + aside[0];
        QCOMPARE(readFile(a + QStringLiteral("\\config.json")).value_or("<missing>"), QByteArray("OLD"));
        QCOMPARE(readFile(a + QStringLiteral("\\sub\\inner.txt")).value_or("<missing>"), QByteArray("INNER"));
        QVERIFY(isReparse(a + QStringLiteral("\\j")));   // still a junction, not walked
        QCOMPARE(readFile(outside + QStringLiteral("\\canary.txt")).value_or("<missing>"), QByteArray("CANARY"));
        QVERIFY(f.fileChanges.join(QLatin1Char(' ')).contains(aside[0]));
    }

    // ---- INV-63 (WS-5 deferred rows) ----------------------------------------------------------------
    void start_token_directory_untrusted()
    {
        WindowsDataFolder folder(seams(), m_data);
        QCOMPARE(makeTrusted(folder).action, DirAction::Trust);
        QVERIFY(CreateDirectoryW(W(m_data + QStringLiteral("\\start.token")), nullptr));
        QCOMPARE(folder.inspect(TrustMode::Service).action, DirAction::Recreate);
        WinFileIo io(seams(), CreateSecurity::dataFolder());
        FileStartToken token(folder, io);
        QString why;
        QVERIFY(!token.read(&why));
        QVERIFY2(why.contains(QLatin1String("not trusted")), qPrintable(why));
    }

    void config_json_directory_untrusted_native()
    {
        WindowsDataFolder folder(seams(), m_data);
        QVERIFY(TrustedConfig::open(folder, TrustMode::Mutating).usable);
        QVERIFY(CreateDirectoryW(W(m_data + QStringLiteral("\\config.json")), nullptr));
        const OpenedFolder f = TrustedConfig::open(folder, TrustMode::Service);
        QVERIFY(!f.usable);
        QCOMPARE(f.inspection.action, DirAction::Recreate);
        QVERIFY2(f.inspection.reason.contains(QLatin1String("config.json")), qPrintable(f.inspection.reason));
    }

    // ---- INV-116 (WS-5 deferred rows) ---------------------------------------------------------------
    void ntfs_refs_only()
    {
        QCOMPARE(WindowsDataFolder(seams(), m_data).inspect(TrustMode::Mutating).action, DirAction::Create);   // NTFS C:
        const QString fat = qEnvironmentVariable("ASB_FAT_VOLUME");
        if (fat.isEmpty()) {
            if (inCi()) QFAIL("ASB_FAT_VOLUME is not set: the CI step that mounts a FAT32 volume did not run");
            QSKIP("no FAT32 volume (set ASB_FAT_VOLUME)");
        }
        const QString path = nativePath(fat) + QStringLiteral("AetherSerialBridge-%1").arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
        WindowsDataFolder folder(seams(), path);
        const Inspection i = folder.inspect(TrustMode::Mutating);
        QCOMPARE(i.action, DirAction::Refuse);
        QVERIFY2(i.reason.contains(QLatin1String("FAT")), qPrintable(i.reason));
        QVERIFY(!TrustedConfig::open(folder, TrustMode::Mutating).usable);
        QCOMPARE(GetFileAttributesW(W(path)), INVALID_FILE_ATTRIBUTES);   // refused before any change
        // An existing folder there is refused too.
        QVERIFY(CreateDirectoryW(W(path), nullptr));
        QCOMPARE(folder.inspect(TrustMode::Repair).action, DirAction::Refuse);
        QVERIFY(!TrustedConfig::open(folder, TrustMode::Repair).usable);
        QVERIFY(GetFileAttributesW(W(path)) != INVALID_FILE_ATTRIBUTES);   // Repair did not move it either
        RemoveDirectoryW(W(path));
    }

    void network_volume_refused()
    {
        WindowsDataFolder local(seams(), m_data);
        QVERIFY(TrustedConfig::open(local, TrustMode::Mutating).usable);
        QVERIFY(m_data.size() > 3 && m_data[1] == QLatin1Char(':'));
        const QString unc = QStringLiteral("\\\\localhost\\%1$").arg(m_data[0]) + m_data.mid(2);
        if (GetFileAttributesW(W(unc)) == INVALID_FILE_ATTRIBUTES) {
            if (inCi()) QFAIL(qPrintable(QStringLiteral("the admin share is not reachable: %1 (%2)").arg(unc).arg(GetLastError())));
            QSKIP("the admin share is not reachable");
        }
        WindowsDataFolder remote(seams(), unc);
        const Inspection i = remote.inspect(TrustMode::Service);
        QCOMPARE(i.action, DirAction::Refuse);
        QVERIFY2(i.reason.contains(QLatin1String("network")), qPrintable(i.reason));
        WindowsDataFolder missing(seams(), unc + QStringLiteral("-absent"));
        QCOMPARE(missing.inspect(TrustMode::Mutating).action, DirAction::Refuse);
    }

    // ---- INV-75 --------------------------------------------------------------------------------------
    void object_creator_policy_files_trusted()
    {
        HANDLE creator = objectCreatorToken();
        QVERIFY(creator);
        const QString me = sidOfToken(creator);
        // The policy is in effect under this token: a file created without an explicit owner is the user's.
        {
            Impersonate as(creator);
            QVERIFY(as.ok());
            QVERIFY(writeFile(m_root + QStringLiteral("\\plain.txt"), "x"));
        }
        QCOMPARE(ownerSidOf(m_root + QStringLiteral("\\plain.txt")), me);

        WindowsDataFolder folder(seams(), m_data);
        WinFileIo io(seams(), CreateSecurity::dataFolder());
        {
            Impersonate as(creator);   // Save & apply, --add-port, an unticked save: Config-policy writes
            QVERIFY(TrustedConfig::open(folder, TrustMode::Mutating).usable);
            QVERIFY(atomicWrite(io, folder.handle(), QStringLiteral("config.json"), "{\"version\":1,\"ports\":[]}", OwnerPolicy::Config).replaced);
            QVERIFY(ConfigFiles::writePending(io, folder.handle(), "{\"version\":1,\"ports\":[]}").replaced);
            FileStartToken token(folder, io);
            QVERIFY(token.writeFresh(FirewallGeneration{"0123456789abcdef"}, {}).replaced);
        }
        {
            Privilege restore(SE_RESTORE_NAME);   // the harness stands in for LocalSystem (the service writes these)
            QVERIFY(restore.ok());
            QVERIFY(folder.inspect(TrustMode::Service).action == DirAction::Trust);
            QVERIFY(atomicWrite(io, folder.handle(), QStringLiteral("status.json"), "{}", OwnerPolicy::Runtime).replaced);
            QString err;
            QVERIFY2(WinLogFile(seams(), folder.handle()).append("line\n", &err), qPrintable(err));
        }
        for (const char* n : {"config.json", "config.pending.json", "start.token"})
            QCOMPARE(ownerOf(m_data + QLatin1Char('\\') + QLatin1String(n)), QStringLiteral("BA"));
        for (const char* n : {"status.json", "bridge.log"}) QCOMPARE(ownerOf(m_data + QLatin1Char('\\') + QLatin1String(n)), QStringLiteral("SY"));

        // The service's view (Service mode: SYSTEM + Administrators, no running-user exception).
        QCOMPARE(folder.inspect(TrustMode::Service).action, DirAction::Trust);
        WinFileIo io2(seams(), CreateSecurity::dataFolder());
        QString why;
        QVERIFY2(FileStartToken(folder, io2).read(&why), qPrintable(why));

        // A second administrator's ReadOnly view, and a standard user's.
        LocalAccount admin2(true), standard(false);
        QVERIFY2(admin2.token(), qPrintable(admin2.error()));
        QVERIFY2(standard.token(), qPrintable(standard.error()));
        for (const LocalAccount* a : {&admin2, &standard}) {
            Impersonate as(a->token());
            QVERIFY(as.ok());
            WindowsDataFolder view(seams(), m_data);
            const Inspection i = view.inspect(TrustMode::ReadOnly);
            QCOMPARE(i.action, DirAction::Trust);
            QString err;
            QVERIFY2(view.readFile(i, QStringLiteral("config.json"), 1024, &err), qPrintable(err));
        }

        // Positive control: the same writes WITHOUT an owner in the create call would not be trusted.
        {
            Impersonate as(creator);
            WinFileIo defaultSa(seams(), CreateSecurity{});
            QVERIFY(atomicWrite(defaultSa, folder.handle(), QStringLiteral("other.json"), "{}", OwnerPolicy::Config).replaced);
        }
        QCOMPARE(ownerSidOf(m_data + QStringLiteral("\\other.json")), me);
        QCOMPARE(folder.inspect(TrustMode::Service).action, DirAction::Recreate);
        CloseHandle(creator);
    }

    void read_file_rechecks_the_token_handle()
    {
        WindowsDataFolder folder(seams(), m_data);
        QCOMPARE(makeTrusted(folder).action, DirAction::Trust);
        WinFileIo io(seams(), CreateSecurity::dataFolder());
        FileStartToken token(folder, io);
        QVERIFY(token.writeFresh(FirewallGeneration{"0123456789abcdef"}, {}).replaced);
        QString why;
        QVERIFY2(token.read(&why), qPrintable(why));
        // Its own (trusted) DACL instead of the folder's: refused (master §4.6: inherit-only DACL).
        QVERIFY(setSddl(m_data + QStringLiteral("\\start.token"), QStringLiteral("D:P(A;;FA;;;SY)(A;;FA;;;BA)"), DACL_SECURITY_INFORMATION));
        QVERIFY(!token.read(&why));
        QVERIFY2(why.contains(QLatin1String("permissions of its own")), qPrintable(why));
    }
};

QTEST_GUILESS_MAIN(tst_WinDataFolderNative)
#include "tst_WinDataFolderNative.moc"
