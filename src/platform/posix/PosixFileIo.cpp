#include "platform/posix/PosixFileIo.h"

#include <cerrno>

namespace asb::posix {
namespace {

FileId idOf(const struct stat& st)
{
    return FileId::fromPosix(quint64(st.st_dev), quint64(st.st_ino));
}

QByteArray local(const QString& name)
{
    return name.toLocal8Bit();
}

// A single path component: no separator, not empty, not "." or "..".
bool plainName(const QString& name, QString* err)
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..") || name.contains(QLatin1Char('/')) ||
        name.contains(QChar(0))) {
        if (err) *err = QStringLiteral("not a single file name: %1").arg(name);
        return false;
    }
    return true;
}

class PosixTempFile final : public TempFile {
public:
    PosixTempFile(PosixSys& sys, int dirfd, QString name, Fd fd, FileId id)
        : m_sys(sys), m_dirfd(dirfd), m_name(std::move(name)), m_fd(std::move(fd)), m_id(id)
    {
    }
    ~PosixTempFile() override
    {
        if (!m_committed) {
            // Remove the temp only while its name still refers to OUR inode: nothing else is ever deleted.
            struct stat st {};
            const QByteArray n = local(m_name);
            if (m_sys.fstatat(m_dirfd, n.constData(), &st, AT_SYMLINK_NOFOLLOW) == 0 && idOf(st) == m_id &&
                S_ISREG(st.st_mode))
                m_sys.unlinkat(m_dirfd, n.constData(), 0);
        }
    }
    bool write(const QByteArray& bytes, QString* err) override { return writeAll(m_sys, m_fd.get(), bytes, err); }
    bool flush(QString* err) override
    {
        if (m_sys.fsync(m_fd.get()) == 0) return true;
        if (err) *err = QStringLiteral("fsync: %1").arg(errnoText());
        return false;
    }
    QByteArray readBack(qint64 cap, QString* err) override
    {
        QString e;
        const std::optional<QByteArray> b = preadAll(m_sys, m_fd.get(), cap, &e);
        if (!b) {
            if (err) *err = e;
            return {};
        }
        return *b;
    }
    FileId id() const override { return m_id; }
    Commit commitAs(const QString& name) override
    {
        Commit c;
        if (m_committed) {
            c.error = QStringLiteral("already committed");
            return c;
        }
        if (!plainName(name, &c.error)) return c;
        const QByteArray from = local(m_name), to = local(name);
        if (m_sys.renameat(m_dirfd, from.constData(), m_dirfd, to.constData()) != 0) {
            c.error = QStringLiteral("rename %1 -> %2: %3").arg(m_name, name, errnoText());
            return c;
        }
        m_committed = true;
        c.replaced = true;
        if (m_sys.fsync(m_dirfd) != 0) {
            c.error = QStringLiteral("fsync of the directory: %1").arg(errnoText());
            return c;
        }
        c.durable = true;
        return c;
    }

private:
    PosixSys& m_sys;
    int m_dirfd;
    QString m_name;
    Fd m_fd;
    FileId m_id;
    bool m_committed = false;
};

class PosixExistingFile final : public ExistingFile {
public:
    PosixExistingFile(PosixSys& sys, int dirfd, QString name, Fd fd, FileId id)
        : m_sys(sys), m_dirfd(dirfd), m_name(std::move(name)), m_fd(std::move(fd)), m_id(id)
    {
    }
    bool truncate(QString* err) override
    {
        if (m_sys.ftruncate(m_fd.get(), 0) == 0) return true;
        if (err) *err = QStringLiteral("ftruncate: %1").arg(errnoText());
        return false;
    }
    bool writeAll(const QByteArray& bytes, QString* err) override
    {
        // From offset 0 (the fd was opened without O_APPEND and nothing else moved it).
        return posix::writeAll(m_sys, m_fd.get(), bytes, err);
    }
    bool flush(QString* err) override
    {
        if (m_sys.fsync(m_fd.get()) == 0) return true;
        if (err) *err = QStringLiteral("fsync: %1").arg(errnoText());
        return false;
    }
    bool removeByHandle(QString* err) override
    {
        struct stat st {};
        const QByteArray n = local(m_name);
        if (m_sys.fstatat(m_dirfd, n.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (err) *err = errnoText();
            return false;
        }
        if (idOf(st) != m_id) {
            if (err) *err = QStringLiteral("%1 was replaced after it was opened; not removed").arg(m_name);
            return false;
        }
        if (m_sys.unlinkat(m_dirfd, n.constData(), 0) != 0) {
            if (err) *err = errnoText();
            return false;
        }
        return true;
    }

private:
    PosixSys& m_sys;
    int m_dirfd;
    QString m_name;
    Fd m_fd;
    FileId m_id;
};

}  // namespace

PosixDirHandle::PosixDirHandle(PosixSys& sys, int fd)
{
    reset(sys, fd);
}

