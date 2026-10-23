#pragma once

// Supported file systems (INV-116; master §4.6 "Filesystem support"). Pure: the adapters read the
// file-system type (Linux fstatfs f_type, Windows GetVolumeInformationByHandleW + GetDriveType) and
// these functions decide. Anything not listed is refused before any mutation; there is no fallback.

#include <QString>
#include <QtGlobal>

namespace asb::FilesystemPolicy {

enum class Kind { Ext, Xfs, Btrfs, F2fs, Overlay, Tmpfs, Nfs, Cifs, Fuse, Ntfs, Refs, Other };

// Linux statfs f_type magic numbers (linux/magic.h; ext2/3/4 share EXT4_SUPER_MAGIC).
Kind fromLinuxMagic(quint64 fType);
// Windows file-system name from GetVolumeInformationByHandleW ("NTFS", "ReFS", "FAT32", ...).
Kind fromWindowsName(const QString& name);
QString name(Kind);

// /var/lib/aether-serial-bridge (engine.lock, engine.lock.id): a persistent local file system whose
// lock and rename semantics we rely on — ext*, XFS, Btrfs, F2FS. Not overlayfs, tmpfs, NFS, CIFS, FUSE.
bool lockDirectorySupported(Kind);
// /run (apply.lock): tmpfs.
bool runDirectorySupported(Kind);
// The Linux data folder: any of the local types above, or overlayfs (containers).
bool linuxDataFolderSupported(Kind);
// Windows data folder and %ProgramFiles% directory: NTFS or ReFS, and not a remote volume.
bool windowsVolumeSupported(Kind, bool remote);

}  // namespace asb::FilesystemPolicy
