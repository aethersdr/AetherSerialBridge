#pragma once

// apply.lock on Windows (WS-6; S3, INV-66, INV-97, INV-104): %ProgramFiles%\AetherSerialBridge\apply.lock.
//
// acquire(timeout): the lock directory is opened (created by this elevated Applier when absent, owner
// Administrators + protected DACL in the create call) and judged by handle (LockFiles.h). Then apply.lock is
// opened relative to it with GENERIC_READ | GENERIC_WRITE, share mode 0, FILE_OPEN_IF and — when it is
// created — owner Administrators and a protected SYSTEM + Administrators DACL in the same call (INV-97: a
// standard user cannot even open it to obstruct acquisition). An existing file is validated on the handle; a
// wider DACL with a trusted owner is reset through a separate WRITE_DAC handle with the same FILE_ID_INFO and
// reported. Then LockFileEx(EXCLUSIVE). Busy (sharing or lock violation) is retried every 100 ms until the
// timeout. The hold releases on destruction; the kernel releases it when the process dies. Global across
// sessions and accounts: it is a file. Never inheritable (no child processes exist anyway, INV-112).
// Uninstall leaves the file and the directory in place.

#include "core/Interfaces.h"
#include "platform/win/LockFiles.h"

namespace asb::win {

class FileApplyLock final : public IApplyLock {
public:
    static constexpr char kName[] = "apply.lock";

    FileApplyLock(NtSeams& seams, QString lockDirectory, IClock& clock = systemClock(), LockReport report = {});
    std::unique_ptr<LockHold> acquire(std::chrono::seconds timeout) override;
    // Why the last acquire returned nullptr (insecure directory or file, timeout), for the caller's message.
    QString lastError() const { return m_lastError; }
    // The same, except for a timeout (another holder): empty then.
    QString refusalReason() const override { return m_timedOut ? QString() : m_lastError; }

private:
    NtSeams& m_s;
    QString m_dirPath;
    IClock& m_clock;
    LockReport m_report;
    QString m_lastError;
    bool m_timedOut = false;
};

// A held lock file: the share-0 handle, closed on destruction. `native()` is for tests (inheritance flag).
// markForDeletion (WS-16, D69): FileDispositionInfoEx with POSIX semantics on this handle; where Windows answers that it
// is not supported (ERROR_INVALID_PARAMETER, ERROR_NOT_SUPPORTED: before Windows 10 1709, or no POSIX semantics on the
// volume) the classic FileDispositionInfo on the same handle, which suffices because the handle is share-0 and the
// folder is removed only after it closes. clearDeletion undoes whichever form was set.
class WinLockHold final : public LockHold {
public:
    WinLockHold(OwnedHandle h, std::function<void()> onRelease = {}) : m_h(std::move(h)), m_onRelease(std::move(onRelease)) {}
    ~WinLockHold() override
    {
        m_h.reset();
        if (m_onRelease) m_onRelease();
    }
    Handle native() const { return m_h.get(); }
    bool markForDeletion(QString* err) override;
    bool clearDeletion(QString* err) override;
    enum class Marked { None, Posix, Classic };
    Marked marked() const { return m_marked; }

private:
    OwnedHandle m_h;
    std::function<void()> m_onRelease;
    Marked m_marked = Marked::None;
};

}  // namespace asb::win
