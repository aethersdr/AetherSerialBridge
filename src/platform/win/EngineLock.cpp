#include "platform/win/EngineLock.h"

#include "core/LockIdentity.h"
#include "platform/win/SecurityDescriptor.h"

namespace asb::win {

EngineLock::EngineLock(NtSeams& seams, QString lockDirectory, LockRole role, IClock& clock, LockReport report)
    : m_s(seams), m_role(role), m_clock(clock), m_dir(seams, std::move(lockDirectory), role, std::move(report)), m_io(seams, CreateSecurity::lockDirectory()),
      m_alive(std::make_shared<bool>(true))
{
}

EngineLock::~EngineLock()
{
    *m_alive = false;
}

bool EngineLock::openDirectory(LockResult* why)
{
    if (m_dir.dir().valid()) return true;
    const LockResult r = m_dir.open(/*create*/ false, &m_lastError);
    if (why) *why = r;
    return r == LockResult::Acquired;
}

void EngineLock::closeDirectory()
{
    if (m_held || m_keepDirectory) return;
    m_dir.close();
}

void EngineLock::closeIdle()
{
    closeDirectory();
}

std::unique_ptr<LockHold> EngineLock::tryAcquire(LockResult* why)
{
    m_lastError.clear();
    LockResult r = LockResult::Acquired;
    // The directory is opened and judged per acquisition: the hold keeps it, its release closes it (INV-128).
    if (!openDirectory(&r)) {
        if (why) *why = r;
        closeDirectory();
        return nullptr;
    }
    OwnedHandle h;
    // The Applier's hold also has DELETE: uninstall marks it for deletion on close (WS-16, D69). The service's does not.
    const quint32 access = m_role == LockRole::Applier ? nt::kGenericRead | nt::kDelete : nt::kGenericRead;
    r = m_dir.openLockFile(QString::fromLatin1(kLock), access, nt::kFileOpen, &h, &m_lastError);
    if (why) *why = r;
    if (r != LockResult::Acquired) {
        closeDirectory();
        return nullptr;
    }
    m_held = h.get();
    std::weak_ptr<bool> alive = m_alive;
    Handle mine = m_held;
    return std::make_unique<WinLockHold>(std::move(h), [this, alive, mine] {
        if (auto a = alive.lock(); a && *a && m_held == mine) {
            m_held = 0;
            closeDirectory();
        }
    });
}

std::unique_ptr<LockHold> EngineLock::acquireUntil(Deadline deadline, LockResult* why)
{
    for (;;) {
        LockResult r = LockResult::Busy;
        std::unique_ptr<LockHold> hold = tryAcquire(&r);
        if (hold || r != LockResult::Busy || m_clock.now() >= deadline) {
            if (why) *why = r;
            return hold;
        }
        m_clock.sleepFor(std::chrono::milliseconds(100));
    }
}

LockFileRead EngineLock::readHeldLock()
{
    LockFileRead out;
    if (!m_held) {
        out.error = QStringLiteral("engine.lock is not held");
        return out;
    }
    const Status st = m_s.read(m_held, 0, LockIdentity::kMaxBytes + 1, &out.bytes);
    if (!st.ok()) {
        out.error = st.describe();
        out.bytes.clear();
        return out;
    }
    out.status = LockFileRead::Status::Ok;
    return out;
}

LockFileRead EngineLock::peekLock()
{
    LockFileRead out;
    LockResult r = LockResult::Acquired;
    if (!openDirectory(&r)) {
        if (r == LockResult::Missing) out.status = LockFileRead::Status::Missing;
        out.error = m_lastError;
        closeDirectory();
        return out;
    }
    struct Close {
        EngineLock* self;
        ~Close() { self->closeDirectory(); }
    } close{this};
    Handle h = 0;
    // Attribute-only: share modes do not apply, so this never meets a holder (or blocks a replace).
    const Status st = m_s.openRelative(m_dir.dir().native(), QString::fromLatin1(kLock), nt::kReadAttributes, nt::kShareAll,
                                       nt::kFileOpen, nt::kNonDirectoryFile | nt::kOpenReparsePoint, {}, &h);
    if (st.kind == Status::Kind::NotFound) {
        out.status = LockFileRead::Status::Missing;
        return out;
    }
    if (!st.ok()) {
        out.error = st.describe();
        return out;
    }
    m_s.close(h);
    out.status = LockFileRead::Status::Unreadable;   // its content can only be read while holding it (B3)
    return out;
}

LockFileRead EngineLock::readRecord()
{
    LockFileRead out;
    LockResult r = LockResult::Acquired;
    if (!openDirectory(&r)) {
        if (r == LockResult::Missing) out.status = LockFileRead::Status::Missing;
        out.error = m_lastError;
        closeDirectory();
        return out;
    }
    struct Close {
        EngineLock* self;
        ~Close() { self->closeDirectory(); }
    } close{this};
    Handle h = 0;
    Status st = m_s.openRelative(m_dir.dir().native(), QString::fromLatin1(kRecord),
                                 nt::kReadData | nt::kReadControl | nt::kReadAttributes, nt::kShareAll, nt::kFileOpen,
                                 nt::kNonDirectoryFile | nt::kOpenReparsePoint, {}, &h);
    if (st.kind == Status::Kind::NotFound) {
        out.status = LockFileRead::Status::Missing;
        return out;
    }
    if (!st.ok()) {
        out.error = st.describe();
        return out;
    }
    OwnedHandle owned(m_s, h);
    Facts f;
    if (st = m_s.facts(h, &f); !st.ok() || f.isReparse() || f.isDirectory()) {
        out.error = QStringLiteral("engine.lock.id is not a regular file");
        return out;
    }
    QByteArray sec;
    const std::optional<sd::Descriptor> d = m_s.getSecurity(h, nt::kOwnerInfo | nt::kDaclInfo, &sec).ok() ? sd::parse(sec) : std::nullopt;
    QString why;
    if (!d || !sd::isLockFileSecure(*d, &why)) {
        out.error = QStringLiteral("engine.lock.id is not secured: %1").arg(d ? why : QStringLiteral("unreadable permissions"));
        return out;
    }
    if (st = m_s.read(h, 0, LockIdentity::kMaxBytes + 1, &out.bytes); !st.ok()) {
        out.error = st.describe();
        out.bytes.clear();
        return out;
    }
    out.status = LockFileRead::Status::Ok;
    return out;
}

bool EngineLock::filesystemSupported(QString* why)
{
    const bool ok = m_dir.volumeSupported(why);
    closeDirectory();
    return ok;
}

const DirHandle& EngineLock::dir()
{
    LockResult r = LockResult::Acquired;
    openDirectory(&r);
    return m_dir.dir();
}

QStringList EngineLock::children()
{
    LockResult r = LockResult::Acquired;
    QStringList names;
    if (openDirectory(&r)) m_s.list(m_dir.dir().native(), &names);
    closeDirectory();
    return names;
}

}  // namespace asb::win
