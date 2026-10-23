#pragma once

// The Linux locks (WS-8; S3, S15, D54, D58; INV-66, INV-97, INV-103, INV-110, INV-116).
//
// engine.lock in /var/lib/aether-serial-bridge (EngineLock):
//  - the directory is opened O_DIRECTORY | O_NOFOLLOW relative to its parent, and its fd must show exactly
//    root:root 0755 on a supported file system (ext*, XFS, Btrfs, F2FS; INV-116); otherwise Insecure (or
//    Unsupported). Never created here: install creates it (SystemdService::installOrConfigure).
//  - engine.lock is opened RELATIVE to that fd on every acquisition, by name (never a cached fd), with
//    O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK; its fd must show a regular file, exactly root:root 0644,
//    else InsecureFile (the directory passed: install/Repair replace the file). Then flock(LOCK_EX | LOCK_NB)
//    — never fcntl(F_SETLK), whose record locks are dropped when the process closes ANY fd of the file.
//    The lock lives with the open file description: no child processes exist, so it is held until the fd
//    closes, i.e. until the LockHold is destroyed or the process dies.
//  - readHeldLock() reads the id through the held fd with pread; peekLock() and readRecord() read without
//    the lock (the Linux prologue), with the same owner/mode checks on the opened fd (anything else: Failed).
//  - a busy lock: the holder is named from /proc/locks (best effort, R17): holderDescription().
//  - io()/dir(): PosixFileIo with owner root:root and mode 0644, on the judged directory, for
//    LockIdentity::publish.
//
// apply.lock at /run/aether-serial-bridge.apply.lock (FileApplyLock): /run must be tmpfs and root-owned, not
// group/other-writable; the lock file is opened (created if missing) relative to it with O_RDWR | O_CREAT |
// O_NOFOLLOW | O_CLOEXEC, mode 0600; its fd must show a regular file owned by root; a mode other than 0600 on
// a root-owned file is reset through that fd (fchmod) and reported. flock(LOCK_EX | LOCK_NB), polled every
// 100 ms until the timeout.

#include "core/Interfaces.h"
#include "platform/posix/PosixFileIo.h"
#include "platform/posix/PosixSys.h"

#include <QByteArray>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

namespace asb::posix {

inline constexpr char kEngineLockDirectory[] = "/var/lib/aether-serial-bridge";
inline constexpr char kApplyLockDirectory[] = "/run";
inline constexpr char kApplyLockName[] = "aether-serial-bridge.apply.lock";

// What the adapters report besides their return values (a reset mode, a named squatter). WS-9 folds these
// into Result::fileChanges / the message.
using LockReport = std::function<void(const QString&)>;

// /proc/locks: the pid holding a FLOCK on the inode (dev, ino); nullopt when none is listed.
std::optional<qint64> procLocksHolder(const QByteArray& procLocks, quint64 dev, quint64 ino);

class EngineLock final : public IEngineLock {
public:
    struct Options {
        QString directory = QString::fromLatin1(kEngineLockDirectory);
        PosixOwner owner = PosixOwner::root();   // the directory and both files
        QString procLocks = QStringLiteral("/proc/locks");
        QString procRoot = QStringLiteral("/proc");
    };
    explicit EngineLock(Options, IClock& clock = systemClock(), PosixSys& sys = realPosixSys());
    EngineLock() : EngineLock(Options{}) {}
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

    QString lastError() const { return m_lastError; }
    // "pid 1234 (aether-serial-bridge)" for the last Busy result, empty when /proc/locks did not name it.
    QString holderDescription() const { return m_holder; }

    static constexpr std::chrono::milliseconds kPoll{100};

private:
    // Opens the lock directory (O_NOFOLLOW) and, with `judge`, checks owner, mode and file system; -1 with
    // *why set on failure.
    int openDirectory(LockResult* why, bool judge = true);
    LockFileRead readChecked(const char* name, qint64 cap);
    void nameHolder(int fd);

    Options m_opt;
    IClock& m_clock;
    PosixSys& m_sys;
    PosixFileIo m_io;
    PosixDirHandle m_dirHandle;
    std::shared_ptr<int> m_alive;           // the fd of the live hold, for readHeldLock; -1 when none. The
                                            // hold shares it and sets it back to -1 when it is released
    QString m_lastError, m_holder;
};

class FileApplyLock final : public IApplyLock {
public:
    struct Options {
        QString directory = QString::fromLatin1(kApplyLockDirectory);
        QString name = QString::fromLatin1(kApplyLockName);
        PosixOwner owner = PosixOwner::root();
        bool requireTmpfs = true;           // /run must be tmpfs (INV-116); tests on ext4 turn it off
        LockReport report;
    };
    explicit FileApplyLock(Options, PosixSys& sys = realPosixSys());
    FileApplyLock() : FileApplyLock(Options{}) {}

    std::unique_ptr<LockHold> acquire(std::chrono::seconds timeout) override;

    // Why the last acquire() returned nullptr for a reason other than the timeout (empty for a timeout).
    QString refusalReason() const override { return m_refusal; }

private:
    Options m_opt;
    PosixSys& m_sys;
    QString m_refusal;
};

}  // namespace asb::posix
