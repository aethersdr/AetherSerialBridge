#pragma once

// engine.lock and engine.lock.id on Windows (WS-6; S15, D58, INV-103, INV-104, INV-116), in the lock directory
// %ProgramFiles%\AetherSerialBridge beside apply.lock (LockFiles.h).
//
//  tryAcquire / acquireUntil   engine.lock is opened (existing file only) relative to the judged directory
//      with GENERIC_READ and share mode 0: "held" = the open fails with a sharing violation; release = the
//      handle closes (process exit). Validated on the handle: Role::Service refuses an insecure file
//      (Insecure: the runner exits 4); Role::Applier repairs a widened DACL through a WRITE_DAC handle with the
//      same FILE_ID_INFO and reports it. acquireUntil polls every 100 ms.
//  readHeldLock   the content through the held handle (D58).
//  peekLock   never opens the content: an attribute-only open (FILE_READ_ATTRIBUTES, which share modes do not
//      govern, so it neither waits for nor blocks a holder or a replace) says whether the file exists:
//      Missing, or Unreadable (B3 decides) — WS-5's "Unreadable on Windows".
//  readRecord   engine.lock.id, opened with share read/write/delete (a reader never blocks its replacement),
//      validated like a lock file, at most 64 bytes.
//  io() / dir() / children()   the lock directory for LockIdentity::publish: WinFileIo with the lock files'
//      security (owner Administrators, protected SYSTEM + Administrators DACL) in the create call.
//
// The lock directory's handle is opened and judged PER ACQUISITION (v1.1 INV-128, D69): a hold keeps it and its
// release closes it; peekLock, readRecord, children and filesystemSupported open it for the call and close it again;
// dir() opens it for the caller and closeIdle() (after every operation) closes it. A long-lived process (the window)
// therefore holds nothing in %ProgramFiles%\AetherSerialBridge between operations, so an uninstall can remove the
// folder. v1.0 kept it for the object's life.
//
// Replacing engine.lock requires that NO handle holds it: a share-0 handle makes the rename fail with a
// sharing violation (probed on windows-latest). The Applier therefore releases its hold before publishing a
// new lock and acquires the new one afterwards (WS-5 fold); every other open here is attribute-only or
// shares delete, so nothing in this adapter can block that replace.

#include "core/Interfaces.h"
#include "platform/win/FileApplyLock.h"
#include "platform/win/LockFiles.h"

namespace asb::win {

class EngineLock final : public IEngineLock {
public:
    static constexpr char kLock[] = "engine.lock";
    static constexpr char kRecord[] = "engine.lock.id";

    EngineLock(NtSeams& seams, QString lockDirectory, LockRole role, IClock& clock = systemClock(), LockReport report = {});
    ~EngineLock() override;

    std::unique_ptr<LockHold> tryAcquire(LockResult* why) override;
    std::unique_ptr<LockHold> acquireUntil(Deadline, LockResult* why) override;
    LockFileRead readHeldLock() override;
    LockFileRead peekLock() override;
    LockFileRead readRecord() override;
    bool filesystemSupported(QString* why) override;
    FileIo& io() override { return m_io; }
    const DirHandle& dir() override;
    QStringList children() override;
    void closeIdle() override;

    QString lastError() const { return m_lastError; }
    // Test builds only (tst_GuiNative's handle-probe control): keep the directory handle for the object's life, as
    // v1.0 did.
    void setKeepDirectory(bool on) { m_keepDirectory = on; }

private:
    bool openDirectory(LockResult* why);
    void closeDirectory();   // unless a hold is live (or the v1.0 control keeps it)

    NtSeams& m_s;
    LockRole m_role;
    IClock& m_clock;
    LockDirectory m_dir;
    WinFileIo m_io;
    Handle m_held = 0;   // the handle of the live hold, for readHeldLock
    std::shared_ptr<bool> m_alive;
    QString m_lastError;
    bool m_keepDirectory = false;
};

}  // namespace asb::win
