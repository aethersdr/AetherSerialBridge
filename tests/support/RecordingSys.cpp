#include "support/RecordingSys.h"

#include <QMutexLocker>

#include <cerrno>

namespace asb::test {

QStringList RecordingSys::log() const
{
    QMutexLocker l(&m_mutex);
    return m_log;
}

void RecordingSys::clearLog()
{
    QMutexLocker l(&m_mutex);
    m_log.clear();
}

void RecordingSys::label(int fd, const QString& name)
{
    QMutexLocker l(&m_mutex);
    m_names.insert(fd, name);
}

void RecordingSys::after(const QString& prefix, std::function<void()> f)
{
    QMutexLocker l(&m_mutex);
    m_after.emplace_back(prefix, std::move(f));
}

void RecordingSys::failWith(const QString& prefix, int err)
{
    QMutexLocker l(&m_mutex);
    m_fail.emplace_back(prefix, err);
}

QString RecordingSys::nameOf(int fd) const
{
    QMutexLocker l(&m_mutex);
    return m_names.value(fd, QStringLiteral("fd%1").arg(fd));
}

int RecordingSys::before(const QString& entry)
{
    QMutexLocker l(&m_mutex);
    m_log << entry;
    for (const auto& [prefix, err] : m_fail)
        if (entry.startsWith(prefix)) return err;
    return 0;
}

void RecordingSys::afterCall(const QString& entry)
{
    std::function<void()> f;
    {
        QMutexLocker l(&m_mutex);
        for (auto it = m_after.begin(); it != m_after.end(); ++it) {
            if (entry.startsWith(it->first)) {
                f = std::move(it->second);
                m_after.erase(it);
                break;
            }
        }
    }
    if (f) f();
}

#define ASB_REC(entry, call)              \
    const QString e_ = (entry);           \
    if (const int inj = before(e_)) {     \
        errno = inj;                      \
        return -1;                        \
    }                                     \
    const auto r_ = (call);               \
    const int saved_ = errno;             \
    afterCall(e_);                        \
    errno = saved_;

int RecordingSys::open(const char* path, int flags, mode_t mode)
{
    ASB_REC(QStringLiteral("open %1").arg(QString::fromLocal8Bit(path)), m_real.open(path, flags, mode));
    if (r_ >= 0) label(r_, QString::fromLocal8Bit(path));
    return r_;
}

int RecordingSys::openat(int dirfd, const char* name, int flags, mode_t mode)
{
    ASB_REC(QStringLiteral("openat %1%2").arg(QString::fromLocal8Bit(name), (flags & O_CREAT) ? QStringLiteral(" create") : QString()),
            m_real.openat(dirfd, name, flags, mode));
    if (r_ >= 0) label(r_, QString::fromLocal8Bit(name));
    return r_;
}

int RecordingSys::close(int fd)
{
    {
        QMutexLocker l(&m_mutex);
        m_names.remove(fd);
    }
    return m_real.close(fd);
}

int RecordingSys::fstat(int fd, struct stat* st)
{
    ASB_REC(QStringLiteral("fstat %1").arg(nameOf(fd)), m_real.fstat(fd, st));
    if (r_ == 0 && editFstat) editFstat(*st);
    return r_;
}

int RecordingSys::fstatat(int dirfd, const char* name, struct stat* st, int flags)
{
    ASB_REC(QStringLiteral("fstatat %1").arg(QString::fromLocal8Bit(name)), m_real.fstatat(dirfd, name, st, flags));
    return r_;
}

int RecordingSys::fstatfs(int fd, struct statfs* st)
{
    ASB_REC(QStringLiteral("fstatfs %1").arg(nameOf(fd)), m_real.fstatfs(fd, st));
    if (r_ == 0 && fsType) st->f_type = *fsType;
    return r_;
}

int RecordingSys::fchown(int fd, uid_t uid, gid_t gid)
{
    ASB_REC(QStringLiteral("fchown %1").arg(nameOf(fd)), m_real.fchown(fd, uid, gid));
    return r_;
}

int RecordingSys::fchmod(int fd, mode_t mode)
{
    ASB_REC(QStringLiteral("fchmod %1 %2").arg(nameOf(fd)).arg(uint(mode), 4, 8, QLatin1Char('0')), m_real.fchmod(fd, mode));
    return r_;
}

ssize_t RecordingSys::read(int fd, void* buf, size_t n)
{
    return m_real.read(fd, buf, n);
}

ssize_t RecordingSys::pread(int fd, void* buf, size_t n, off_t off)
{
    return m_real.pread(fd, buf, n, off);
}

ssize_t RecordingSys::write(int fd, const void* buf, size_t n)
{
    ASB_REC(QStringLiteral("write %1").arg(nameOf(fd)), m_real.write(fd, buf, n));
    return r_;
}

int RecordingSys::ftruncate(int fd, off_t len)
{
    ASB_REC(QStringLiteral("ftruncate %1").arg(nameOf(fd)), m_real.ftruncate(fd, len));
    return r_;
}

int RecordingSys::fsync(int fd)
{
    ASB_REC(QStringLiteral("fsync %1").arg(nameOf(fd)), m_real.fsync(fd));
    return r_;
}

int RecordingSys::renameat(int a, const char* an, int b, const char* bn)
{
    ASB_REC(QStringLiteral("renameat %1 -> %2").arg(QString::fromLocal8Bit(an), QString::fromLocal8Bit(bn)),
            m_real.renameat(a, an, b, bn));
    return r_;
}

int RecordingSys::renameNoReplace(int a, const char* an, int b, const char* bn)
{
    ASB_REC(QStringLiteral("renameNoReplace %1 -> %2").arg(QString::fromLocal8Bit(an), QString::fromLocal8Bit(bn)),
            m_real.renameNoReplace(a, an, b, bn));
    return r_;
}

int RecordingSys::unlinkat(int dirfd, const char* name, int flags)
{
    ASB_REC(QStringLiteral("unlinkat %1").arg(QString::fromLocal8Bit(name)), m_real.unlinkat(dirfd, name, flags));
    return r_;
}

int RecordingSys::mkdirat(int dirfd, const char* name, mode_t mode)
{
    ASB_REC(QStringLiteral("mkdirat %1").arg(QString::fromLocal8Bit(name)), m_real.mkdirat(dirfd, name, mode));
    return r_;
}

int RecordingSys::flock(int fd, int op)
{
    ASB_REC(QStringLiteral("flock %1").arg(nameOf(fd)), m_real.flock(fd, op));
    return r_;
}

ssize_t RecordingSys::readlinkat(int dirfd, const char* name, char* buf, size_t n)
{
    return m_real.readlinkat(dirfd, name, buf, n);
}

std::optional<QStringList> RecordingSys::listDirectory(int dirfd)
{
    {
        QMutexLocker l(&m_mutex);
        m_log << QStringLiteral("list %1").arg(m_names.value(dirfd, QStringLiteral("fd%1").arg(dirfd)));
    }
    return m_real.listDirectory(dirfd);
}

#undef ASB_REC

}  // namespace asb::test
