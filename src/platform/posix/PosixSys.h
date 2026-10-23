#pragma once

// The POSIX system-call seam of the Linux adapters (WS-8). Every file-system call the data folder, the
// FileIo, the locks and the systemd file steps make goes through one PosixSys, so the order of the
// durability barriers (INV-77), the ownership step before a rename (INV-75, INV-96) and the checks made on
// an opened fd rather than on a name (INV-65) are observable and injectable in tests, which wrap the real
// calls (tests/support/RecordingSys). Production uses realPosixSys(): a thin pass-through, nothing more.
//
// Conventions: the C library's. A failing call returns -1 (or nullopt) with errno set; nothing throws.
// Every fd an adapter opens carries O_CLOEXEC (no process is ever created, S17; defence in depth).

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>

#include <optional>

namespace asb::posix {

class PosixSys {
public:
    virtual ~PosixSys() = default;

    virtual int open(const char* path, int flags, mode_t mode = 0) = 0;
    virtual int openat(int dirfd, const char* name, int flags, mode_t mode = 0) = 0;
    virtual int close(int fd) = 0;
    virtual int fstat(int fd, struct stat* st) = 0;
    // AT_SYMLINK_NOFOLLOW is the only flag the adapters pass.
    virtual int fstatat(int dirfd, const char* name, struct stat* st, int flags) = 0;
    virtual int fstatfs(int fd, struct statfs* st) = 0;
    virtual int fchown(int fd, uid_t uid, gid_t gid) = 0;
    virtual int fchmod(int fd, mode_t mode) = 0;
    virtual ssize_t read(int fd, void* buf, size_t n) = 0;
    virtual ssize_t pread(int fd, void* buf, size_t n, off_t off) = 0;
    virtual ssize_t write(int fd, const void* buf, size_t n) = 0;
    virtual int ftruncate(int fd, off_t len) = 0;
    virtual int fsync(int fd) = 0;
    virtual int renameat(int olddirfd, const char* oldname, int newdirfd, const char* newname) = 0;
    // renameat2(RENAME_NOREPLACE): never replaces an existing entry.
    virtual int renameNoReplace(int olddirfd, const char* oldname, int newdirfd, const char* newname) = 0;
    virtual int unlinkat(int dirfd, const char* name, int flags) = 0;
    virtual int mkdirat(int dirfd, const char* name, mode_t mode) = 0;
    virtual int flock(int fd, int op) = 0;
    virtual ssize_t readlinkat(int dirfd, const char* name, char* buf, size_t n) = 0;
    // The names in the directory behind `dirfd` (not "." or ".."), read through a duplicate of it, so the
    // caller's fd keeps its offset and stays open. nullopt with errno set on failure.
    virtual std::optional<QStringList> listDirectory(int dirfd) = 0;
};

PosixSys& realPosixSys();

// errno as text ("ENOENT: No such file or directory").
QString errnoText(int err);
QString errnoText();   // the current errno

// Owns one fd; closes it through the seam.
class Fd {
public:
    Fd() = default;
    Fd(PosixSys& sys, int fd) : m_sys(&sys), m_fd(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : m_sys(o.m_sys), m_fd(o.m_fd) { o.m_fd = -1; }
    Fd& operator=(Fd&& o) noexcept
    {
        if (this != &o) {
            reset();
            m_sys = o.m_sys;
            m_fd = o.m_fd;
            o.m_fd = -1;
        }
        return *this;
    }
    int get() const { return m_fd; }
    bool valid() const { return m_fd >= 0; }
    explicit operator bool() const { return valid(); }
    void reset()
    {
        if (m_fd >= 0 && m_sys) m_sys->close(m_fd);
        m_fd = -1;
    }
    int release()
    {
        const int f = m_fd;
        m_fd = -1;
        return f;
    }

private:
    PosixSys* m_sys = nullptr;
    int m_fd = -1;
};

// Who owns what the bridge creates as root. Production: uid 0, gid 0. The native tests in the
// unprivileged build container pass their own uid/gid (vetoable, like WS-6's SeRestorePrivilege harness):
// the code path — fchown on the private fd, then fchmod, before the rename — is the same; only the ids
// differ. Rows that need real root ownership run in the systemd container.
struct PosixOwner {
    uid_t uid = 0;
    gid_t gid = 0;
    static PosixOwner root() { return {}; }
    static PosixOwner current();   // getuid()/getgid(): tests only
};

// Reads at most `cap` + 1 bytes from offset 0 of `fd` with pread (so a shared offset never matters);
// nullopt with *err when the read fails or the file holds more than `cap` bytes.
std::optional<QByteArray> preadAll(PosixSys&, int fd, qint64 cap, QString* err);
// The full-write loop: short writes and EINTR retried.
bool writeAll(PosixSys&, int fd, const QByteArray& bytes, QString* err);

}  // namespace asb::posix
