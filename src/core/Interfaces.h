#pragma once

// The platform seams of the transaction core (WS-5; master §4.4, §4.6). The Applier, the trust logic
// and BridgeRunner are written against these interfaces only; WS-6 (Windows data folder and locks),
// WS-7 (SCM, CIM firewall, firewall monitor) and WS-8 (POSIX data folder, systemd over D-Bus, locks)
// implement them, and tests/support/Fakes.* implements every one of them with failure injection.
//
// Conventions for every seam below:
//  - Every method that can block in the operating system takes a Deadline: the remaining phase budget.
//    A native call cannot be cancelled; the adapter honours the deadline where it can (D-Bus timeouts,
//    lock polling), and the Applier checks the clock when the call returns (master §4.6 "Deadlines and
//    exclusion"): a call that returned after its phase deadline ends the operation as StateUnknown.
//  - Mutating methods that return void report failure by throwing (PlatformError, or any
//    std::exception); "not running" / "not installed" are not failures where the comment says so.
//  - Nothing here creates a process (S17, INV-112).

#include "core/BridgeConfig.h"
#include "core/FileIo.h"
#include "core/StatusStore.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace asb {

// A point in time by which a blocking call should return. steady_clock, converted to running time by
// whoever arms a watchdog with it (RunningTime.h).
using Deadline = std::chrono::steady_clock::time_point;

// The time source of the Applier, OperationHost and BridgeRunner. Production uses systemClock();
// tests use a fake whose time moves only when the test (or a fake's sleep) moves it, so no test waits
// in real time.
class IClock {
public:
    virtual ~IClock() = default;
    virtual Deadline now() const = 0;
    // Blocks the calling thread for `d` (the fake advances its time instead).
    virtual void sleepFor(std::chrono::milliseconds d) = 0;
};
IClock& systemClock();

// What an adapter throws when a native call fails; `what()` is the user-facing reason (CIM/SCM/D-Bus
// error text).
class PlatformError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A failure after the call had already changed something: `changes` lists what it changed and left on disk
// (for Result::fileChanges), so the caller reports those changes although the call failed (master §4.6 P4:
// folder creation and ACL normalisation are recorded "even if a later step fails"). The list may be empty.
class PlatformErrorAfterChanges : public PlatformError {
public:
    PlatformErrorAfterChanges(const std::string& what, QStringList changesMade)
        : PlatformError(what), changes(std::move(changesMade))
    {
    }
    QStringList changes;
};

// ---------------------------------------------------------------------------------------------------
// Data folder (master §4.4)
// ---------------------------------------------------------------------------------------------------

// Who is looking at the data folder, which decides what may happen to it (master §4.4 table).
enum class TrustMode {
    ReadOnly,   // every viewer: reads through the validated handle, never writes (INV-44)
    Mutating,   // every Applier operation, under apply.lock: create or normalise; untrusted -> refuse
    Repair,     // Applier::repair only: normalise, create, or rename an untrusted folder aside (S2)
    Service,    // --service: read only; anything but Trust -> refuse, never writes (INV-22)
    Scratch,    // --run --config <path outside the data folder>: like Service, different owners
};

// What an inspection decided (DataDirectoryTrust::decide).
enum class DirAction {
    Create,     // nothing at the path
    Trust,      // trusted owner, no untrusted ACE, every child trusted, known names are regular files
    Recreate,   // something is untrusted: only Repair may act on it (rename aside, never delete)
    Refuse,     // a link, junction or file at the path: touch nothing
};

struct Inspection {
    DirAction action = DirAction::Create;
    QString reason;            // why it is not trusted (empty for Trust/Create)
    FileId folderId;           // identity of the inspected directory (mutation handles are checked against it)
    QStringList fileChanges;   // changes the adapter made while inspecting (normally none)
    QStringList children;      // names of the direct children as enumerated (P5 finds stale *.asbtmp here)
};