void PosixDirHandle::reset(PosixSys& sys, int fd)
{
    openError.clear();
    m_fd = Fd(sys, fd);
    m_id = {};
    struct stat st {};
    if (fd >= 0 && sys.fstat(fd, &st) == 0) m_id = idOf(st);
}

int dirFd(const DirHandle& d)
{
    const auto* p = dynamic_cast<const PosixDirHandle*>(&d);
    return p ? p->fd() : -1;
}

QString noDirectoryError(const DirHandle& d)
{
    const auto* p = dynamic_cast<const PosixDirHandle*>(&d);
    if (!p) return QStringLiteral("not a POSIX directory handle");
    return p->openError.isEmpty() ? QStringLiteral("the directory is not open") : p->openError;
}

PosixFileIo::PosixFileIo(PosixSys& sys, PosixOwner configOwner, mode_t mode)
    : m_sys(sys), m_owner(configOwner), m_mode(mode)
{
}

std::unique_ptr<TempFile> PosixFileIo::createTemp(const DirHandle& dir, OwnerPolicy policy, const NameGen& gen, QString* err)
{
    const int dfd = dirFd(dir);
    if (dfd < 0) {
        if (err) *err = noDirectoryError(dir);
        return nullptr;
    }
    const QString name = gen ? gen() : QString();
    if (!plainName(name, err)) return nullptr;
    const QByteArray n = local(name);
    Fd fd(m_sys, m_sys.openat(dfd, n.constData(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!fd) {
        if (err) *err = QStringLiteral("create %1: %2").arg(name, errnoText());
        return nullptr;
    }
    struct stat st {};
    if (m_sys.fstat(fd.get(), &st) != 0) {
        const QString e = errnoText();
        m_sys.unlinkat(dfd, n.constData(), 0);   // just created exclusively: it is ours
        if (err) *err = QStringLiteral("fstat %1: %2").arg(name, e);
        return nullptr;
    }
    const FileId id = idOf(st);
    // The owner is set on the private fd before anything can rename it into view (S9, INV-75); the Runtime
    // policy never changes ownership (INV-96).
    if (policy == OwnerPolicy::Config && m_sys.fchown(fd.get(), m_owner.uid, m_owner.gid) != 0) {
        const QString e = errnoText();
        m_sys.unlinkat(dfd, n.constData(), 0);
        if (err) *err = QStringLiteral("fchown %1: %2").arg(name, e);
        return nullptr;
    }
    if (m_sys.fchmod(fd.get(), m_mode) != 0) {
        const QString e = errnoText();
        m_sys.unlinkat(dfd, n.constData(), 0);
        if (err) *err = QStringLiteral("fchmod %1: %2").arg(name, e);
        return nullptr;
    }
    return std::make_unique<PosixTempFile>(m_sys, dfd, name, std::move(fd), id);
}

bool PosixFileIo::remove(const DirHandle& dir, const QString& name, QString* err)
{
    const int dfd = dirFd(dir);
    if (dfd < 0) {
        if (err) *err = noDirectoryError(dir);
        return false;
    }
    if (!plainName(name, err)) return false;
    const QByteArray n = local(name);
    struct stat st {};
    if (m_sys.fstatat(dfd, n.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (err) *err = errno == ENOENT ? fileNotFoundError() : errnoText();
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        if (err) *err = QStringLiteral("%1 is not a regular file; not removed").arg(name);
        return false;
    }
    if (m_sys.unlinkat(dfd, n.constData(), 0) != 0) {
        if (err) *err = errnoText();
        return false;
    }
    return true;
}

bool PosixFileIo::syncDir(const DirHandle& dir, QString* err)
{
    const int dfd = dirFd(dir);
    if (dfd < 0) {
        if (err) *err = noDirectoryError(dir);
        return false;
    }
    if (m_sys.fsync(dfd) == 0) return true;
    if (err) *err = QStringLiteral("fsync of the directory: %1").arg(errnoText());
    return false;
}

std::unique_ptr<ExistingFile> PosixFileIo::openExisting(const DirHandle& dir, const QString& name, QString* err)
{
    const int dfd = dirFd(dir);
    if (dfd < 0) {
        if (err) *err = noDirectoryError(dir);
        return nullptr;
    }
    if (!plainName(name, err)) return nullptr;
    const QByteArray n = local(name);
    Fd fd(m_sys, m_sys.openat(dfd, n.constData(), O_WRONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (!fd) {
        if (err) *err = errno == ENOENT ? fileNotFoundError() : QStringLiteral("open %1: %2").arg(name, errnoText());
        return nullptr;
    }
    struct stat st {};
    if (m_sys.fstat(fd.get(), &st) != 0) {
        if (err) *err = QStringLiteral("fstat %1: %2").arg(name, errnoText());
        return nullptr;
    }
    if (!S_ISREG(st.st_mode)) {
        if (err) *err = QStringLiteral("%1 is not a regular file").arg(name);
        return nullptr;
    }
    return std::make_unique<PosixExistingFile>(m_sys, dfd, name, std::move(fd), idOf(st));
}

}  // namespace asb::posix
