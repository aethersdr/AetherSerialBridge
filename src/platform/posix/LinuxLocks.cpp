#include "platform/posix/LinuxLocks.h"

#include "core/FilesystemPolicy.h"
#include "core/LockIdentity.h"

#include <QList>

#include <sys/file.h>
#include <sys/sysmacros.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <thread>

namespace asb::posix {
namespace {

class PosixLockHold final : public LockHold {
public:
    PosixLockHold(Fd fd, std::weak_ptr<int> slot) : m_fd(std::move(fd)), m_slot(std::move(slot)) {}
    ~PosixLockHold() override
    {
        if (auto p = m_slot.lock())
            if (*p == m_fd.get()) *p = -1;
        // Closing the only fd of this open file description releases the flock (nothing was dup'ed or
        // inherited: no child processes exist).
    }

private:
    Fd m_fd;
    std::weak_ptr<int> m_slot;
};

class ApplyHold final : public LockHold {
public:
    explicit ApplyHold(Fd fd) : m_fd(std::move(fd)) {}

private:
    Fd m_fd;
};

QString parentOf(const QString& path)
{
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    return slash <= 0 ? QStringLiteral("/") : path.left(slash);
}

QString nameOf(const QString& path)
{
    return path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
}

QString modeText(mode_t m)
{
    return QStringLiteral("%1").arg(uint(m & 07777), 4, 8, QLatin1Char('0'));
}

}  // namespace

std::optional<qint64> procLocksHolder(const QByteArray& procLocks, quint64 dev, quint64 ino)
{
    const QByteArray want = QByteArray::number(qulonglong(major(dev)), 16).rightJustified(2, '0') + ':' +
                            QByteArray::number(qulonglong(minor(dev)), 16).rightJustified(2, '0') + ':' +
                            QByteArray::number(qulonglong(ino));
    for (const QByteArray& line : procLocks.split('\n')) {
        const QList<QByteArray> f = line.simplified().split(' ');
        // "<n>: FLOCK  ADVISORY  WRITE <pid> <maj>:<min>:<ino> <start> <end>"; a "->" line is a waiter.
        if (f.size() < 6 || f.contains("->")) continue;
        if (f.value(1) != "FLOCK") continue;
        if (f.value(5) != want) continue;
        bool ok = false;
        const qint64 pid = f.value(4).toLongLong(&ok);
        if (ok && pid > 0) return pid;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------------
// EngineLock
// ---------------------------------------------------------------------------------------------------

EngineLock::EngineLock(Options o, IClock& clock, PosixSys& sys)
    : m_opt(std::move(o)), m_clock(clock), m_sys(sys), m_io(sys, m_opt.owner, 0644), m_alive(std::make_shared<int>(-1))
{
}

EngineLock::~EngineLock() = default;

int EngineLock::openDirectory(LockResult* why, bool judge)
{
    auto fail = [&](LockResult r, const QString& e) {
        if (why) *why = r;
        m_lastError = e;
        return -1;
    };
    const QByteArray parent = parentOf(m_opt.directory).toLocal8Bit();
    Fd p(m_sys, m_sys.open(parent.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!p) return fail(LockResult::Insecure, QStringLiteral("%1 cannot be opened: %2").arg(QString::fromLocal8Bit(parent), errnoText()));
    const QByteArray n = nameOf(m_opt.directory).toLocal8Bit();
    Fd d(m_sys, m_sys.openat(p.get(), n.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!d) {
        const int e = errno;
        if (e == ENOENT) return fail(LockResult::Missing, QStringLiteral("%1 does not exist").arg(m_opt.directory));
        return fail(LockResult::Insecure, QStringLiteral("%1 is not a plain directory: %2").arg(m_opt.directory, errnoText(e)));
    }
    if (!judge) return d.release();
    struct stat st {};
    if (m_sys.fstat(d.get(), &st) != 0)
        return fail(LockResult::Insecure, QStringLiteral("%1 cannot be examined: %2").arg(m_opt.directory, errnoText()));
    if (!S_ISDIR(st.st_mode) || st.st_uid != m_opt.owner.uid || st.st_gid != m_opt.owner.gid || (st.st_mode & 07777) != 0755)
        return fail(LockResult::Insecure, QStringLiteral("%1 must be a directory owned by %2:%3 with mode 0755 (it is %4:%5 %6)")
                                              .arg(m_opt.directory)
                                              .arg(m_opt.owner.uid)
                                              .arg(m_opt.owner.gid)
                                              .arg(st.st_uid)
                                              .arg(st.st_gid)
                                              .arg(modeText(st.st_mode)));
    struct statfs fs {};
    if (m_sys.fstatfs(d.get(), &fs) != 0)
        return fail(LockResult::Unsupported, QStringLiteral("the file system of %1 cannot be determined: %2").arg(m_opt.directory, errnoText()));
    const FilesystemPolicy::Kind kind = FilesystemPolicy::fromLinuxMagic(quint64(fs.f_type));
    if (!FilesystemPolicy::lockDirectorySupported(kind))
        return fail(LockResult::Unsupported, QStringLiteral("%1 is on %2; the engine lock needs ext4, XFS, Btrfs or F2FS")
                                                 .arg(m_opt.directory, FilesystemPolicy::name(kind)));
    return d.release();
}

std::unique_ptr<LockHold> EngineLock::tryAcquire(LockResult* why)
{
    m_holder.clear();
    LockResult r = LockResult::Busy;
    Fd dir(m_sys, openDirectory(&r));
    if (!dir) {
        if (why) *why = r;
        return nullptr;
    }
    auto fail = [&](LockResult res, const QString& e) -> std::unique_ptr<LockHold> {
        if (why) *why = res;
        m_lastError = e;
        return nullptr;
    };
    // By name, relative to the judged directory, on every call: a replaced engine.lock is the one locked.
    Fd fd(m_sys, m_sys.openat(dir.get(), LockIdentity::kLockName, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!fd) {
        const int e = errno;
        if (e == ENOENT) return fail(LockResult::Missing, QStringLiteral("engine.lock does not exist"));
        if (e == ELOOP) return fail(LockResult::InsecureFile, QStringLiteral("engine.lock is a symbolic link"));
        return fail(LockResult::Insecure, QStringLiteral("engine.lock cannot be opened: %1").arg(errnoText(e)));
    }
    // The file is judged on the fd that is locked (INV-110): type, owner and mode, exactly.
    struct stat st {};
    if (m_sys.fstat(fd.get(), &st) != 0)
        return fail(LockResult::Insecure, QStringLiteral("engine.lock cannot be examined: %1").arg(errnoText()));
    if (!S_ISREG(st.st_mode) || st.st_uid != m_opt.owner.uid || st.st_gid != m_opt.owner.gid || (st.st_mode & 07777) != 0644)
        return fail(LockResult::InsecureFile, QStringLiteral("engine.lock must be a regular file owned by %1:%2 with mode 0644 "
                                                             "(it is %3:%4 %5)")
                                                  .arg(m_opt.owner.uid)
                                                  .arg(m_opt.owner.gid)
                                                  .arg(st.st_uid)
                                                  .arg(st.st_gid)
                                                  .arg(modeText(st.st_mode)));
    if (m_sys.flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
        const int e = errno;
        if (e == EWOULDBLOCK || e == EINTR) {
            nameHolder(fd.get());
            return fail(LockResult::Busy, m_holder.isEmpty() ? QStringLiteral("engine.lock is held by another process")
                                                             : QStringLiteral("engine.lock is held by %1").arg(m_holder));
        }
        return fail(LockResult::Insecure, QStringLiteral("flock(engine.lock): %1").arg(errnoText(e)));
    }
    if (why) *why = LockResult::Acquired;
    m_lastError.clear();
    *m_alive = fd.get();
    return std::make_unique<PosixLockHold>(std::move(fd), m_alive);
}

void EngineLock::nameHolder(int fd)
{
    struct stat st {};
    if (m_sys.fstat(fd, &st) != 0) return;
    const QByteArray path = m_opt.procLocks.toLocal8Bit();
    Fd pl(m_sys, m_sys.open(path.constData(), O_RDONLY | O_CLOEXEC));
    if (!pl) return;
    QByteArray text;
    char buf[8192];
    for (;;) {
        const ssize_t n = m_sys.read(pl.get(), buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        text.append(buf, int(n));
        if (text.size() > 4 * 1024 * 1024) break;
    }
    const std::optional<qint64> pid = procLocksHolder(text, quint64(st.st_dev), quint64(st.st_ino));
    if (!pid) return;
    QString comm;
    const QByteArray commPath = QStringLiteral("%1/%2/comm").arg(m_opt.procRoot).arg(*pid).toLocal8Bit();
    Fd c(m_sys, m_sys.open(commPath.constData(), O_RDONLY | O_CLOEXEC));
    if (c) {
        char cb[64];
        const ssize_t n = m_sys.read(c.get(), cb, sizeof cb);
        if (n > 0) comm = QString::fromLocal8Bit(cb, int(n)).trimmed();
    }
    m_holder = comm.isEmpty() ? QStringLiteral("pid %1").arg(*pid) : QStringLiteral("pid %1 (%2)").arg(*pid).arg(comm);
}

std::unique_ptr<LockHold> EngineLock::acquireUntil(Deadline deadline, LockResult* why)
{
    for (;;) {
        LockResult r = LockResult::Busy;
        std::unique_ptr<LockHold> h = tryAcquire(&r);
        const Deadline now = m_clock.now();
        if (h || r != LockResult::Busy || now >= deadline) {
            if (why) *why = h ? LockResult::Acquired : r;
            return h;
        }
        // Never sleep past the deadline (WS-5 round 3: the waits get their phase's remaining budget; a wait
        // that overslept would return after the phase deadline and be judged late).
        m_clock.sleepFor(std::min(kPoll, std::chrono::ceil<std::chrono::milliseconds>(deadline - now)));
    }
}

LockFileRead EngineLock::readHeldLock()
{
    LockFileRead out;
    if (*m_alive < 0) {
        out.status = LockFileRead::Status::Failed;
        out.error = QStringLiteral("engine.lock is not held");
        return out;
    }
    QString err;
    std::optional<QByteArray> b = preadAll(m_sys, *m_alive, LockIdentity::kMaxBytes, &err);
    if (!b) {
        out.status = LockFileRead::Status::Failed;
        out.error = err;
        return out;
    }
    out.status = LockFileRead::Status::Ok;
    out.bytes = *b;
    return out;
}

LockFileRead EngineLock::readChecked(const char* name, qint64 cap)
{
    LockFileRead out;
    out.status = LockFileRead::Status::Failed;
    // Read without judging the directory: the prologue only classifies the pair; a directory that fails its
    // checks is refused by every acquisition (B3), never replaced (LockResult::Insecure).
    LockResult r = LockResult::Busy;
    Fd dir(m_sys, openDirectory(&r, false));
    if (!dir) {
        if (r == LockResult::Missing) out.status = LockFileRead::Status::Missing;
        out.error = m_lastError;
        return out;
    }
    Fd fd(m_sys, m_sys.openat(dir.get(), name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!fd) {
        if (errno == ENOENT) out.status = LockFileRead::Status::Missing;
        out.error = QStringLiteral("%1: %2").arg(QString::fromLatin1(name), errnoText());
        return out;
    }
    struct stat st {};
    if (m_sys.fstat(fd.get(), &st) != 0) {
        out.error = QStringLiteral("%1: %2").arg(QString::fromLatin1(name), errnoText());
        return out;
    }
    // Both lock files, exactly root:root 0644 (INV-110): the record too, because the DynamicUser service must
    // read it. A record root can read but the service cannot (0600, 0640) is not a matching pair: install and
    // Repair replace it.
    const bool modeOk = (st.st_mode & 07777) == 0644 && st.st_gid == m_opt.owner.gid;
    if (!S_ISREG(st.st_mode) || st.st_uid != m_opt.owner.uid || !modeOk) {
        out.error = QStringLiteral("%1 has the wrong type, owner or mode (%2:%3 %4)")
                        .arg(QString::fromLatin1(name))
                        .arg(st.st_uid)
                        .arg(st.st_gid)
                        .arg(modeText(st.st_mode));
        return out;
    }
    QString err;
    std::optional<QByteArray> b = preadAll(m_sys, fd.get(), cap, &err);
    if (!b) {
        out.error = QStringLiteral("%1: %2").arg(QString::fromLatin1(name), err);
        return out;
    }
    out.status = LockFileRead::Status::Ok;
    out.bytes = *b;
    return out;
}

LockFileRead EngineLock::peekLock()
{
    return readChecked(LockIdentity::kLockName, LockIdentity::kMaxBytes);
}

LockFileRead EngineLock::readRecord()
{
    return readChecked(LockIdentity::kRecordName, LockIdentity::kMaxBytes);
}

bool EngineLock::filesystemSupported(QString* why)
{
    // The directory if it exists, else where install would create it.
    QString target = m_opt.directory;
    const QByteArray dirPath = target.toLocal8Bit();
    Fd fd(m_sys, m_sys.open(dirPath.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!fd && errno == ENOENT) {
        target = parentOf(m_opt.directory);
        const QByteArray p = target.toLocal8Bit();
        fd = Fd(m_sys, m_sys.open(p.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    }
    if (!fd) {
        if (why) *why = QStringLiteral("%1 cannot be opened: %2").arg(target, errnoText());
        return false;
    }
    struct statfs fs {};
    if (m_sys.fstatfs(fd.get(), &fs) != 0) {
        if (why) *why = QStringLiteral("the file system of %1 cannot be determined: %2").arg(target, errnoText());
        return false;
    }
    const FilesystemPolicy::Kind kind = FilesystemPolicy::fromLinuxMagic(quint64(fs.f_type));
    if (FilesystemPolicy::lockDirectorySupported(kind)) return true;
    if (why)
        *why = QStringLiteral("%1 is on %2; the engine lock needs ext4, XFS, Btrfs or F2FS").arg(target, FilesystemPolicy::name(kind));
    return false;
}

const DirHandle& EngineLock::dir()
{
    LockResult r = LockResult::Busy;
    const int fd = openDirectory(&r);
    m_dirHandle.reset(m_sys, fd);
    if (fd < 0) m_dirHandle.openError = m_lastError;
    return m_dirHandle;
}

QStringList EngineLock::children()
{
    LockResult r = LockResult::Busy;
    Fd fd(m_sys, openDirectory(&r));
    if (!fd) return {};
    return m_sys.listDirectory(fd.get()).value_or(QStringList{});
}

// ---------------------------------------------------------------------------------------------------
// FileApplyLock
// ---------------------------------------------------------------------------------------------------

FileApplyLock::FileApplyLock(Options o, PosixSys& sys) : m_opt(std::move(o)), m_sys(sys) {}

std::unique_ptr<LockHold> FileApplyLock::acquire(std::chrono::seconds timeout)
{
    m_refusal.clear();
    auto refuse = [&](const QString& why) -> std::unique_ptr<LockHold> {
        m_refusal = why;
        return nullptr;
    };
    const QByteArray d = m_opt.directory.toLocal8Bit();
    Fd dir(m_sys, m_sys.open(d.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!dir) return refuse(QStringLiteral("%1 cannot be opened: %2").arg(m_opt.directory, errnoText()));
    struct stat ds {};
    if (m_sys.fstat(dir.get(), &ds) != 0) return refuse(QStringLiteral("%1 cannot be examined: %2").arg(m_opt.directory, errnoText()));
    if (ds.st_uid != m_opt.owner.uid || (ds.st_mode & (S_IWGRP | S_IWOTH)))
        return refuse(QStringLiteral("%1 is not a root-only directory (%2:%3 %4)")
                          .arg(m_opt.directory)
                          .arg(ds.st_uid)
                          .arg(ds.st_gid)
                          .arg(modeText(ds.st_mode)));
    if (m_opt.requireTmpfs) {
        struct statfs fs {};
        if (m_sys.fstatfs(dir.get(), &fs) != 0)
            return refuse(QStringLiteral("the file system of %1 cannot be determined: %2").arg(m_opt.directory, errnoText()));
        const FilesystemPolicy::Kind kind = FilesystemPolicy::fromLinuxMagic(quint64(fs.f_type));
        if (!FilesystemPolicy::runDirectorySupported(kind))
            return refuse(QStringLiteral("%1 is on %2, not tmpfs").arg(m_opt.directory, FilesystemPolicy::name(kind)));
    }
    const QByteArray n = m_opt.name.toLocal8Bit();
    Fd fd(m_sys, m_sys.openat(dir.get(), n.constData(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600));
    if (!fd) return refuse(QStringLiteral("%1/%2 cannot be opened: %3").arg(m_opt.directory, m_opt.name, errnoText()));
    struct stat st {};
    if (m_sys.fstat(fd.get(), &st) != 0)
        return refuse(QStringLiteral("%1/%2 cannot be examined: %3").arg(m_opt.directory, m_opt.name, errnoText()));
    if (!S_ISREG(st.st_mode) || st.st_uid != m_opt.owner.uid)
        return refuse(QStringLiteral("%1/%2 is not a regular file owned by uid %3").arg(m_opt.directory, m_opt.name).arg(m_opt.owner.uid));
    if ((st.st_mode & 07777) != 0600) {
        // Root-owned in a root-only directory: only root changed the mode; restore it on this fd (INV-97 twin).
        if (m_sys.fchmod(fd.get(), 0600) != 0)
            return refuse(QStringLiteral("%1/%2 has mode %3 and it cannot be reset: %4")
                              .arg(m_opt.directory, m_opt.name, modeText(st.st_mode), errnoText()));
        if (m_opt.report)
            m_opt.report(QStringLiteral("%1/%2: mode %3 -> 0600").arg(m_opt.directory, m_opt.name, modeText(st.st_mode)));
    }
    const auto end = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (m_sys.flock(fd.get(), LOCK_EX | LOCK_NB) == 0) return std::make_unique<ApplyHold>(std::move(fd));
        const int e = errno;
        if (e != EWOULDBLOCK && e != EINTR) return refuse(QStringLiteral("flock(%1): %2").arg(m_opt.name, errnoText(e)));
        if (std::chrono::steady_clock::now() >= end) return nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

}  // namespace asb::posix
