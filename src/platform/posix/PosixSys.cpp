#include "platform/posix/PosixSys.h"

#include <dirent.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace asb::posix {
namespace {

class RealSys final : public PosixSys {
public:
    int open(const char* path, int flags, mode_t mode) override { return ::open(path, flags, mode); }
    int openat(int dirfd, const char* name, int flags, mode_t mode) override { return ::openat(dirfd, name, flags, mode); }
    int close(int fd) override { return ::close(fd); }
    int fstat(int fd, struct stat* st) override { return ::fstat(fd, st); }
    int fstatat(int dirfd, const char* name, struct stat* st, int flags) override { return ::fstatat(dirfd, name, st, flags); }
    int fstatfs(int fd, struct statfs* st) override { return ::fstatfs(fd, st); }
    int fchown(int fd, uid_t uid, gid_t gid) override { return ::fchown(fd, uid, gid); }
    int fchmod(int fd, mode_t mode) override { return ::fchmod(fd, mode); }
    ssize_t read(int fd, void* buf, size_t n) override { return ::read(fd, buf, n); }
    ssize_t pread(int fd, void* buf, size_t n, off_t off) override { return ::pread(fd, buf, n, off); }
    ssize_t write(int fd, const void* buf, size_t n) override { return ::write(fd, buf, n); }
    int ftruncate(int fd, off_t len) override { return ::ftruncate(fd, len); }
    int fsync(int fd) override { return ::fsync(fd); }
    int renameat(int a, const char* an, int b, const char* bn) override { return ::renameat(a, an, b, bn); }
    int renameNoReplace(int a, const char* an, int b, const char* bn) override
    {
        return ::renameat2(a, an, b, bn, RENAME_NOREPLACE);
    }
    int unlinkat(int dirfd, const char* name, int flags) override { return ::unlinkat(dirfd, name, flags); }
    int mkdirat(int dirfd, const char* name, mode_t mode) override { return ::mkdirat(dirfd, name, mode); }
    int flock(int fd, int op) override { return ::flock(fd, op); }
    ssize_t readlinkat(int dirfd, const char* name, char* buf, size_t n) override
    {
        return ::readlinkat(dirfd, name, buf, n);
    }
    std::optional<QStringList> listDirectory(int dirfd) override
    {
        const int dup = ::fcntl(dirfd, F_DUPFD_CLOEXEC, 0);
        if (dup < 0) return std::nullopt;
        DIR* d = ::fdopendir(dup);
        if (!d) {
            const int e = errno;
            ::close(dup);
            errno = e;
            return std::nullopt;
        }
        ::rewinddir(d);
        QStringList names;
        errno = 0;
        while (struct dirent* de = ::readdir(d)) {
            if (std::strcmp(de->d_name, ".") == 0 || std::strcmp(de->d_name, "..") == 0) continue;
            names << QString::fromLocal8Bit(de->d_name);
            errno = 0;
        }
        const int e = errno;
        ::closedir(d);
        if (e != 0) {
            errno = e;
            return std::nullopt;
        }
        return names;
    }
};

}  // namespace

PosixSys& realPosixSys()
{
    static RealSys sys;
    return sys;
}

QString errnoText(int err)
{
    const char* name = strerrorname_np(err);
    return QStringLiteral("%1: %2").arg(QString::fromLatin1(name ? name : "errno"), QString::fromLocal8Bit(std::strerror(err)));
}

QString errnoText()
{
    return errnoText(errno);
}

PosixOwner PosixOwner::current()
{
    return {::getuid(), ::getgid()};
}

std::optional<QByteArray> preadAll(PosixSys& sys, int fd, qint64 cap, QString* err)
{
    QByteArray out;
    char buf[65536];
    off_t off = 0;
    for (;;) {
        const ssize_t n = sys.pread(fd, buf, sizeof buf, off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (err) *err = errnoText();
            return std::nullopt;
        }
        if (n == 0) break;
        out.append(buf, int(n));
        off += n;
        if (out.size() > cap) {
            if (err) *err = QStringLiteral("larger than %1 bytes").arg(cap);
            return std::nullopt;
        }
    }
    return out;
}

bool writeAll(PosixSys& sys, int fd, const QByteArray& bytes, QString* err)
{
    qsizetype done = 0;
    while (done < bytes.size()) {
        const ssize_t n = sys.write(fd, bytes.constData() + done, size_t(bytes.size() - done));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (err) *err = errnoText();
            return false;
        }
        if (n == 0) {
            if (err) *err = QStringLiteral("write returned 0");
            return false;
        }
        done += n;
    }
    return true;
}

}  // namespace asb::posix
