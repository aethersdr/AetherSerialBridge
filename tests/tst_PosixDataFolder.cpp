// PosixDataFolder on a real file system (WS-8; INV-22, 44, 63, 65, 74, 116). The trusted owner is this test's
// own uid (the build container is unprivileged); a "foreign" owner is modelled by trusting another uid.

#include "core/ConfigFiles.h"
#include "core/StartToken.h"
#include "core/TrustedConfig.h"
#include "platform/posix/PosixDataFolder.h"
#include "support/RecordingSys.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#include <unistd.h>

using namespace asb;
using namespace asb::posix;
using asb::test::RecordingSys;

namespace {

constexpr long kOverlay = 0x794C7630;
constexpr long kTmpfs = 0x01021994;
constexpr long kNfs = 0x6969;

QByteArray enc(const QString& p)
{
    return QFile::encodeName(p);
}

void writeFile(const QString& path, const QByteArray& bytes, mode_t mode = 0644)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(bytes);
    f.close();
    QVERIFY(::chmod(enc(path).constData(), mode) == 0);
}

mode_t modeOf(const QString& path)
{
    struct stat st {};
    if (::lstat(enc(path).constData(), &st) != 0) return mode_t(-1);
    return st.st_mode & 07777;
}

struct Fixture {
    QTemporaryDir tmp;
    RecordingSys sys;
    QString folderPath() const { return tmp.path() + QStringLiteral("/aether-serial-bridge"); }
    QString file(const QString& n) const { return folderPath() + QLatin1Char('/') + n; }
    PosixDataFolder::Options opts(uid_t trusted = ::getuid()) const
    {
        PosixDataFolder::Options o;
        o.configOwner = PosixOwner{trusted, ::getgid()};
        o.asideSuffix = [] { return QStringLiteral("untrusted-test"); };
        return o;
    }
    void makeTrustedFolder()
    {
        QVERIFY(QDir().mkdir(folderPath()));
        QVERIFY(::chmod(enc(folderPath()).constData(), 0755) == 0);
        writeFile(file(QStringLiteral("config.json")), "{\"version\":1,\"ports\":[]}");
    }
};

}  // namespace

class tst_PosixDataFolder : public QObject {
    Q_OBJECT

private slots:
    void init() { ::umask(022); }

    void missingIsCreateAndCreatedLockedDown()
    {
        Fixture f;
        PosixDataFolder folder(f.folderPath(), f.opts(), f.sys);
        QCOMPARE(folder.inspect(TrustMode::Mutating).action, DirAction::Create);
        ::umask(077);   // the created folder must still be 0755: the DynamicUser service reads it
        folder.createLockedDown();
        ::umask(022);
        QCOMPARE(modeOf(f.folderPath()), mode_t(0755));
        QCOMPARE(folder.inspect(TrustMode::Mutating).action, DirAction::Trust);
    }
    void trustedFolderReadsThroughTheFd()
    {
        Fixture f;
        f.makeTrustedFolder();
        PosixDataFolder folder(f.folderPath(), f.opts(), f.sys);
        const Inspection i = folder.inspect(TrustMode::Service);
        QCOMPARE(i.action, DirAction::Trust);
        QString err;
        const auto b = folder.readFile(i, QStringLiteral("config.json"), 1024, &err);
        QVERIFY2(b, qPrintable(err));
        QCOMPARE(*b, QByteArray("{\"version\":1,\"ports\":[]}"));
        QVERIFY(!folder.readFile(i, QStringLiteral("config.pending.json"), 1024, &err));
        QCOMPARE(err, fileNotFoundError());
        QVERIFY(!folder.readFile(i, QStringLiteral("config.json"), 3, &err));   // the cap
    }