class IDataFolder {
public:
    virtual ~IDataFolder() = default;
    // Owner, DACL/mode, reparse and regular-file checks of the folder and its direct children
    // (DataDirectoryTrust::decide). Read-only in every mode; the mode only selects the trusted-owner set
    // (Scratch: root + the invoking user).
    virtual Inspection inspect(TrustMode) = 0;
    // The bytes of one known file, read from a handle opened relative to the inspected directory (no
    // link or reparse point followed, regular file only, owner and permissions re-checked on that
    // handle). nullopt on failure with *err set; *err == fileNotFoundError() when the file does not
    // exist. More than `cap` bytes is a failure.
    virtual std::optional<QByteArray> readFile(const Inspection&, const QString& name, qint64 cap, QString* err) = 0;
    // Creates the folder with its final owner and protected DACL/mode in one call (INV-16). Throws.
    virtual void createLockedDown() = 0;
    // Re-applies owner and DACL/mode to the inspected folder and its regular children through
    // identity-checked mutation handles. Returns the changes it made (empty when nothing changed), for
    // Result::fileChanges. Throws; a failure after a change was made throws PlatformErrorAfterChanges
    // listing every change already made, so none of them goes unreported.
    virtual QStringList normalise(const Inspection&) = 0;
    // Repair only (S2, INV-74): renames the inspected folder, with everything inside it, to a fresh
    // sibling name and returns that path. Deletes nothing, follows nothing. Throws (nothing changed).
    virtual QString renameAside(const Inspection&) = 0;
    // The directory handle of the most recent inspection; owned by this folder object and valid for its
    // lifetime. FileIo calls for files in the folder go through it.
    virtual const DirHandle& handle() const = 0;

    // ---- uninstall --remove-settings (WS-16, INV-125) -------------------------------------------------------------
    // The names of the files this program writes in the folder: the only ones uninstall may delete.
    virtual QStringList knownFiles() const = 0;
    // Removes the inspected folder itself when it is empty (never recursive, nothing followed). false + *err otherwise.
    virtual bool removeIfEmpty(const Inspection&, QString* err) = 0;
    // The folders Repair moved aside beside it (D23), as paths: listed by uninstall, never removed.
    virtual QStringList asideFolders() { return {}; }

    // Closes the inspected directory's handle (v1.1 INV-128): a long-lived process (the window, the TUI) holds nothing
    // in the data folder between uses, so an uninstall can remove the folder and nothing is held beside the service.
    // The next inspect() opens and judges it again; handle() and every Inspection made before are invalid after it.
    // OperationHost calls it after every operation; the window's view after every load, log read and status read; the
    // TUI after each of its reads (its load, the pending and supersede checks, the activation read, bridge.log).
    virtual void closeIdle() {}
};

// ---------------------------------------------------------------------------------------------------
// Service (master §4.6 installation states, D33)
// ---------------------------------------------------------------------------------------------------

enum class InstallState { NotInstalled, IncompleteInstall, Current, Foreign };

// detect()'s answer.
struct Installation {
    InstallState state = InstallState::NotInstalled;
    enum class Flavor { Windows, Linux } flavor = Flavor::Windows;
    QString detail;          // why Foreign / IncompleteInstall (for the message)
    // Windows: an IncompleteInstall that is complete except for the program's build: the installed program exists and
    // is readable, the lock files and the SCM configuration are what --install writes, and its digest is not this
    // build's (INV-41). Save & apply asks before it replaces that program (user report, 2026-10-16), and may keep it.
    // Linux too since v1.1 (D61, INV-120): the installed program's SHA-256 against this build's.
    bool otherBuild = false;
    QString installedSha256;   // the installed program's digest when it could be read whole (else empty)
    // The installed program's build stamp (D61), for WORDING only, never an input to the classifier (INV-120): unset
    // when the program was not read; an empty string when it was read and has no stamp (a build before 1.1).
    std::optional<QString> installedVersion;
    // NotInstalled only: no service, but this product's program is still at its installed path (Windows:
    // %ProgramFiles%\AetherSerialBridge\aether-serial-bridge.exe; Linux: /usr/local/bin/aether-serial-bridge). An
    // uninstall that removed the service and then kept the program says "uninstall again to finish"; this is what lets
    // the window offer that Uninstall. Never an input to `state`: a clean machine has no program there.
    bool programRemains = false;
};

