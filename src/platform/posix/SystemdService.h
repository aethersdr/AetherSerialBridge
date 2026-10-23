#pragma once

// IServiceControl for systemd over D-Bus (WS-8; master §4.6, §4.8; D21, D33, D37, D51, D55; INV-48, 79,
// 98, 115). No systemctl, no child process.
//
// Authority: the adapter runs only as root (the mutating CLI commands, the pkexec helper); every call that
// talks to the manager refuses otherwise. The DynamicUser service never constructs it.
//
// Steps (the master's table):
//  detect            read-only, files only: /etc/systemd/system/<unit> (mask link, or foreign), the Qt
//                    unit in /usr/local/lib/systemd/system, the binary, the lock files ->
//                    InstallationEvidence::classifyLinux. The binary is hashed
//                    (v1.1 D61, INV-120) when it is a regular file of the owner (never through a link; read through
//                    the opened fd, at most 256 MiB) and compared with Options::expectedSha256; its build stamp
//                    is read for wording only.
//  inhibitStarts     ServicePlan::blockStarts(Linux): MaskUnitFiles(persistent, never --runtime: the mask must
//                    survive a reboot, INV-48), DisableUnitFiles, Reload, fsync /etc/systemd/system; verified
//                    LoadState = masked.
//  requestStop       StopUnit(replace); the job is not awaited (the engine lock is the proof); a unit systemd
//                    does not know is a no-op (INV-79).
//  installOrConfigure  MASK FIRST (allowed before the unit exists; INV-79: the mask link precedes the unit
//                    file), then the unit file, the binary, the polkit policy and the lock directory
//                    (root:root 0755), each written temp -> fsync -> rename -> fsync(dir); the legacy policy
//                    (agency.cloaked.*, up to v1.1.0) removed once the current one is written; Reload; verified
//                    masked. The lock files themselves are published by the core (LockIdentity) after the
//                    engine-free proof.
//  allowStarts       ServicePlan::allowStarts(Linux): UnmaskUnitFiles, EnableUnitFiles, Reload, fsync;
//                    verified LoadState = loaded and UnitFileState = enabled.
//  start             StartUnit(replace), awaited: JobRemoved with result "done"; verified ActiveState = active.
//  uninstall         StopUnit, DisableUnitFiles, UnmaskUnitFiles, then the unit file, binary, policy (and the
//                    legacy policy) and lock directory removed, Reload; verified LoadState = not-found.
//  reconcileOutstandingJobs  ListJobs for the unit: a job still waiting is cancelled (CancelJob), a running one
//                    is awaited; until none is left or the deadline (before R9 and before uninstall).
//  query             LoadUnit + LoadState/ActiveState/MainPID + GetUnitProcesses. "Stopped" (pid 0) only when
//                    the unit is inactive or failed AND its process set is empty: the engine-free proof.
//
// Job tracking (INV-115): Subscribe() once; the JobRemoved receiver is installed before every submission;
// completions that arrive before the reply are kept (by exact job path) and matched when the reply supplies
// the path; a manager owner change fails the step; any result but "done" fails it. Every call's timeout is the
// remaining phase budget, and a call is not sent once the deadline has passed. A timed-out call may still be
// executed by PID 1 later (R16): that is what reconcileOutstandingJobs is for.

#include "core/Interfaces.h"
#include "platform/posix/PosixSys.h"
#include "platform/posix/SystemdBus.h"

#include <QHash>

#include <functional>
#include <optional>

class QThread;