    // INV-65: every check is made on the fd that is read. A name checked and then opened can be swapped in
    // between; here the untrusted file is what the open gets, and a trusted one is renamed over the name
    // right after the open: a check by name would see the trusted one and read the untrusted bytes.
    void swapAfterLstatDetected()
    {
        Fixture f;
        f.makeTrustedFolder();
        PosixDataFolder folder(f.folderPath(), f.opts(), f.sys);
        const Inspection i = folder.inspect(TrustMode::Service);
        QCOMPARE(i.action, DirAction::Trust);
        // After the inspection: config.json becomes other-writable (planted bytes), and a trusted copy waits.
        writeFile(f.file(QStringLiteral("config.json")), "PLANTED", 0666);
        writeFile(f.folderPath() + QStringLiteral("/../trusted-copy"), "TRUSTED", 0644);
        f.sys.after(QStringLiteral("openat config.json"), [&] {
            QVERIFY(::rename(enc(f.folderPath() + QStringLiteral("/../trusted-copy")).constData(),
                             enc(f.file(QStringLiteral("config.json"))).constData()) == 0);
        });
        QString err;
        const auto b = folder.readFile(i, QStringLiteral("config.json"), 1024, &err);
        QVERIFY2(!b, b ? b->constData() : "");
        QVERIFY2(err.contains(QLatin1String("changed by other accounts")), qPrintable(err));
    }

    void symlinkOrFileAtThePathRefused_data()
    {
        QTest::addColumn<bool>("symlink");
        QTest::newRow("symlink") << true;
        QTest::newRow("file") << false;
    }
    void symlinkOrFileAtThePathRefused()
    {
        QFETCH(bool, symlink);
        Fixture f;
        const QString real = f.tmp.path() + QStringLiteral("/real");
        QVERIFY(QDir().mkdir(real));
        if (symlink) QVERIFY(::symlink(enc(real).constData(), enc(f.folderPath()).constData()) == 0);
        else writeFile(f.folderPath(), "x");
        PosixDataFolder folder(f.folderPath(), f.opts(), f.sys);
        const Inspection i = folder.inspect(TrustMode::Mutating);
        QCOMPARE(i.action, DirAction::Refuse);
        const OpenedFolder o = TrustedConfig::open(folder, TrustMode::Repair);   // not even Repair touches it
        QVERIFY(!o.usable);
        QVERIFY(QFileInfo(f.folderPath()).exists());
        QVERIFY(QDir(real).isEmpty());
    }