struct ServiceQuery {
    enum State { Absent, Stopped, StartPending, Running, StopPending, Other } state = Absent;
    qint64 pid = 0;          // pid only for the ACK comparison, never as a death proof
    // How the last run ended, when the service manager knows (while the service is not running): `exitCode` is the
    // bridge's own exit code when it reported one (Windows: ERROR_SERVICE_SPECIFIC_ERROR, so not activated, reported
    // as NO_ERROR since v1.1's station fix, has none; Linux: ExecMainStatus of a main process that exited);
    // `endedUnreported` = the process ended without reporting (ERROR_PROCESS_ABORTED, 1067): a fault, or the stale
    // firewall exit 7, which reports nothing by design (D56).
    std::optional<int> exitCode;
    bool endedUnreported = false;
};

// What a stopped service's last exit means, for a result or the window (empty while it runs or starts, and after a
// clean exit 0). Stopped with no code names the two causes it can be: a stop, or not activated.
QString describeServiceExit(const ServiceQuery&);

class IServiceControl {
public:
    virtual ~IServiceControl() = default;
    virtual Installation detect(Deadline) = 0;     // read-only; throws when the query fails
    virtual ServiceQuery query(Deadline) = 0;
    virtual void inhibitStarts(Deadline) = 0;      // B1: disabled + RegFlushKey / mask over D-Bus + fsync
    virtual void allowStarts(Deadline) = 0;        // R9: auto (+flush) / unmask + enable
    virtual void requestStop(Deadline) = 0;        // B2: asynchronous; "not running" is not an error
    virtual void installOrConfigure(InstallState, Deadline) = 0;   // R3: binary + service, starts blocked
    virtual void start(Deadline) = 0;
    // Windows: the service only (its program goes in removeProgram()). Linux: the unit, the program, the policy and
    // the staging root (WS-16). "Not installed" is not an error.
    virtual void uninstall(Deadline) = 0;
    virtual void reconcileOutstandingJobs(Deadline) = 0;   // Linux: ListJobs for the unit, wait/cancel; Windows: no-op

    // ---- Uninstall's program side and the Apps & features entry (WS-16; D63, D69, INV-125, INV-126) -------------
    // Windows: true. The program is removed by removeProgram() only after the held lock files were marked for deletion,
    // so a failed clean-up never leaves an Apps & features entry naming a removed program. Linux: false; uninstall()
    // removes the program with the unit, and the three calls below are not made.
    virtual bool separateProgramRemoval() const { return false; }
    // Where the program and the lock files are (Windows: %ProgramFiles%\AetherSerialBridge), for messages.
    virtual QString programFolder() const { return {}; }
    struct ProgramRemoval {
        // Removed; ScheduledAtRestart: the program is this process's own running image, renamed aside in its folder and
        // registered for deletion at the next restart (counts as removed); Absent: there was none; Kept: it is still
        // there under its own name (`error` says why).
        enum class Kind { Removed, ScheduledAtRestart, Absent, Kept } kind = Kind::Absent;
        QStringList changes;   // "removed <path>", "renamed <path> to <path>; it is deleted when Windows restarts"
        QString error;
    };
    virtual ProgramRemoval removeProgram(Deadline) { return {}; }
    // The program folder (the lock directory), after the operation released its locks: removed when it is empty, else
    // kept and named with what is still in it. Never recursive. The lines are for Result::fileChanges.
    virtual QStringList removeProgramFolder() { return {}; }
    // The Apps & features entry naming this build's installed program (D63): written after R3 installed or verified this
    // build's program. Returns an empty string when written, else why not (the install is not failed by it).
    virtual QString writeAppsEntry() { return {}; }
    // Removes the entry; "not there" is not an error. Throws.
    virtual void removeAppsEntry(Deadline) {}
};

// ---------------------------------------------------------------------------------------------------
// Firewall (D51, D53; Windows CIM in WS-7, a no-op on Linux)
// ---------------------------------------------------------------------------------------------------

