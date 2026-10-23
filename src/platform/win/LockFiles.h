#pragma once

// The Windows lock directory %ProgramFiles%\AetherSerialBridge and the lock files in it (WS-6; master §4.6
// "Two locks"; INV-66, INV-97, INV-104). Shared by FileApplyLock and EngineLock.
//
// Directory: created only by the Applier, with owner Administrators and a protected DACL in the create call
// (only an elevated process can create under Program Files). Opened by path without following a reparse
// point and judged on that handle: a directory, owner SYSTEM / Administrators / TrustedInstaller, no Unknown
// entry and no entry that lets anyone else write (AceJudge). One narrow exception: an inherit-only CREATOR
// OWNER entry — what every folder under %ProgramFiles% inherits — is not
// counted, because it grants rights only on objects a principal creates there, and the judge already
// requires that only trusted principals can create anything (vetoable; master §4.6 names no exception).
//
// Lock files: opened RELATIVE to that directory handle with share mode 0, never through a reparse point,
// non-inheritable; then validated on the handle (SecurityDescriptor::isLockFileSecure: owner SYSTEM or
// Administrators, protected DACL with only SYSTEM / Administrators entries). Role::Service refuses an
// insecure lock (exit 4). Role::Applier repairs a lock whose owner is trusted but whose DACL is wider,
// through a SEPARATE handle opened with READ_CONTROL | WRITE_DAC | WRITE_OWNER whose FILE_ID_INFO must
// equal the lock handle's, and reports it (LockReport); an untrusted owner is refused, never taken over.
//
// Results (WS-5 round 2): a lock file that fails its owner/DACL check inside a directory that passed its own
// checks is InsecureFile — the Service's refusal, an untrusted owner, or a repair that failed (mutation handle,
// identity, setSecurity, read-back) — which install/Repair replace after the engine-free proof. Everything the
// adapter cannot attribute to the file's owner/DACL (a failing directory, an open that fails, not a regular
// file, unreadable or undecodable permissions) is Insecure: never replaced. A DACL repaired in place is
// Acquired (reported through LockReport, not as a LockResult).

#include "core/Interfaces.h"
#include "platform/win/NtSeams.h"
#include "platform/win/WinFileIo.h"

#include <functional>

namespace asb::win {

enum class LockRole { Applier, Service };
using LockReport = std::function<void(const QString&)>;

class LockDirectory {
public:
    LockDirectory(NtSeams& seams, QString path, LockRole role, LockReport report);

    // Opens (Role::Applier with `create`: creates when absent) and judges the directory. Acquired, Missing
    // (absent and not created), Insecure or Unsupported (not NTFS/ReFS, or remote: INV-116), with *why.
    LockResult open(bool create, QString* why);
    // Read-only: the volume of the directory (opened if needed).
    bool volumeSupported(QString* why);
    const WinDirHandle& dir() const { return m_dir; }
    QString path() const { return m_path; }
    // Closes the directory handle (open() opens and judges it again).
    void close() { m_dir = WinDirHandle(); }

    // Opens `name` in the directory with share mode 0 and validates (and, for the Applier, repairs) it.
    // Busy = another handle holds it (sharing violation); Missing = it does not exist (FILE_OPEN).
    LockResult openLockFile(const QString& name, quint32 access, quint32 disposition, OwnedHandle* out, QString* why);

    // Judges a directory descriptor as described above (pure, exposed for tests).
    static bool directoryDescriptorTrusted(const QByteArray& sd, QString* why);

private:
    NtSeams& m_s;
    QString m_path;
    LockRole m_role;
    LockReport m_report;
    WinDirHandle m_dir;
};

}  // namespace asb::win
