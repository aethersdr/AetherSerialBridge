// Supported file systems (INV-116, pure half; the native fstatfs/volume rows are WS-6's and WS-8's).

#include "core/FilesystemPolicy.h"

#include <QTest>

using namespace asb::FilesystemPolicy;

class tst_FilesystemPolicy : public QObject {
    Q_OBJECT
private slots:
    void linuxMagicNumbers_data();
    void linuxMagicNumbers();
    void lockDirectoryOnlyOnPersistentLocalFileSystems();
    void overlayLockDirRefused();
    void runMustBeTmpfs();
    void dataFolderMayBeOverlay();
    void windowsNtfsOrRefsAndNotRemote();
};

void tst_FilesystemPolicy::linuxMagicNumbers_data()
{
    QTest::addColumn<quint64>("magic");
    QTest::addColumn<int>("kind");
    QTest::newRow("ext4") << quint64(0xEF53) << int(Kind::Ext);
    QTest::newRow("xfs") << quint64(0x58465342) << int(Kind::Xfs);
    QTest::newRow("btrfs") << quint64(0x9123683E) << int(Kind::Btrfs);
    QTest::newRow("f2fs") << quint64(0xF2F52010) << int(Kind::F2fs);
    QTest::newRow("overlay") << quint64(0x794C7630) << int(Kind::Overlay);
    QTest::newRow("tmpfs") << quint64(0x01021994) << int(Kind::Tmpfs);
    QTest::newRow("nfs") << quint64(0x6969) << int(Kind::Nfs);
    QTest::newRow("cifs") << quint64(0xFF534D42) << int(Kind::Cifs);
    QTest::newRow("smb2") << quint64(0xFE534D42) << int(Kind::Cifs);
    QTest::newRow("fuse") << quint64(0x65735546) << int(Kind::Fuse);
    QTest::newRow("zfs") << quint64(0x2FC12FC1) << int(Kind::Other);
}

void tst_FilesystemPolicy::linuxMagicNumbers()
{
    QFETCH(quint64, magic);
    QFETCH(int, kind);
    QCOMPARE(int(fromLinuxMagic(magic)), kind);
}

void tst_FilesystemPolicy::lockDirectoryOnlyOnPersistentLocalFileSystems()
{
    for (Kind k : {Kind::Ext, Kind::Xfs, Kind::Btrfs, Kind::F2fs}) QVERIFY(lockDirectorySupported(k));
    for (Kind k : {Kind::Tmpfs, Kind::Nfs, Kind::Cifs, Kind::Fuse, Kind::Ntfs, Kind::Other}) QVERIFY(!lockDirectorySupported(k));
}

// INV-116. Kills: accepting overlayfs for the lock directory.
void tst_FilesystemPolicy::overlayLockDirRefused()
{
    QVERIFY(!lockDirectorySupported(Kind::Overlay));
    QVERIFY(!lockDirectorySupported(fromLinuxMagic(0x794C7630)));
}

void tst_FilesystemPolicy::runMustBeTmpfs()
{
    QVERIFY(runDirectorySupported(Kind::Tmpfs));
    QVERIFY(!runDirectorySupported(Kind::Ext));
}

void tst_FilesystemPolicy::dataFolderMayBeOverlay()
{
    QVERIFY(linuxDataFolderSupported(Kind::Overlay));
    QVERIFY(linuxDataFolderSupported(Kind::Ext));
    QVERIFY(!linuxDataFolderSupported(Kind::Nfs));
    QVERIFY(!linuxDataFolderSupported(Kind::Tmpfs));
}

void tst_FilesystemPolicy::windowsNtfsOrRefsAndNotRemote()
{
    QVERIFY(windowsVolumeSupported(fromWindowsName(QStringLiteral("NTFS")), false));
    QVERIFY(windowsVolumeSupported(fromWindowsName(QStringLiteral("ReFS")), false));
    QVERIFY(!windowsVolumeSupported(Kind::Ntfs, true));
    QVERIFY(!windowsVolumeSupported(fromWindowsName(QStringLiteral("FAT32")), false));
    QVERIFY(!windowsVolumeSupported(fromWindowsName(QStringLiteral("exFAT")), false));
}

QTEST_GUILESS_MAIN(tst_FilesystemPolicy)
#include "tst_FilesystemPolicy.moc"
