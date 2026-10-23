#include "core/FilesystemPolicy.h"

namespace asb::FilesystemPolicy {

Kind fromLinuxMagic(quint64 fType)
{
    switch (fType) {
    case 0xEF53: return Kind::Ext;          // EXT2/3/4_SUPER_MAGIC
    case 0x58465342: return Kind::Xfs;      // XFS_SUPER_MAGIC
    case 0x9123683E: return Kind::Btrfs;    // BTRFS_SUPER_MAGIC
    case 0xF2F52010: return Kind::F2fs;     // F2FS_SUPER_MAGIC
    case 0x794C7630: return Kind::Overlay;  // OVERLAYFS_SUPER_MAGIC
    case 0x01021994: return Kind::Tmpfs;    // TMPFS_MAGIC
    case 0x6969: return Kind::Nfs;          // NFS_SUPER_MAGIC
    case 0xFF534D42:                        // CIFS_SUPER_MAGIC
    case 0xFE534D42:                        // SMB2_SUPER_MAGIC
    case 0x517B: return Kind::Cifs;         // SMB_SUPER_MAGIC
    case 0x65735546: return Kind::Fuse;     // FUSE_SUPER_MAGIC
    default: return Kind::Other;
    }
}

Kind fromWindowsName(const QString& n)
{
    if (n.compare(QStringLiteral("NTFS"), Qt::CaseInsensitive) == 0) return Kind::Ntfs;
    if (n.compare(QStringLiteral("ReFS"), Qt::CaseInsensitive) == 0) return Kind::Refs;
    return Kind::Other;
}

QString name(Kind k)
{
    switch (k) {
    case Kind::Ext: return QStringLiteral("ext2/3/4");
    case Kind::Xfs: return QStringLiteral("XFS");
    case Kind::Btrfs: return QStringLiteral("Btrfs");
    case Kind::F2fs: return QStringLiteral("F2FS");
    case Kind::Overlay: return QStringLiteral("overlayfs");
    case Kind::Tmpfs: return QStringLiteral("tmpfs");
    case Kind::Nfs: return QStringLiteral("NFS");
    case Kind::Cifs: return QStringLiteral("CIFS/SMB");
    case Kind::Fuse: return QStringLiteral("FUSE");
    case Kind::Ntfs: return QStringLiteral("NTFS");
    case Kind::Refs: return QStringLiteral("ReFS");
    case Kind::Other: return QStringLiteral("an unsupported file system");
    }
    return {};
}

bool lockDirectorySupported(Kind k)
{
    return k == Kind::Ext || k == Kind::Xfs || k == Kind::Btrfs || k == Kind::F2fs;
}

bool runDirectorySupported(Kind k)
{
    return k == Kind::Tmpfs;
}

bool linuxDataFolderSupported(Kind k)
{
    return lockDirectorySupported(k) || k == Kind::Overlay;
}

bool windowsVolumeSupported(Kind k, bool remote)
{
    return !remote && (k == Kind::Ntfs || k == Kind::Refs);
}

}  // namespace asb::FilesystemPolicy