namespace asb::posix {

// detect()'s last digest of the installed program (v1.1 WS-14 risk: hashing a 30-45 MB program on every poll costs
// ~200 ms), reused only while the file's device, inode, size, mtime and ctime are unchanged. A write moves ctime, but
// only to the kernel's coarse clock: a same-size rewrite in place within one tick of the hash keeps every one of
// those fields. So a digest is kept only for a settled file, one whose ctime was already 2 s older than
// the wall clock when it was hashed (the racy-clean rule): any later write then lands on a later ctime. A file
// younger than that is hashed on every call. The one hole left is the wall clock stepping back past the cached ctime.
struct ProgramDigestCache {
    bool valid = false;
    dev_t dev = 0;
    ino_t ino = 0;
    off_t size = 0;
    struct timespec mtime {}, ctime {};
    QString sha256;
    std::optional<QString> version;
};

class SystemdService final : public IServiceControl {
public:
    struct Options {
        QString unitName = QStringLiteral("aether-serial-bridge.service");
        QString unitDirectory = QStringLiteral("/usr/local/lib/systemd/system");
        QString etcDirectory = QStringLiteral("/etc/systemd/system");
        QString binaryPath = QStringLiteral("/usr/local/bin/aether-serial-bridge");
        QString sourceBinary = QStringLiteral("/proc/self/exe");   // what install copies (the running program)
        QString lockDirectory = QStringLiteral("/var/lib/aether-serial-bridge");
        // WS-15's staging root (the window's staged lifecycle program): uninstall removes it, its "staging.*" directories
        // included (WS-16). Empty: none.
        QString stagingRoot = QStringLiteral("/usr/local/lib/aether-serial-bridge");
        QString policyPath = QStringLiteral("/usr/share/polkit-1/actions/org.aethersdr.aetherserialbridge.policy");
        // The policy every release up to v1.1.0 installed, before the identifiers moved to org.aethersdr (2026-10-22). Its
        // action names the same program and argument as policyPath's, and two such actions leave pkexec's choice between
        // them unspecified, so it is never kept beside the current one: install refuses a non-file there before any change and
        // removes it after writing policyPath, and uninstall removes it with policyPath. Only with `policy` set; empty, or
        // equal to policyPath: never touched.
        QString legacyPolicyPath = QStringLiteral("/usr/share/polkit-1/actions/agency.cloaked.aetherserialbridge.policy");
        QByteArray policy;                 // the polkit policy (WS-9); empty: none written or removed
        PosixOwner owner = PosixOwner::root();
        // This build's program digest (lower-case SHA-256 hex; v1.1 D61, INV-120): detect() hashes the installed
        // program and compares. Empty (a caller that does not know its build) = never Current.
        QByteArray expectedSha256;
        bool requireRoot = true;           // tests over the fake bus turn it off
        // The wall clock detect() compares the installed program's ctime with before it keeps that program's digest
        // (CLOCK_REALTIME when empty; a test seam).
        std::function<struct timespec()> wallClock;
        // Test seam: called at named points ("after-mask", "before-allow-starts", "before-start"); the
        // systemd suite pauses there to kill the process.
        std::function<void(const QString&)> onStep;
    };

    SystemdService(ISystemdBus& bus, Options options, IClock& clock = systemClock(), PosixSys& sys = realPosixSys());

    Installation detect(Deadline) override;
    ServiceQuery query(Deadline) override;
    void inhibitStarts(Deadline) override;
    void allowStarts(Deadline) override;
    void requestStop(Deadline) override;
    void installOrConfigure(InstallState, Deadline) override;
    void start(Deadline) override;
    void uninstall(Deadline) override;
    void reconcileOutstandingJobs(Deadline) override;

    // What the steps changed on disk, for Result::fileChanges (WS-9 folds them in); cleared by the caller.
    QStringList takeChanges();
    // Uninstall's last file step (WS-16): the staging root and its "staging.*" directories, one level of regular files
    // each, nothing followed. A staged program running this uninstall is unlinked (Linux allows it). Anything else in
    // it is kept and named, and so is the root.
    void removeStagingRoot();

    const Options& options() const { return m_opt; }

private:
    std::chrono::milliseconds remaining(Deadline, const char* what) const;
    void requireRoot() const;
    void ensureReceivers();
    void subscribeOnce(Deadline);
    // Submits a job-returning call (receivers installed first), then waits for its JobRemoved by exact path.
    QString awaitJob(const char* what, const std::function<QString(std::chrono::milliseconds)>& submit, Deadline);
    void runSteps(bool block, Deadline);
    void syncUnitDirectories();
    QString loadState(Deadline);
    QString unitProperty(const char* iface, const char* property, Deadline);
    void verify(const char* property, const QString& want, Deadline);
    int openDirectories(const QString& path);   // openDirectoryCreating, its creations recorded; throws
    void writeFile(const QString& directory, const QString& name, const QByteArray& bytes, mode_t mode);
    void copyBinary();
    void ensureLockDirectory();
    void removeFile(const QString& path);
    bool legacyPolicyApplies() const;   // a policy to write, and a legacy path that is set and not policyPath
    void removeLegacyPolicy();
    void step(const char* name);

    ISystemdBus& m_bus;
    Options m_opt;
    IClock& m_clock;
    PosixSys& m_sys;
    bool m_subscribed = false;
    bool m_receiversInstalled = false;
    QThread* m_receiverThread = nullptr;
    bool m_ownerChanged = false;
    QHash<QString, QString> m_done;   // job path -> result, for jobs of our unit
    QStringList m_changes;
    ProgramDigestCache m_programCache;
};

}  // namespace asb::posix
