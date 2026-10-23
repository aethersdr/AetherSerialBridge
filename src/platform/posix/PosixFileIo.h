#pragma once

// The POSIX FileIo (WS-8; WS-3 seam; INV-15, INV-75, INV-77, INV-96). Every call is relative to a
// directory fd; nothing takes a path and nothing follows a link.
//
//  createTemp   openat(dirfd, gen(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600): an existing
//               entry of any kind at the generated name is a failure, never opened, followed or removed
//               (INV-15). Config policy: fchown(owner) then fchmod(0644) on that private fd, BEFORE anything
//               can rename it into view (INV-75). Runtime policy: no chown at all (the systemd DynamicUser
//               owns its RuntimeDirectory, INV-96), fchmod(0644).
//  flush        fsync(temp) — before the rename (INV-77).
//  commitAs     renameat(dirfd, temp, dirfd, name) = replaced; then fsync(dirfd) = durable (INV-77).
//  ~TempFile    an uncommitted temp is unlinked only while the name still names the temp's own inode.
//  remove       fstatat(AT_SYMLINK_NOFOLLOW): regular files only (a link or directory is refused), unlinkat.
//  openExisting openat(O_WRONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC) + fstat on the fd: regular files only.
//
// The directory itself is a PosixDirHandle: an O_DIRECTORY | O_NOFOLLOW fd that the IDataFolder (or the
// lock directory, or the runtime directory) opened and judged.

#include "core/FileIo.h"
#include "platform/posix/PosixSys.h"

#include <memory>

namespace asb::posix {

class PosixDirHandle final : public DirHandle {
public:
    PosixDirHandle() = default;
    PosixDirHandle(PosixSys& sys, int fd);   // takes ownership
    FileId id() const override { return m_id; }
    int fd() const { return m_fd.get(); }
    bool valid() const { return m_fd.valid(); }
    void reset(PosixSys& sys, int fd);       // takes ownership; refreshes id()
    // Why the directory could not be opened (set by whoever failed to open it; reported by PosixFileIo).
    QString openError;

private:
    Fd m_fd;
    FileId m_id;
};

class PosixFileIo final : public FileIo {
public:
    // `configOwner` receives every Config-policy file (production: root). `mode` is what every created file
    // ends with (0644: the DynamicUser service reads config.json, start.token and the lock files).
    explicit PosixFileIo(PosixSys& sys = realPosixSys(), PosixOwner configOwner = PosixOwner::root(), mode_t mode = 0644);

    std::unique_ptr<TempFile> createTemp(const DirHandle& dir, OwnerPolicy, const NameGen& gen, QString* err) override;
    bool remove(const DirHandle& dir, const QString& name, QString* err) override;
    bool syncDir(const DirHandle& dir, QString* err) override;
    std::unique_ptr<ExistingFile> openExisting(const DirHandle& dir, const QString& name, QString* err) override;

    PosixSys& sys() { return m_sys; }

private:
    PosixSys& m_sys;
    PosixOwner m_owner;
    mode_t m_mode;
};

// The fd of a DirHandle issued by the POSIX adapters; -1 for anything else.
int dirFd(const DirHandle&);
// The error text for a DirHandle without an fd.
QString noDirectoryError(const DirHandle&);

}  // namespace asb::posix
