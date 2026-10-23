#include "platform/win/FileApplyLock.h"

namespace asb::win {

FileApplyLock::FileApplyLock(NtSeams& seams, QString lockDirectory, IClock& clock, LockReport report)
    : m_s(seams), m_dirPath(std::move(lockDirectory)), m_clock(clock), m_report(std::move(report))
{
}

std::unique_ptr<LockHold> FileApplyLock::acquire(std::chrono::seconds timeout)
{
    m_lastError.clear();
    m_timedOut = false;
    const Deadline deadline = m_clock.now() + timeout;
    LockDirectory dir(m_s, m_dirPath, LockRole::Applier, m_report);
    QString why;
    if (dir.open(/*create*/ true, &why) != LockResult::Acquired) {
        m_lastError = why;
        return nullptr;
    }
    for (;;) {
        OwnedHandle h;
        // DELETE: uninstall marks the held file for deletion on close (WS-16, D69).
        const LockResult r = dir.openLockFile(QString::fromLatin1(kName), nt::kGenericRead | nt::kGenericWrite | nt::kDelete,
                                              nt::kFileOpenIf, &h, &why);
        if (r == LockResult::Acquired) {
            const Status st = m_s.lockExclusive(h.get());
            if (st.ok()) return std::make_unique<WinLockHold>(std::move(h));
            if (st.kind != Status::Kind::LockViolation) {
                m_lastError = QStringLiteral("could not lock %1: %2").arg(QLatin1String(kName), st.describe());
                return nullptr;
            }
            why = QStringLiteral("%1 is locked").arg(QLatin1String(kName));
        } else if (r != LockResult::Busy) {
            m_lastError = why;
            return nullptr;
        }
        if (m_clock.now() >= deadline) {
            m_lastError = QStringLiteral("%1 was not free within %2 s").arg(QLatin1String(kName)).arg(timeout.count());
            m_timedOut = true;
            return nullptr;
        }
        m_clock.sleepFor(std::chrono::milliseconds(100));
    }
}

bool WinLockHold::markForDeletion(QString* err)
{
    NtSeams* s = m_h.seams();
    if (!s || !m_h) {
        if (err) *err = QStringLiteral("the lock is not held");
        return false;
    }
    const Status posix = s->setDeleteOnClose(m_h.get(), true, true);
    if (posix.ok()) {
        m_marked = Marked::Posix;
        return true;
    }
    constexpr quint32 kInvalidParameter = 87, kNotSupported = 50;
    if (posix.code != kInvalidParameter && posix.code != kNotSupported) {
        if (err) *err = posix.describe();
        return false;
    }
    const Status classic = s->setDeleteOnClose(m_h.get(), true, false);
    if (classic.ok()) {
        m_marked = Marked::Classic;
        return true;
    }
    if (err) *err = QStringLiteral("%1; then the classic form: %2").arg(posix.describe(), classic.describe());
    return false;
}

bool WinLockHold::clearDeletion(QString* err)
{
    if (m_marked == Marked::None) return true;
    NtSeams* s = m_h.seams();
    const Status st = s ? s->setDeleteOnClose(m_h.get(), false, m_marked == Marked::Posix)
                        : Status::fail(Status::Kind::Other, 6, QStringLiteral("the lock is not held"));
    if (!st.ok()) {
        if (err) *err = st.describe();
        return false;
    }
    m_marked = Marked::None;
    return true;
}

}  // namespace asb::win