// The generation of one sync: 16 lower-case hex digits, created once in R6, passed unchanged to sync,
// rollback and the R9 sweep, and written into start.token in R9.
struct FirewallGeneration {
    QByteArray id;
    static FirewallGeneration fresh();
    static bool isValidId(const QByteArray&);
    bool operator==(const FirewallGeneration&) const = default;
};

class IFirewall {
public:
    virtual ~IFirewall() = default;                // all calls synchronous, on the operation worker
    virtual void connect(Deadline) = 0;            // COM setup + ConnectServer (Windows)
    // Plan from FirewallPlan::build; steps from FirewallPlan::syncSteps: create disabled -> filters ->
    // enable, for every rule; then remove every other rule in the group.
    virtual void sync(const BridgeConfig&, const FirewallGeneration&, Deadline) = 0;
    virtual void rollback(const FirewallGeneration&, Deadline) = 0;    // delete only this generation's rules
    virtual void sweep(const FirewallGeneration& keep, Deadline) = 0;  // R9: delete every rule not of `keep`
    virtual void removeAll(Deadline) = 0;
    // Releases the provider connection (Windows: the CIM session) on the operation worker when an operation ends; the
    // next operation connects again. A long-lived host (the window, the TUI) holds no firewall session between
    // operations.
    virtual void disconnect() {}
};

// ---------------------------------------------------------------------------------------------------
// Locks (S3, S15, D54, D58)
// ---------------------------------------------------------------------------------------------------

class LockHold {
public:
    virtual ~LockHold() = default;                 // releases on destruction
    // Windows uninstall (D69): the held file is deleted when this hold closes (FileDispositionInfoEx with POSIX
    // semantics, else the classic FileDispositionInfo on the same share-0 handle). false + *err when neither call
    // succeeded; then nothing was changed. Locks that cannot do it (Linux, where uninstall removes the lock files itself)
    // say so.
    virtual bool markForDeletion(QString* err)
    {
        if (err) *err = QStringLiteral("this lock cannot be marked for deletion");
        return false;
    }
    // Undoes markForDeletion before the hold closes (the program was kept, so its lock files stay with it).
    virtual bool clearDeletion(QString* err)
    {
        if (err) *err = QStringLiteral("this lock cannot be marked for deletion");
        return false;
    }
};

class IApplyLock {
public:
    virtual ~IApplyLock() = default;
    // nullptr when the timeout passed, or when the lock cannot be used at all. Non-inheritable; no child
    // processes exist.
    virtual std::unique_ptr<LockHold> acquire(std::chrono::seconds timeout) = 0;
    // Why the last acquire returned nullptr when it was NOT because another holder kept the lock past the
    // timeout (an insecure or unsupported lock directory or lock file, a failing open); empty otherwise. The
    // Applier reports it instead of "another operation is still running" (WS-6 round-1 class audit).
    virtual QString refusalReason() const { return {}; }
};

// Why an engine.lock acquisition failed. The two "insecure" values differ in
// what may follow:
//  - InsecureFile: the lock DIRECTORY is trusted and on a supported file system, but engine.lock itself
//    fails its handle check (owner, mode / DACL). install/Repair replace such a file after the
//    engine-free proof (the new file is published with the lock files' security); every other caller
//    refuses it like Insecure (the service exits 4).
//  - Insecure: anything that replacing engine.lock would not fix (the lock directory's owner, mode or DACL
//    is wrong, or the adapter cannot tell). Always refused; an administrator must secure the directory.
// An adapter that cannot tell the two apart returns Insecure: the safe default.
enum class LockResult { Acquired, Busy, Missing, Insecure, Unsupported, InsecureFile };

// One of the two lock-identity files (D58), read raw; LockIdentity parses and judges the bytes.
struct LockFileRead {
    enum class Status { Ok, Missing, Unreadable, Failed } status = Status::Failed;
    QByteArray bytes;
    QString error;
};

