// WS-6 native rows on windows-latest (admin), label win-integration: the six INV-15 FileIoContract checks against
// the real WinFileIo on NTFS (the WS-3 hand-off), reader_does_not_block_rename, the native create-time owner
// (INV-75, INV-96) and the directory flush (INV-77).
#include "WinNativeSupport.h"

#include "core/AtomicJson.h"
#include "core/TrustedConfig.h"
#include "platform/win/NativeSeams.h"
#include "platform/win/WinFileIo.h"
#include "platform/win/WinLogFile.h"
#include "platform/win/WindowsDataFolder.h"
#include "support/FileIoContract.h"

#include <QTest>

using namespace asb;
using namespace asb::win;
using namespace asb::test;
using namespace asb::test::native;

namespace {

class NativeHarness final : public FileIoHarness {
public:
    NativeHarness()
        : m_root(scratch(QStringLiteral("io"))), m_dir(m_root + QStringLiteral("\\AetherSerialBridge")),
          m_folder(nativeSeams(), m_dir), m_io(nativeSeams(), CreateSecurity::dataFolder())
    {
        m_ok = TrustedConfig::open(m_folder, TrustMode::Mutating).usable;
    }
    ~NativeHarness() override { removeTree(m_root); }
    bool ok() const { return m_ok; }
    QString path(const QString& name) const { return m_dir + QLatin1Char('\\') + name; }
    WindowsDataFolder& folder() { return m_folder; }

    FileIo& io() override { return m_io; }
    const DirHandle& dir() override { return m_folder.handle(); }
    void plantFile(const QString& name, const QByteArray& bytes) override { QVERIFY(writeFile(path(name), bytes)); }
    void plantSymlink(const QString& name, const QString& target) override
    {
        QVERIFY2(CreateSymbolicLinkW(W(path(name)), W(target), 0), qPrintable(QString::number(GetLastError())));   // relative target
    }
    void plantDirectory(const QString& name) override { QVERIFY(CreateDirectoryW(W(path(name)), nullptr)); }
    std::optional<QByteArray> read(const QString& name) override
    {
        const DWORD a = GetFileAttributesW(W(path(name)));
        if (a == INVALID_FILE_ATTRIBUTES || (a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return std::nullopt;
        return readFile(path(name));
    }
    QStringList names() override
    {
        QStringList n = QDir(m_dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden);
        n.sort();
        return n;
    }

private:
    QString m_root, m_dir;
    WindowsDataFolder m_folder;
    WinFileIo m_io;
    bool m_ok = false;
};

QString ownerOf(const QString& path)
{
    return sddlOf(path, OWNER_SECURITY_INFORMATION).mid(2);
}

}  // namespace

class tst_WinFileIoNative : public QObject {
    Q_OBJECT

private slots:
    // ---- INV-15: the WS-3 FileIoContract against the native FileIo -------------------------------
    void fileIoContract_data()
    {
        QTest::addColumn<int>("check");
        QTest::newRow("plantedGeneratedNameNotReused") << 0;
        QTest::newRow("plantedSymlinkNotFollowed") << 1;
        QTest::newRow("tempRemovedOnFailure") << 2;
        QTest::newRow("repeatedSavesLeaveOnlyTheTarget") << 3;
        QTest::newRow("readBackThroughTheSameHandle") << 4;
        QTest::newRow("openExistingMissingAndLinks") << 5;
    }
    void fileIoContract()
    {
        QFETCH(int, check);
        NativeHarness h;
        QVERIFY(h.ok());
        switch (check) {
        case 0: FileIoContract::plantedGeneratedNameNotReused(h); break;
        case 1: FileIoContract::plantedSymlinkNotFollowed(h); break;
        case 2: FileIoContract::tempRemovedOnFailure(h); break;
        case 3: FileIoContract::repeatedSavesLeaveOnlyTheTarget(h); break;
        case 4: FileIoContract::readBackThroughTheSameHandle(h); break;
        case 5: FileIoContract::openExistingMissingAndLinks(h); break;
        }
    }