    void untrustedFolders_data()
    {
        QTest::addColumn<QString>("what");
        QTest::newRow("folder group-writable") << QStringLiteral("folder-g+w");
        QTest::newRow("folder other-writable") << QStringLiteral("folder-o+w");
        QTest::newRow("child group-writable") << QStringLiteral("child-g+w");
        QTest::newRow("child is a symlink") << QStringLiteral("child-link");
        QTest::newRow("config.json is a directory") << QStringLiteral("config-dir");      // INV-63 native
        QTest::newRow("start.token is a symlink") << QStringLiteral("token-link");        // INV-63 native
        QTest::newRow("start.token is a FIFO") << QStringLiteral("token-fifo");
        QTest::newRow("foreign owner") << QStringLiteral("owner");
    }
    void untrustedFolders()
    {
        QFETCH(QString, what);
        Fixture f;
        f.makeTrustedFolder();
        uid_t trusted = ::getuid();
        if (what == QLatin1String("folder-g+w")) QVERIFY(::chmod(enc(f.folderPath()).constData(), 0775) == 0);
        if (what == QLatin1String("folder-o+w")) QVERIFY(::chmod(enc(f.folderPath()).constData(), 0757) == 0);
        if (what == QLatin1String("child-g+w")) writeFile(f.file(QStringLiteral("notes.txt")), "n", 0664);
        if (what == QLatin1String("child-link"))
            QVERIFY(::symlink("/etc/hostname", enc(f.file(QStringLiteral("notes.txt"))).constData()) == 0);
        if (what == QLatin1String("config-dir")) {
            QVERIFY(QFile::remove(f.file(QStringLiteral("config.json"))));
            QVERIFY(QDir().mkdir(f.file(QStringLiteral("config.json"))));
        }
        if (what == QLatin1String("token-link")) {
            writeFile(f.tmp.path() + QStringLiteral("/elsewhere"), "{}");
            QVERIFY(::symlink(enc(f.tmp.path() + QStringLiteral("/elsewhere")).constData(),
                              enc(f.file(QStringLiteral("start.token"))).constData()) == 0);
        }
        if (what == QLatin1String("token-fifo")) QVERIFY(::mkfifo(enc(f.file(QStringLiteral("start.token"))).constData(), 0644) == 0);
        if (what == QLatin1String("owner")) trusted = ::getuid() + 1;
        PosixDataFolder folder(f.folderPath(), f.opts(trusted), f.sys);
        const Inspection i = folder.inspect(TrustMode::Service);
        QVERIFY2(i.action == DirAction::Recreate, qPrintable(i.reason));
        // Service mode never changes anything (INV-22); ReadOnly neither (INV-44).
        const QStringList before = QDir(f.folderPath()).entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot);
        f.sys.clearLog();
        QVERIFY(!TrustedConfig::open(folder, TrustMode::Service).usable);
        QVERIFY(!TrustedConfig::open(folder, TrustMode::ReadOnly).usable);
        for (const QString& e : f.sys.log())
            QVERIFY2(!e.startsWith(QLatin1String("fchmod")) && !e.startsWith(QLatin1String("fchown")) &&
                         !e.startsWith(QLatin1String("rename")) && !e.startsWith(QLatin1String("unlink")) &&
                         !e.startsWith(QLatin1String("mkdir")) && !e.contains(QLatin1String(" create")),
                     qPrintable(e));
        QCOMPARE(QDir(f.folderPath()).entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot), before);
    }

    void readFileRechecksOnTheFd_data()
    {
        QTest::addColumn<QString>("what");
        QTest::newRow("group-writable") << QStringLiteral("g+w");
        QTest::newRow("symlink") << QStringLiteral("link");
        QTest::newRow("fifo") << QStringLiteral("fifo");
    }
    void readFileRechecksOnTheFd()
    {
        QFETCH(QString, what);
        Fixture f;
        f.makeTrustedFolder();
        PosixDataFolder folder(f.folderPath(), f.opts(), f.sys);
        const Inspection i = folder.inspect(TrustMode::Service);
        QCOMPARE(i.action, DirAction::Trust);
        const QString token = f.file(QStringLiteral("start.token"));
        if (what == QLatin1String("g+w")) writeFile(token, "{}", 0664);
        if (what == QLatin1String("link")) QVERIFY(::symlink(enc(f.file(QStringLiteral("config.json"))).constData(), enc(token).constData()) == 0);
        if (what == QLatin1String("fifo")) QVERIFY(::mkfifo(enc(token).constData(), 0644) == 0);
        QString err;
        QVERIFY(!folder.readFile(i, QStringLiteral("start.token"), 256, &err));   // never blocks on a FIFO
        QVERIFY(!err.isEmpty() && err != fileNotFoundError());
    }
    void readFileRefusesAnotherFolder()
    {
        Fixture f;
        f.makeTrustedFolder();
        PosixDataFolder folder(f.folderPath(), f.opts(), f.sys);
        Inspection i = folder.inspect(TrustMode::Service);
        i.folderId = FileId::fromPosix(1, 2);
        QString err;
        QVERIFY(!folder.readFile(i, QStringLiteral("config.json"), 1024, &err));
    }

    void normaliseRestoresReadableModes()
    {
        Fixture f;
        f.makeTrustedFolder();
        QVERIFY(::chmod(enc(f.folderPath()).constData(), 0700) == 0);
        QVERIFY(::chmod(enc(f.file(QStringLiteral("config.json"))).constData(), 0600) == 0);
        PosixDataFolder folder(f.folderPath(), f.opts(), f.sys);
        const OpenedFolder o = TrustedConfig::open(folder, TrustMode::Mutating);
        QVERIFY2(o.usable, qPrintable(o.error));
        QCOMPARE(modeOf(f.folderPath()), mode_t(0755));
        QCOMPARE(modeOf(f.file(QStringLiteral("config.json"))), mode_t(0644));
        QCOMPARE(o.fileChanges.size(), 2);
    }
    void normaliseRefusesASwappedChild()
    {
        Fixture f;
        f.makeTrustedFolder();
        QVERIFY(::chmod(enc(f.file(QStringLiteral("config.json"))).constData(), 0600) == 0);
        PosixDataFolder folder(f.folderPath(), f.opts(), f.sys);
        const Inspection i = folder.inspect(TrustMode::Mutating);
        writeFile(f.folderPath() + QStringLiteral("/../other"), "x", 0600);
        QVERIFY(::rename(enc(f.folderPath() + QStringLiteral("/../other")).constData(), enc(f.file(QStringLiteral("config.json"))).constData()) == 0);
        QVERIFY_THROWS_EXCEPTION(PlatformError, folder.normalise(i));
        QCOMPARE(modeOf(f.file(QStringLiteral("config.json"))), mode_t(0600));
    }

    // INV-74 POSIX twin of repair_preserves_untrusted_contents: Repair renames the untrusted folder aside with
    // everything in it; nothing inside is opened, deleted or followed.
    void repairPreservesUntrustedContents()
    {
        Fixture f;
        f.makeTrustedFolder();
        QVERIFY(::chmod(enc(f.folderPath()).constData(), 0777) == 0);
        QVERIFY(QDir().mkdir(f.file(QStringLiteral("subdir"))));
        writeFile(f.file(QStringLiteral("subdir/inner.txt")), "INNER");
        writeFile(f.tmp.path() + QStringLiteral("/outside"), "OUTSIDE");
        QVERIFY(::symlink(enc(f.tmp.path() + QStringLiteral("/outside")).constData(), enc(f.file(QStringLiteral("link"))).constData()) == 0);
        writeFile(f.file(QStringLiteral("start.token")), "{\"token\":\"x\"}");
        PosixDataFolder folder(f.folderPath(), f.opts(), f.sys);
        const OpenedFolder o = TrustedConfig::open(folder, TrustMode::Repair);
        QVERIFY2(o.usable, qPrintable(o.error));
        const QString aside = f.tmp.path() + QStringLiteral("/aether-serial-bridge.untrusted-test");
        QCOMPARE(QDir(aside).entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot, QDir::Name),
                 (QStringList{QStringLiteral("config.json"), QStringLiteral("link"), QStringLiteral("start.token"), QStringLiteral("subdir")}));
        QFile inner(aside + QStringLiteral("/subdir/inner.txt"));
        QVERIFY(inner.open(QIODevice::ReadOnly));
        QCOMPARE(inner.readAll(), QByteArray("INNER"));
        QFile out(f.tmp.path() + QStringLiteral("/outside"));
        QVERIFY(out.open(QIODevice::ReadOnly));
        QCOMPARE(out.readAll(), QByteArray("OUTSIDE"));
        QCOMPARE(QFileInfo(aside + QStringLiteral("/link")).symLinkTarget(), f.tmp.path() + QStringLiteral("/outside"));
        QCOMPARE(modeOf(aside), mode_t(0777));
        // A fresh, trusted, empty folder took its place.
        QVERIFY(QDir(f.folderPath()).isEmpty());
        QCOMPARE(modeOf(f.folderPath()), mode_t(0755));
        // Nothing inside the aside folder was opened or unlinked.
        for (const QString& e : f.sys.log())
            QVERIFY2(!e.startsWith(QLatin1String("unlinkat")) && !e.startsWith(QLatin1String("openat subdir")) &&
                         !e.startsWith(QLatin1String("openat link")) && !e.startsWith(QLatin1String("openat start.token")),
                     qPrintable(e));
    }
    void renameAsideNeverReplaces()
    {
        Fixture f;
        f.makeTrustedFolder();
        QVERIFY(::chmod(enc(f.folderPath()).constData(), 0777) == 0);
        const QString taken = f.tmp.path() + QStringLiteral("/aether-serial-bridge.untrusted-test");
        QVERIFY(QDir().mkdir(taken));
        writeFile(taken + QStringLiteral("/keep"), "KEEP");
        PosixDataFolder folder(f.folderPath(), f.opts(), f.sys);
        const Inspection i = folder.inspect(TrustMode::Repair);
        QVERIFY_THROWS_EXCEPTION(PlatformError, folder.renameAside(i));
        QVERIFY(QFileInfo::exists(taken + QStringLiteral("/keep")));
        QVERIFY(QFileInfo::exists(f.file(QStringLiteral("config.json"))));
    }
    void renameAsideRefusesASwappedFolder()
    {
        Fixture f;
        f.makeTrustedFolder();
        PosixDataFolder folder(f.folderPath(), f.opts(), f.sys);
        const Inspection i = folder.inspect(TrustMode::Repair);
        QVERIFY(::rename(enc(f.folderPath()).constData(), enc(f.tmp.path() + QStringLiteral("/moved")).constData()) == 0);
        QVERIFY(QDir().mkdir(f.folderPath()));
        QVERIFY_THROWS_EXCEPTION(PlatformError, folder.renameAside(i));
        QVERIFY(QFileInfo::exists(f.folderPath()));
    }

    void scratchModeAlsoTrustsTheInvokingUser()
    {
        Fixture f;
        f.makeTrustedFolder();
        PosixDataFolder::Options o = f.opts(::getuid() + 1);
        o.scratchUser = ::getuid();
        PosixDataFolder folder(f.folderPath(), o, f.sys);
        QCOMPARE(folder.inspect(TrustMode::Scratch).action, DirAction::Trust);
        QCOMPARE(folder.inspect(TrustMode::Service).action, DirAction::Recreate);
    }

    // ReadFile trusts the owners the inspection trusted, no more: in Scratch mode the invoking
    // user's config.json is read; the same file under a Service inspection is refused.
    void scratchModeReadsTheInvokingUsersFile()
    {
        Fixture f;
        f.makeTrustedFolder();
        PosixDataFolder::Options o = f.opts(::getuid() + 1);
        o.scratchUser = ::getuid();
        PosixDataFolder folder(f.folderPath(), o, f.sys);
        QString err;
        const Inspection scratch = folder.inspect(TrustMode::Scratch);
        QCOMPARE(scratch.action, DirAction::Trust);
        const auto b = folder.readFile(scratch, QStringLiteral("config.json"), 1024, &err);
        QVERIFY2(b, qPrintable(err));
        const Inspection service = folder.inspect(TrustMode::Service);
        QVERIFY(!folder.readFile(service, QStringLiteral("config.json"), 1024, &err));
        QVERIFY2(err.contains(QStringLiteral("owned by uid")), qPrintable(err));
    }

    // A Scratch-mode refusal names every owner Scratch trusts (root and the invoking user), not only root.
    // The Service-mode refusal names the one owner it trusts.
    void scratchOwnerRefusalNamesBothOwners()
    {
        Fixture f;
        f.makeTrustedFolder();
        PosixDataFolder::Options o = f.opts(::getuid() + 1);
        o.scratchUser = ::getuid() + 2;   // the file's owner (getuid()) is neither
        PosixDataFolder folder(f.folderPath(), o, f.sys);
        QString err;
        const Inspection scratch = folder.inspect(TrustMode::Scratch);
        QVERIFY(!folder.readFile(scratch, QStringLiteral("config.json"), 1024, &err));
        QCOMPARE(err, QStringLiteral("config.json is owned by uid %1, not by uid %2 or uid %3 (the user running this command)")
                          .arg(::getuid()).arg(::getuid() + 1).arg(::getuid() + 2));
        const Inspection service = folder.inspect(TrustMode::Service);
        QVERIFY(!folder.readFile(service, QStringLiteral("config.json"), 1024, &err));
        QCOMPARE(err, QStringLiteral("config.json is owned by uid %1, not by uid %2").arg(::getuid()).arg(::getuid() + 1));
    }

    // The new folder's entry must be durable before anything is saved into it. A failed fsync
    // of the parent fails the step, and the folder it created is still reported (PlatformErrorAfterChanges).
    void createLockedDownReportsAFailedParentFlush()
    {
        Fixture f;
        f.sys.failWith(QStringLiteral("fsync ") + f.tmp.path(), EIO);
        PosixDataFolder folder(f.folderPath(), f.opts(), f.sys);
        QCOMPARE(folder.inspect(TrustMode::Mutating).action, DirAction::Create);
        try {
            folder.createLockedDown();
            QFAIL("a failed parent fsync was not reported");
        } catch (const PlatformErrorAfterChanges& e) {
            QCOMPARE(e.changes, QStringList{QStringLiteral("created the data folder")});
            QVERIFY2(QString::fromLocal8Bit(e.what()).contains(QStringLiteral("cannot flush")), e.what());
        }
        // Through TrustedConfig: not usable (nothing is saved), and the creation is in fileChanges.
        QVERIFY(QDir(f.folderPath()).removeRecursively());
        const OpenedFolder o = TrustedConfig::open(folder, TrustMode::Mutating);
        QVERIFY(!o.usable);
        QVERIFY2(o.fileChanges.contains(QStringLiteral("created the data folder")), qPrintable(o.fileChanges.join(QLatin1Char(';'))));
        QVERIFY2(o.error.contains(QStringLiteral("cannot flush")), qPrintable(o.error));
    }
    // The same class (WS-6's INV-76 contract): normalise reports the changes it made before a later failure,
    // and renameAside reports the move when the parent cannot be flushed after it.
    void normaliseFailureCarriesTheChangesAlreadyMade()
    {
        Fixture f;
        f.makeTrustedFolder();
        QVERIFY(::chmod(enc(f.folderPath()).constData(), 0700) == 0);
        QVERIFY(::chmod(enc(f.file(QStringLiteral("config.json"))).constData(), 0600) == 0);
        f.sys.failWith(QStringLiteral("fchmod config.json"), EPERM);
        PosixDataFolder folder(f.folderPath(), f.opts(), f.sys);
        const Inspection i = folder.inspect(TrustMode::Mutating);
        try {
            folder.normalise(i);
            QFAIL("the failure was not reported");
        } catch (const PlatformErrorAfterChanges& e) {
            QCOMPARE(e.changes.size(), 1);
            QVERIFY2(e.changes.first().contains(QStringLiteral("-> 0755")), qPrintable(e.changes.first()));
        }
        QCOMPARE(modeOf(f.folderPath()), mode_t(0755));
    }
    void renameAsideReportsAFailedParentFlush()
    {
        Fixture f;
        f.makeTrustedFolder();
        QVERIFY(::chmod(enc(f.folderPath()).constData(), 0777) == 0);
        PosixDataFolder folder(f.folderPath(), f.opts(), f.sys);
        const Inspection i = folder.inspect(TrustMode::Repair);
        f.sys.failWith(QStringLiteral("fsync ") + f.tmp.path(), EIO);
        try {
            folder.renameAside(i);
            QFAIL("the failed flush was not reported");
        } catch (const PlatformErrorAfterChanges& e) {
            QCOMPARE(e.changes.size(), 1);
            QVERIFY2(e.changes.first().contains(QStringLiteral("untrusted-test")), qPrintable(e.changes.first()));
        }
        QVERIFY(QFileInfo::exists(f.tmp.path() + QStringLiteral("/aether-serial-bridge.untrusted-test/config.json")));
    }

    // INV-116: the data folder may be any local file system or overlayfs, never tmpfs or a network one.
    void fileSystemPolicy_data()
    {
        QTest::addColumn<long>("fs");
        QTest::addColumn<bool>("trusted");
        QTest::newRow("overlayfs") << kOverlay << true;
        QTest::newRow("tmpfs") << kTmpfs << false;
        QTest::newRow("nfs") << kNfs << false;
    }
    void fileSystemPolicy()
    {
        QFETCH(long, fs);
        QFETCH(bool, trusted);
        Fixture f;
        f.makeTrustedFolder();
        f.sys.fsType = fs;
        PosixDataFolder folder(f.folderPath(), f.opts(), f.sys);
        const Inspection i = folder.inspect(TrustMode::Service);
        QCOMPARE(i.action == DirAction::Trust, trusted);
        if (!trusted) QCOMPARE(i.action, DirAction::Refuse);
    }

    // The start token over this folder: FileStartToken + PosixFileIo (WS-5 hand-off).
    void startTokenRoundTripOnTheNativeFolder()
    {
        Fixture f;
        f.makeTrustedFolder();
        PosixDataFolder folder(f.folderPath(), f.opts(), f.sys);
        PosixFileIo io(f.sys, PosixOwner::current(), 0644);
        QVERIFY(TrustedConfig::open(folder, TrustMode::Mutating).usable);
        FileStartToken token(folder, io);
        const IStartToken::Written w = token.writeFresh(FirewallGeneration::fresh(), std::chrono::steady_clock::now() + std::chrono::seconds(5));
        QVERIFY2(w.replaced && w.durable, qPrintable(w.error));
        QCOMPARE(modeOf(f.file(QStringLiteral("start.token"))), mode_t(0644));
        QString why;
        const auto t = token.read(&why);
        QVERIFY2(t, qPrintable(why));
        QCOMPARE(t->token, w.token);
        QString err;
        QCOMPARE(token.revoke(std::chrono::steady_clock::now() + std::chrono::seconds(5), &err), IStartToken::Revocation::Done);
        QVERIFY(!QFileInfo::exists(f.file(QStringLiteral("start.token"))));
        QCOMPARE(token.revoke(std::chrono::steady_clock::now() + std::chrono::seconds(5), &err), IStartToken::Revocation::Absent);
    }
};

QTEST_GUILESS_MAIN(tst_PosixDataFolder)
#include "tst_PosixDataFolder.moc"