// engine.lock (S15) and its record engine.lock.id (D58). Never inheritable.
//
// Replacement contract (WS-6 implements the Windows side). On Windows every holder opens
// engine.lock with share mode 0, the Applier included, so engine.lock cannot be renamed over or deleted
// while ANY hold is open. The core therefore never replaces engine.lock while it holds a LockHold:
// install/Repair release their hold (after the engine-free proof; apply.lock and blocked starts stay),
// publish the new engine.lock and then engine.lock.id through io(), acquire again with acquireUntil, and
// compare the ids through the new hold (readHeldLock vs readRecord). The adapter must:
//  - report a replacement refused by a sharing violation as TempFile::Commit{replaced = false} with the
//    OS error (the core reports Uncommitted; it never retries while holding);
//  - open engine.lock by name on every acquireUntil/tryAcquire (never a cached handle or file id), so
//    the hold after a replacement is on the new file;
//  - keep engine.lock.id unlocked (it is replaced while engine.lock is held, for the "partial" pair);
//  - return LockResult::InsecureFile only when the directory itself passed its checks (see LockResult).
class IEngineLock {
public:
    virtual ~IEngineLock() = default;
    virtual std::unique_ptr<LockHold> tryAcquire(LockResult* why) = 0;                  // non-blocking (service)
    virtual std::unique_ptr<LockHold> acquireUntil(Deadline, LockResult* why) = 0;     // polls every 100 ms (Applier)
    // The content of engine.lock read through the HELD handle (D58). Only while holding the lock.
    virtual LockFileRead readHeldLock() = 0;
    // engine.lock read without holding it, for the read-only prologue check (Linux). Unreadable where
    // the OS cannot open it while an engine may hold it (Windows: share mode 0); B3 decides there.
    virtual LockFileRead peekLock() = 0;
    // engine.lock.id (owner and permissions checked by the adapter; at most 64 bytes).
    virtual LockFileRead readRecord() = 0;
    // INV-116: the lock directory (and on Windows the volume) is on a supported file system. Read-only.
    virtual bool filesystemSupported(QString* why) = 0;
    // The lock directory, for publication (LockIdentity::publish: temp, write, flush, rename, directory
    // flush, through this FileIo) and for removing leftover *.asbtmp from an interrupted publication.
    // The adapter's FileIo applies the lock files' security for OwnerPolicy::Config in this directory.
    virtual FileIo& io() = 0;
    virtual const DirHandle& dir() = 0;
    virtual QStringList children() = 0;            // names in the lock directory
    // Closes anything the adapter keeps open between uses (Windows: the lock directory's handle, which dir() opens and
    // a hold keeps), so a long-lived process holds nothing in the lock directory between operations (v1.1 INV-128).
    // OperationHost calls it after every operation, on its worker. A DirHandle from dir() is invalid after it.
    virtual void closeIdle() {}
};

// ---------------------------------------------------------------------------------------------------
// Start token (S18, D52, INV-113). The platform-neutral implementation is FileStartToken (StartToken.h).
// ---------------------------------------------------------------------------------------------------

class IStartToken {
public:
    virtual ~IStartToken() = default;
    enum class Revocation {
        Done,         // overwritten with the revoked form, file flushed (then deleted, best effort)
        Absent,       // no token: nothing to revoke
        NotOpened,    // could not be opened safely: nothing was changed (NothingChanged)
        NotFlushed,   // truncate/write/flush failed: the bytes may already be invalid (Uncommitted)
    };
    // B0: overwrite with {"revoked":true} + file flush, then delete + directory flush (best effort).
    virtual Revocation revoke(Deadline, QString* err) = 0;
    struct Written {
        bool replaced = false;
        bool durable = false;
        QByteArray token;
        QString error;
    };
    // R9: {"token","generation"} via temp + rename (Config policy).
    virtual Written writeFresh(const FirewallGeneration&, Deadline) = 0;
    struct Token {
        QByteArray token;
        FirewallGeneration generation;
    };
    // service: regular, Config owner, not writable by others, <= 256 B, exact JSON, not revoked.
    virtual std::optional<Token> read(QString* why) = 0;
};