    // ---- INV-15: a reader sharing delete never blocks the replace ---------------------------------
    void reader_does_not_block_rename()
    {
        NativeHarness h;
        QVERIFY(h.ok());
        QVERIFY(atomicWrite(h.io(), h.dir(), QStringLiteral("config.json"), "OLD", OwnerPolicy::Config).replaced);
        // A reader as the bridge opens files (share read/write/delete), held open across the save.
        HANDLE reader = CreateFileW(W(h.path(QStringLiteral("config.json"))), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(reader != INVALID_HANDLE_VALUE);
        const WriteResult r = atomicWrite(h.io(), h.dir(), QStringLiteral("config.json"), "NEW", OwnerPolicy::Config);
        QVERIFY2(r.replaced && r.durable, qPrintable(r.error));
        char buf[8] = {};
        DWORD got = 0;
        QVERIFY(ReadFile(reader, buf, sizeof buf, &got, nullptr));
        QCOMPARE(QByteArray(buf, int(got)), QByteArray("OLD"));   // the reader keeps the old bytes
        CloseHandle(reader);
        QCOMPARE(h.read(QStringLiteral("config.json")).value_or("<missing>"), QByteArray("NEW"));
        QCOMPARE(h.names(), QStringList{QStringLiteral("config.json")});
        // And through the adapter's own read path, also held across a save.
        const Inspection i = h.folder().inspect(TrustMode::ReadOnly);
        QString err;
        QCOMPARE(h.folder().readFile(i, QStringLiteral("config.json"), 64, &err).value_or("<fail>"), QByteArray("NEW"));
    }

    // ---- INV-75 / INV-96: owner in the create call --------------------------------------------------
    void create_time_owner_native()
    {
        NativeHarness h;
        QVERIFY(h.ok());
        QVERIFY(atomicWrite(h.io(), h.dir(), QStringLiteral("config.json"), "{}", OwnerPolicy::Config).replaced);
        QCOMPARE(ownerOf(h.path(QStringLiteral("config.json"))), QStringLiteral("BA"));
        // Runtime = SYSTEM: only a SYSTEM process (the service) or one holding SeRestorePrivilege may create it.
        const WriteResult denied = atomicWrite(h.io(), h.dir(), QStringLiteral("status.json"), "{}", OwnerPolicy::Runtime);
        QVERIFY(!denied.replaced);
        QVERIFY2(denied.error.contains(QLatin1String("0xc000005a")), qPrintable(denied.error));   // STATUS_INVALID_OWNER, not a silent default
        QCOMPARE(h.names(), QStringList{QStringLiteral("config.json")});
        Privilege restore(SE_RESTORE_NAME);
        QVERIFY(restore.ok());
        QVERIFY(atomicWrite(h.io(), h.dir(), QStringLiteral("status.json"), "{}", OwnerPolicy::Runtime).replaced);
        QCOMPARE(ownerOf(h.path(QStringLiteral("status.json"))), QStringLiteral("SY"));
        QString err;
        WinLogFile log(nativeSeams(), h.dir());
        QVERIFY2(log.append("a\n", &err), qPrintable(err));
        QCOMPARE(ownerOf(h.path(QStringLiteral("bridge.log"))), QStringLiteral("SY"));
        QVERIFY2(log.rollover(&err), qPrintable(err));
        QVERIFY2(log.append("b\n", &err), qPrintable(err));
        QCOMPARE(h.read(QStringLiteral("bridge.log.1")).value_or("<missing>"), QByteArray("a\n"));
        QCOMPARE(h.read(QStringLiteral("bridge.log")).value_or("<missing>"), QByteArray("b\n"));
        QCOMPARE(h.folder().inspect(TrustMode::Service).action, DirAction::Trust);
    }

    // ---- INV-77: the directory durability barrier works on NTFS ------------------------------------
    void directory_flush_native()
    {
        NativeHarness h;
        QVERIFY(h.ok());
        QString err;
        QVERIFY2(h.io().syncDir(h.dir(), &err), qPrintable(err));
        const WriteResult r = atomicWrite(h.io(), h.dir(), QStringLiteral("config.pending.json"), "{}", OwnerPolicy::Config);
        QVERIFY(r.replaced);
        QVERIFY2(r.durable, qPrintable(r.error));
    }

    void revocation_by_handle_native()
    {
        NativeHarness h;
        QVERIFY(h.ok());
        QVERIFY(atomicWrite(h.io(), h.dir(), QStringLiteral("start.token"), "TOKEN", OwnerPolicy::Config).replaced);
        QString err;
        std::unique_ptr<ExistingFile> f = h.io().openExisting(h.dir(), QStringLiteral("start.token"), &err);
        QVERIFY2(f, qPrintable(err));
        QVERIFY(f->truncate(&err));
        QVERIFY(f->writeAll("{\"revoked\":true}", &err));
        QVERIFY(f->flush(&err));
        QCOMPARE(h.read(QStringLiteral("start.token")).value_or("<missing>"), QByteArray("{\"revoked\":true}"));
        QVERIFY2(f->removeByHandle(&err), qPrintable(err));
        f.reset();
        QVERIFY(h.names().isEmpty());
    }
};

QTEST_GUILESS_MAIN(tst_WinFileIoNative)
#include "tst_WinFileIoNative.moc"
