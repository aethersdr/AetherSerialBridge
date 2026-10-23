#pragma once

// The file-I/O seam (WS-3): every write the bridge makes in its data folder goes through these
// interfaces, so the order of the durability barriers (INV-77), the ownership policy (INV-96) and
// exclusive creation (INV-15) are testable without a real file system, and the native rules live in
// one place per OS. Implemented by WS-6 (Windows: CreateFileW(CREATE_NEW, owner SID), FlushFileBuffers,
// MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH), FILE_ID_INFO) and WS-8 (POSIX: openat(O_CREAT|O_EXCL|
// O_NOFOLLOW, 0600), fchown/fchmod on the private fd, fsync, renameat, fsync(dirfd), st_dev/st_ino).
//
// Every name is a single path component inside the directory a DirHandle stands for; nothing here
// takes a path. Nothing here follows a link or a reparse point.

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <array>
#include <functional>
#include <memory>

namespace asb {

// Who owns a file the bridge creates (INV-96). A required argument everywhere; there is no default.
//  Config  — config.json, config.pending.json, start.token and their temps, in the data folder.
//            Windows: owner BUILTIN\Administrators in the create call. POSIX: the temp is created 0600,
//            then fchown(0, 0) + fchmod(0644) on that private fd BEFORE the rename publishes it.
//  Runtime — status.json and the log, written by the service. Windows: owner SYSTEM in the create
//            call. POSIX: NO chown (the systemd DynamicUser owns its RuntimeDirectory), fchmod(0644).
enum class OwnerPolicy { Config, Runtime };

// A file's identity. Windows: FILE_ID_INFO (volume serial number + 128-bit file id). POSIX: st_dev in
// `volume` and st_ino in the first 8 bytes of `id` (little-endian).
struct FileId {
    quint64 volume = 0;
    std::array<quint8, 16> id{};
    bool operator==(const FileId&) const = default;

    static FileId fromPosix(quint64 dev, quint64 ino)
    {
        FileId f;
        f.volume = dev;
        for (int i = 0; i < 8; ++i) f.id[size_t(i)] = quint8(ino >> (8 * i));
        return f;
    }
};

// An open, identity-checked directory. Owned by the IDataFolder that issued it and handed out by
// const reference; valid for that object's lifetime.
class DirHandle {
public:
    virtual ~DirHandle() = default;
    virtual FileId id() const = 0;
};

// Returns a fresh temp name "<base>.<128-bit hex>.asbtmp" on every call (see defaultNameGen in
// AtomicJson.h). Injectable so a test can plant a file at exactly the name that will be generated.
using NameGen = std::function<QString()>;

// One staged write: an exclusively created temp file, kept open from creation to commit (R5 keeps its
// identity, so the bytes read back are the bytes that get renamed).
class TempFile {
public:
    // Deletes the temp (by handle) unless commitAs replaced the target with it.
    virtual ~TempFile() = default;
    virtual bool write(const QByteArray&, QString* err) = 0;        // the whole buffer, or false
    virtual bool flush(QString* err) = 0;                           // FlushFileBuffers / fsync
    virtual QByteArray readBack(qint64 cap, QString* err) = 0;      // from offset 0, through the same handle/fd;
                                                                    // more than `cap` bytes is an error
    virtual FileId id() const = 0;
    // Replacement and durability, reported separately: `replaced` = the rename over `name` happened
    // (the new bytes are visible); `durable` = the directory sync after it succeeded too.
    struct Commit {
        bool replaced = false;
        bool durable = false;
        QString error;
    };
    // Rename over `name` (Windows MOVEFILE_WRITE_THROUGH), then the directory durability barrier.
    // Called after flush(); a TempFile is committed at most once.
    virtual Commit commitAs(const QString& name) = 0;
};

// An existing regular file opened by name relative to a DirHandle, for start.token revocation
// (N8-6, N9-4): no link or reparse point is followed; anything but a regular file is refused.
class ExistingFile {
public:
    virtual ~ExistingFile() = default;
    virtual bool truncate(QString* err) = 0;                        // to 0
    virtual bool writeAll(const QByteArray&, QString* err) = 0;     // full-write loop; short writes and EINTR retried
    virtual bool flush(QString* err) = 0;                           // file data flush
    virtual bool removeByHandle(QString* err) = 0;                  // POSIX: unlinkat after an fstat identity match;
                                                                    // Windows: delete disposition (POSIX semantics)
};

// The exact `*err` text of FileIo::openExisting when the name does not exist (ENOENT /
// ERROR_FILE_NOT_FOUND): the one failure that means "nothing there", e.g. no token to revoke.
inline QString fileNotFoundError() { return QStringLiteral("ENOENT"); }

class FileIo {
public:
    virtual ~FileIo() = default;
    // Exclusive create (O_CREAT|O_EXCL|O_NOFOLLOW / CREATE_NEW) of gen() in `dir`, with the policy's
    // owner applied as described at OwnerPolicy. Calls gen() exactly once; an existing entry of any
    // kind at that name (file, link, directory, reparse point) is a failure, and it is never opened,
    // followed, truncated or removed.
    virtual std::unique_ptr<TempFile> createTemp(const DirHandle& dir, OwnerPolicy, const NameGen& gen,
                                                 QString* err) = 0;
    // Removes `name` by handle; regular files only (a link or directory is refused, not followed).
    virtual bool remove(const DirHandle& dir, const QString& name, QString* err) = 0;
    // The directory durability barrier (fsync(dirfd) / FlushFileBuffers on the directory handle).
    virtual bool syncDir(const DirHandle& dir, QString* err) = 0;
    // nullptr with *err == fileNotFoundError() when `name` does not exist; nullptr with any other error
    // when it cannot be opened safely (link, reparse point, not a regular file, access denied, ...).
    virtual std::unique_ptr<ExistingFile> openExisting(const DirHandle& dir, const QString& name, QString* err) = 0;
};

}  // namespace asb
