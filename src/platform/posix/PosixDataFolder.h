#pragma once

// /etc/aether-serial-bridge: the POSIX IDataFolder (WS-8; master §4.4; INV-22, 44, 63, 65, 74, 116). The
// decision itself is DataDirectoryTrust::decide; this adapter gathers the facts on fds and acts through
// identity-checked fds.
//
//  inspect      the parent is opened O_DIRECTORY, the folder RELATIVE to it with O_DIRECTORY | O_NOFOLLOW: a
//               symlink or a file at the path is Refuse (ELOOP / ENOTDIR), nothing there is Create. fstat on
//               the folder fd: owner and mode; fstatfs: a local file system or overlayfs (INV-116), else
//               Refuse. Children are listed through that fd and judged with fstatat(AT_SYMLINK_NOFOLLOW): a
//               link is untrusted, a known name must be a regular file (INV-63), every owner must be trusted
//               and nothing may be group- or other-writable. A child that vanished between the listing and
//               its fstatat is not evidence either way.
//  readFile     openat(folder fd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC), then fstat ON THAT FD
//               (INV-65): a regular file, owned by an owner the last inspection trusted (the Config owner; in
//               Scratch mode also the invoking user, as policyFor says), not group/other-writable; the bytes
//               come from the same fd with a hard cap. A name is never checked and then opened.
//  createLockedDown   mkdirat(parent fd, name, 0755): root-owned in the create call (the Applier is root),
//               then fchmod 0755 on the new folder's own fd (umask cannot narrow it below what the
//               DynamicUser service must read), then fsync of the parent. A failure after the mkdirat throws
//               PlatformErrorAfterChanges ("created the data folder"); so do normalise and renameAside once they
//               changed something (renameAside: the parent's fsync after the rename).
//  normalise    through the folder fd and fds opened O_NOFOLLOW whose st_dev/st_ino equal what the inspection
//               saw (else nothing is changed): folder mode 0755, the known files 0644.
//  renameAside  Repair only (S2, INV-74): renameat2(RENAME_NOREPLACE) of the folder, after its identity is
//               re-checked by fstatat on the parent, to "<name>.untrusted-<UTC>-<rand>". Nothing inside is
//               opened, read or deleted.
//
// Owners and modes are expressed for DataDirectoryTrust as an owner "uid:<n>" and one Allow entry per
// writable class ("group:<gid>" for S_IWGRP, "other" for S_IWOTH) with the write right: group or other
// write is therefore untrusted whoever the group is (the master's "mode & 022 == 0"). POSIX ACLs need no
// separate read: a named-user or named-group write entry raises the mask, which fstat reports as S_IWGRP.

#include "core/DataDirectoryTrust.h"
#include "core/Interfaces.h"
#include "platform/posix/PosixFileIo.h"
#include "platform/posix/PosixSys.h"

#include <QMap>

#include <functional>

namespace asb::posix {

class PosixDataFolder final : public IDataFolder {
public:
    struct Options {
        PosixOwner configOwner = PosixOwner::root();   // owns config, pending, token; the trusted owner
        uid_t scratchUser = uid_t(-1);                 // also trusted in Scratch mode (the invoking euid)
        std::function<QString()> asideSuffix;          // default: "untrusted-<UTC>-<8 hex>"
    };
    // `path`: absolute, e.g. /etc/aether-serial-bridge (from serviceLocations(), or a test root).
    PosixDataFolder(QString path, Options options, PosixSys& sys = realPosixSys());
    explicit PosixDataFolder(QString path) : PosixDataFolder(std::move(path), Options{}) {}

    Inspection inspect(TrustMode) override;
    std::optional<QByteArray> readFile(const Inspection&, const QString& name, qint64 cap, QString* err) override;
    void createLockedDown() override;
    QStringList normalise(const Inspection&) override;
    QString renameAside(const Inspection&) override;
    const DirHandle& handle() const override { return m_dir; }
    // WS-16 (INV-125): config.json, config.pending.json and start.token (the status lives under /run, the log in the
    // journal). removeIfEmpty: unlinkat(parent fd, name, AT_REMOVEDIR) after the identity is re-checked (rmdir never
    // removes a folder with anything in it). asideFolders: the "<name>.untrusted-*" siblings renameAside made.
    QStringList knownFiles() const override;
    bool removeIfEmpty(const Inspection&, QString* err) override;
    void closeIdle() override { m_dir = PosixDirHandle(); }   // v1.1 INV-128
    QStringList asideFolders() override;

    QString path() const { return m_path; }
    static QString defaultAsideSuffix();

    // The owner/mode encoding described above (also used by the tests).
    static FsEntry entryFor(const QString& name, const struct stat& st);
    static TrustPolicy policyFor(TrustMode, const Options&);

private:
    int openParent();
    bool sameFolder(const Inspection&) const;

    PosixSys& m_sys;
    QString m_path, m_parent, m_name;
    Options m_opt;
    PosixDirHandle m_dir;
    QMap<QString, FileId> m_children;   // the last inspection's regular files and their identity
    TrustMode m_mode = TrustMode::Service;   // the last inspection's mode: readFile trusts the owners it trusted
};

}  // namespace asb::posix