// Everything the Applier touches. Built and owned by the host process (WS-9 per OS); it outlives the
// OperationHost and the Applier, which only hold references to it.
struct Platform {
    IDataFolder& folder;
    IServiceControl& service;
    IFirewall& firewall;
    IApplyLock& applyLock;
    IEngineLock& engineLock;
    IStartToken& token;
    IStatusReader& status;
    FileIo& io;
};

// ---------------------------------------------------------------------------------------------------
// Results (D47, master §4.6 "Result")
// ---------------------------------------------------------------------------------------------------

enum class Outcome { NothingChanged, Uncommitted, CommittedNotRunning, StateUnknown, SavedNotApplied, Ok };
enum class Persisted { None, Staged, Committed };

// Where the submitted bytes came from (INV-102): the pending file the editor loaded, the committed
// file, or neither; sha256 is sha256Hex of the bytes the editor loaded from that source.
struct Source {
    enum Kind { Pending, Committed, New } kind = New;
    QByteArray sha256;
};
struct SaveRequest {
    QByteArray config;               // the exact bytes; persistedSha256 is their digest
    Source source;
    bool supersedePending = false;
    // Windows: the installed program is another build (Installation::otherBuild) and the user chose to keep it: the
    // settings are applied to the existing service and its program is not replaced (user report, 2026-10-16).
    bool keepInstalledBuild = false;
    // The window's install route (Install the bridge service, Finish installing, Update to this build, and the main
    // button's Install and apply): the installation the window read before its questions, which the
    // user consented on. Under apply.lock Applier::run refuses NothingChanged, before anything is written, when the
    // installation is no longer that one (an install or another build appeared while a question was open, D67). Unset
    // (the CLI's --install, the TUI, every other route): no such check. Carried in process only: the Linux staged
    // helper's install op has no member for it. A plain member with its own `set` flag, not a std::optional: GCC 13's
    // -Wmaybe-uninitialized misfires on an optional nested in the std::optional<SaveRequest> the actions take.
    struct InstallationBasis {
        bool set = false;   // false: no check (every route but the window's install)
        InstallState state = InstallState::NotInstalled;
        bool otherBuild = false;
        QString installedSha256;   // compared only for another build: consent named that build
    };
    InstallationBasis expectInstallation = {};
};

struct Result {
    Outcome outcome = Outcome::NothingChanged;
    Persisted persisted = Persisted::None;
    QByteArray persistedSha256;      // sha256Hex of the submitted bytes, when persisted != None
    QString step, message, postcondition;
    QStringList fileChanges;
};
QString enumName(Outcome);
QString enumName(Persisted);

// ---------------------------------------------------------------------------------------------------
// Firewall monitor (D56, INV-119). Windows: CimFirewallMonitor (WS-7); Linux: NoFirewallMonitor.
// ---------------------------------------------------------------------------------------------------

class IFirewallMonitor {
public:
    virtual ~IFirewallMonitor() = default;
    enum class Check { Match, Mismatch, Failed };   // Failed = error, partial, missing association
    virtual bool enabled() const = 0;
    virtual void threadInit() = 0;                   // on the monitor thread: own COM apartment + connection
    virtual Check checkOnce(const BridgeConfig&, const FirewallGeneration&) = 0;   // blocking; monitor thread only
    // The reason for the last checkOnce's result (empty on a match); monitor thread only, right after checkOnce.
    virtual QString lastWhy() const { return {}; }
};

class NoFirewallMonitor final : public IFirewallMonitor {
public:
    bool enabled() const override { return false; }
    void threadInit() override {}
    Check checkOnce(const BridgeConfig&, const FirewallGeneration&) override { return Check::Match; }
};

// ---------------------------------------------------------------------------------------------------
// Operation lifetime (D57, INV-118)
// ---------------------------------------------------------------------------------------------------

struct Interim {
    QString phase, message;          // "still processing"; never a final result
};
class IOperationObserver {
public:
    virtual ~IOperationObserver() = default;
    virtual void interim(const Interim&) = 0;        // delivered on the host's thread, at most once per phase
    virtual void finished(const Result&) = 0;        // exactly once, after the locks are released
};

}  // namespace asb
