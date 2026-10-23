#pragma once

// Fakes for every WS-5 seam (master §4.6; WS-5 "Fakes"): FakeFolder, FakeService, FakeFirewall,
// FakeApplyLock, FakeEngineLock, FakeStatus, a fake clock, and the BridgeRunner side (engine, side
// channel, watchdog, monitor). They share one EventLog so tests can assert the order of calls across
// seams, and every fake checks that the apply lock is held when the Applier calls it (INV-24).
//
// The world they model (FakeWorld): the data folder's files live in a FakeFileIo (WS-3, crash model
// included), the lock directory in a second one; the "engine" is the managed service process: started
// by FakeService::start (when starts are allowed), it holds engine.lock while it runs, reads start.token
// and config.json, and publishes a status report (FakeStatus) as the real engine would — or a stale or
// wrong one, as the test scripts it.

#include "core/AceJudge.h"
#include "core/Interfaces.h"
#include "core/IBridgeEngine.h"
#include "core/Exit.h"
#include "core/RunningTime.h"
#include "support/FakeFileIo.h"

#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>

namespace asb::test {

// ---------------------------------------------------------------------------------------------------
// Shared event log
// ---------------------------------------------------------------------------------------------------
class EventLog {
public:
    void add(const QString& e);
    QStringList all() const;
    void clear();
    // WS-5 round 3 (tst_IrreversibleGate): called after every add, outside the lock (e.g. to make the call
    // that logged it return late); and a source for the operation phase each event happened in (a test
    // passes a PhaseClock to the Applier and reads it here), so a step failClosed repeats as cleanup can be
    // told apart from the step itself.
    std::function<void(const QString& event)> onAdd;
    std::function<int()> phaseOf;
    // The phase recorded with event i (-1 when no phaseOf was set).
    int phaseAt(int i) const;
    // Index of the first event starting with `prefix` at or after `from`; -1 if none.
    int indexOf(const QString& prefix, int from = 0) const;
    bool contains(const QString& prefix) const { return indexOf(prefix) >= 0; }
    int count(const QString& prefix) const;

private:
    mutable std::mutex m_m;
    QStringList m_events;
    QList<int> m_phases;
};

// ---------------------------------------------------------------------------------------------------
// Clock: time moves only when advanced (or when something sleeps on it). It keeps two times, as a real
// machine does (RunningTime.h): steady time (now(), what IClock gives) and running time (running()).
// advance() and sleepFor() move both by the same amount; suspend() moves only steady time, as a machine
// asleep does on Windows. Until a test suspends, the two read the same count.
// ---------------------------------------------------------------------------------------------------
class FakeClock final : public IClock {
public:
    Deadline now() const override;
    void sleepFor(std::chrono::milliseconds d) override;
    void advance(std::chrono::nanoseconds d);
    // Running time (suspend excluded).
    RunningTime::Duration running() const;
    // The machine sleeps for `d`: steady time passes, running time does not.
    void suspend(std::chrono::nanoseconds d);
    // { running, now } over this clock, for the seams that take RunningTime::Clocks.
    RunningTime::Clocks clocks();
    // Called after every sleepFor (e.g. so a fake engine can react to the passing time).
    std::function<void()> onSleep;

private:
    std::atomic<std::chrono::steady_clock::rep> m_ticks{1'000'000'000};
    std::atomic<RunningTime::Duration::rep> m_running{1'000'000'000};
};

// ---------------------------------------------------------------------------------------------------
// Apply lock: a real cross-thread mutex, so INV-47 (Stop waits for a running apply) is observable.
// ---------------------------------------------------------------------------------------------------
class FakeApplyLock final : public IApplyLock {
public:
    explicit FakeApplyLock(EventLog* log = nullptr) : m_log(log) {}
    std::unique_ptr<LockHold> acquire(std::chrono::seconds timeout) override;
    bool held() const { return m_held.load(); }
    bool alwaysTimeout = false;
    std::optional<QString> refuse;  // acquire fails at once with this refusalReason() (an insecure lock directory)
    QString refusalReason() const override { return m_refusal; }
    std::atomic<int> waiting{0};   // threads blocked in acquire
    // WS-16 (D69): apply.lock as a file in the lock directory. A hold marked for deletion deletes it when it closes;
    // the next acquire creates it again (FILE_OPEN_IF).
    bool present = true;
    bool marked = false;
    std::optional<QString> failMark, failClear;   // markForDeletion / clearDeletion refused with this error

private:
    friend class FakeApplyHold;
    void release();
    EventLog* m_log;
    QString m_refusal;
    std::mutex m_m;
    std::condition_variable m_cv;
    std::atomic<bool> m_held{false};
};

// Everything the Applier-side fakes share.
struct FakeWorld {
    EventLog log;
    FakeClock clock;
    FakeFileIo files;        // the data folder's entries
    FakeFileIo lockFiles;    // the lock directory's entries (engine.lock, engine.lock.id)
    FakeApplyLock applyLock{&log};
    QStringList violations;  // seam calls made without the apply lock (INV-24)
    std::mutex violationsMutex;
    bool checkLock = true;   // false for calls the service host makes (BridgeRunner tests)

    void touch(const QString& call);   // log + lock check
};

// ---------------------------------------------------------------------------------------------------
// Data folder over FakeFileIo. Owners are FakeFileIo's strings: "root" (Config policy) and "process"
// (Runtime policy) are trusted, "attacker" is not.
// ---------------------------------------------------------------------------------------------------
class FakeFolder final : public IDataFolder {
public:
    explicit FakeFolder(FakeWorld& w);

    enum class DirState { Missing, Directory, Link, File };
    DirState state = DirState::Directory;
    QString owner = QStringLiteral("root");
    bool folderWritableByOthers = false;
    QSet<QString> writableByOthers;     // files with an untrusted write ACE / group-other write bit
    std::optional<QString> failCreate, failNormalise, failRenameAside;
    std::function<void(const QString& method)> onCall;   // after create / normalise / renameAside began
    QStringList normaliseChanges;       // what normalise() reports having changed (also with failNormalise:
                                        // made, then the failure)
    // Counters (INV-22, INV-44): create/normalise/renameAside calls.
    int creates = 0, normalises = 0, renames = 0;
    int mutations() const { return creates + normalises + renames; }
    int inspections = 0;
    QStringList reads;                  // names passed to readFile
    std::function<void(const QString& name)> onRead;
    QMap<QString, QByteArray> aside;    // what renameAside moved away (name -> bytes)
    QString asidePath;
    TrustPolicy policy{{QStringLiteral("root"), QStringLiteral("process")}, {}};
    // Files whose owner must be the Config owner ("root") when read: the service's own Runtime files
    // ("process") are trusted in the folder, but never as configuration or as the start token.
    QSet<QString> configOwned{QStringLiteral("config.json"), QStringLiteral("config.pending.json"),
                              QStringLiteral("start.token")};
    // WS-16: the files uninstall --remove-settings may delete (the Windows list), the folders Repair moved aside, and a
    // removal of the (empty) folder that fails.
    QStringList known{QStringLiteral("config.json"), QStringLiteral("config.pending.json"), QStringLiteral("start.token"),
                      QStringLiteral("status.json"), QStringLiteral("bridge.log"), QStringLiteral("bridge.log.1")};
    QStringList asides;
    std::optional<QString> failRemoveFolder;

    Inspection inspect(TrustMode) override;
    std::optional<QByteArray> readFile(const Inspection&, const QString& name, qint64 cap, QString* err) override;
    void createLockedDown() override;
    QStringList normalise(const Inspection&) override;
    QString renameAside(const Inspection&) override;
    const DirHandle& handle() const override { return m_dir; }
    QStringList knownFiles() const override { return known; }
    bool removeIfEmpty(const Inspection&, QString* err) override;
    QStringList asideFolders() override { return asides; }
    // v1.1 INV-128: the folder's handle is open from an inspection until closeIdle() (the real adapters close it there).
    bool directoryOpen = false;
    void closeIdle() override { directoryOpen = false; }

private:
    FakeWorld& m_w;
    quint64 m_ino = 7;
    FakeDir m_dir{7};
};

// ---------------------------------------------------------------------------------------------------
// FileIo decorator over the world's FakeFileIo: logs every call into the EventLog, checks the apply
// lock, records the directory identity of each call, can tamper with read-back bytes and can "crash"
// (throw SimulatedCrash, which the Applier does not catch) before a chosen operation.
// ---------------------------------------------------------------------------------------------------
struct SimulatedCrash {
    QString at;
};

class SpyFileIo final : public FileIo {
public:
    SpyFileIo(FakeWorld& w, FakeFileIo& inner, QString tag) : m_w(w), m_inner(inner), m_tag(std::move(tag)) {}

    // "createTemp", "write", "flush", "readBack", "commitAs", "remove", "syncDir", "openExisting",
    // "truncate", "writeAll", "flushExisting", "removeByHandle" — with the target name.
    void crashBefore(const QString& op, const QString& name) { m_crashOp = op; m_crashName = name; }
    // Crash before the n-th operation (1-based) through this FileIo, counted from now.
    void crashAtOperation(int n) { m_crashAt = m_ops + n; }
    int operations() const { return m_ops; }
    std::function<QByteArray(const QByteArray&)> tamperReadBack;
    std::function<void(const QString& op, const QString& name)> beforeHook;   // runs before every operation
    // A non-empty answer makes a commitAs or remove of `name` fail with that error, nothing changed (the
    // engine lock's Windows sharing model). Separate from beforeHook, which tests overwrite.
    std::function<QString(const QString& op, const QString& name)> refuse;
    QList<FileId> dirIds;                       // the DirHandle id of every call
    struct TempRecord { int serial; QString name; bool readBack = false; QByteArray readBytes; QString committedAs; };
    QList<TempRecord> temps;                    // every temp, in creation order

    std::unique_ptr<TempFile> createTemp(const DirHandle&, OwnerPolicy, const NameGen&, QString* err) override;
    bool remove(const DirHandle&, const QString& name, QString* err) override;
    bool syncDir(const DirHandle&, QString* err) override;
    std::unique_ptr<ExistingFile> openExisting(const DirHandle&, const QString& name, QString* err) override;

    // internal
    void before(const QString& op, const QString& name);
    FakeWorld& world() { return m_w; }
    const QString& tag() const { return m_tag; }

private:
    FakeWorld& m_w;
    FakeFileIo& m_inner;
    QString m_tag;
    QString m_crashOp, m_crashName;
    int m_ops = 0;
    int m_crashAt = -1;
};

// ---------------------------------------------------------------------------------------------------
// Service + the managed engine process.
// ---------------------------------------------------------------------------------------------------
enum class AckMode {
    Correct,        // status with the token, pid, digest of config.json, no configError
    Never,          // no status at all
    ConfigError,    // correct ACK fields but configError set
    StaleToken,     // a report with the previous token and the same pid (pid reuse)
    WrongPid,
    WrongDigest,
};

class FakeService final : public IServiceControl {
public:
    explicit FakeService(FakeWorld& w);

    Installation installation;            // what detect() answers
    bool startsBlocked = false;
    bool engineRunning = false;           // the engine process exists
    bool engineHoldsLock = false;         // ... and holds engine.lock
    qint64 pid = 0;
    // The engine keeps engine.lock this many 100 ms polls after a stop request (STOPPED-before-exit).
    int releaseAfterPolls = 0;
    bool neverReleases = false;           // engine alive past every deadline
    AckMode ack = AckMode::Correct;
    std::optional<StatusReport> status;   // what FakeStatus returns
    QByteArray previousToken = QByteArrayLiteral("00000000000000000000000000000000");

    // Failure injection: method name -> error; "installOrConfigure" can also leave a partial service.
    QMap<QString, QString> fail;
    bool partialInstallRunning = false;   // installOrConfigure creates an auto+running service, then throws
    bool startThrowsAfterRunning = false; // start brings the engine up, then throws
    // The engine's firewall check at start finds the rules wrong: it ends without a report (exit 7, D56); query()
    // then says so as SCM would (endedUnreported). Every other refusal reports its code (lastExitCode).
    bool staleFirewallAtStart = false;
    std::optional<ServiceQuery::State> stateOverride;   // what query() says the state is (e.g. StartPending)
    std::optional<int> lastExitCode;
    bool lastEndedUnreported = false;
    std::function<void(const QString& method)> onCall;   // e.g. block inside a call (OperationHost tests)
    std::chrono::milliseconds callDuration{0};           // each call advances the clock this much

    // WS-16: the program, its folder and the Apps & features entry. `separateProgram` = the Windows model (D69); false
    // = Linux, where uninstall() removes the program with the unit.
    bool separateProgram = true;
    bool programPresent = false;          // the installed program (set by installedRunning and installOrConfigure)
    bool programIsOwnImage = false;       // this process runs from it: removeProgram renames it aside and schedules it
    bool programScheduled = false;        // ... and it was (counts as removed)
    bool folderPresent = false;           // the program folder (the lock directory)
    QStringList folderOthers;             // other things in the folder: removeProgramFolder keeps it
    struct Entry {
        bool present = false;
        QString uninstallExe;             // the program UninstallString names
        QString version;
    } entry;
    int entryWrites = 0;
    QString installedExePath = QStringLiteral("C:\\Program Files\\AetherSerialBridge\\aether-serial-bridge.exe");
    QString version = QStringLiteral("1.1.0");
    // The entry, when present, names a program that exists (INV-126: keptEntryNamesExistingProgram).
    bool entryNamesExistingProgram() const
    {
        return !entry.present || (entry.uninstallExe == installedExePath && programPresent);
    }

    // IServiceControl
    Installation detect(Deadline) override;
    ServiceQuery query(Deadline) override;
    void inhibitStarts(Deadline) override;
    void allowStarts(Deadline) override;
    void requestStop(Deadline) override;
    void installOrConfigure(InstallState, Deadline) override;
    void start(Deadline) override;
    void uninstall(Deadline) override;
    void reconcileOutstandingJobs(Deadline) override;
    bool separateProgramRemoval() const override { return separateProgram; }
    QString programFolder() const override { return QStringLiteral("C:\\Program Files\\AetherSerialBridge"); }
    ProgramRemoval removeProgram(Deadline) override;
    QStringList removeProgramFolder() override;
    QString writeAppsEntry() override;
    void removeAppsEntry(Deadline) override;

    // The engine's side.
    void launchEngine();                  // what SCM/systemd starting the process does
    void onLockPoll();                    // one 100 ms poll of the Applier's acquireUntil passed
    bool stopRequested = false;
    const bool* m_lockedByApplier = nullptr;   // set by FakeEngineLock

private:
    void call(const QString& method);
    FakeWorld& m_w;
    qint64 m_nextPid = 4000;
};

class FakeStatus final : public IStatusReader {
public:
    explicit FakeStatus(FakeService& s) : m_s(s) {}
    std::optional<StatusReport> read() override { return m_s.status; }

private:
    FakeService& m_s;
};

// ---------------------------------------------------------------------------------------------------
// Engine lock + its record, files in FakeWorld::lockFiles.
// ---------------------------------------------------------------------------------------------------
class FakeEngineLock final : public IEngineLock {
public:
    FakeEngineLock(FakeWorld& w, FakeService& s);

    bool peekUnreadable = false;          // Windows model: an existing lock cannot be opened unheld
    // Windows model: engine.lock is opened with share mode 0 by whoever holds it (the engine or the
    // Applier), so while it is held it cannot be replaced or removed: commitAs/remove of engine.lock
    // through io() fail with a sharing violation (WS-6, MoveFileExW over a share-0 handle).
    bool windowsSharing = false;
    void windows() { peekUnreadable = windowsSharing = true; }
    // The lock DIRECTORY is not secured (LockResult::Insecure): nothing a replacement would fix. An
    // engine.lock FILE with the wrong owner (plantInsecureLock: owner "attacker") is LockResult::InsecureFile,
    // a property of that file, so a replacement clears it.
    bool insecure = false;
    std::optional<QString> unsupportedFs;
    bool appliersHold() const { return m_applierHolds; }
    // Plants a matching pair with `id` (or only one of the files).
    void plantPair(const QByteArray& id);
    void plantLock(const QByteArray& content);
    void plantRecord(const QByteArray& content);
    void plantInsecureLock(const QByteArray& content);
    bool lockFileInsecure() const;
    // WS-16 (D69): the held engine.lock marked for deletion is deleted when the hold closes.
    bool marked = false;
    std::optional<QString> failMark, failClear;
    // WS-5 round 3: runs inside readHeldLock ("held") and readRecord ("record"), e.g. to make an identity
    // read return after the barrier's deadline.
    std::function<void(const QString& which)> onRead;
    QByteArray lockContent() const;
    QByteArray recordContent() const;

    std::unique_ptr<LockHold> tryAcquire(LockResult* why) override;
    std::unique_ptr<LockHold> acquireUntil(Deadline, LockResult* why) override;
    LockFileRead readHeldLock() override;
    LockFileRead peekLock() override;
    LockFileRead readRecord() override;
    bool filesystemSupported(QString* why) override;
    FileIo& io() override { return m_io; }
    // v1.1 INV-128 model: the lock directory's handle (Windows) is open while a hold lives or after dir(), until the hold
    // is released or closeIdle() (after every operation) closes it.
    const DirHandle& dir() override
    {
        directoryOpen = true;
        return m_dir;
    }
    QStringList children() override;
    void closeIdle() override { directoryOpen = false; }
    bool directoryOpen = false;

    void release();

private:
    LockFileRead readFile(const QString& name) const;
    FakeWorld& m_w;
    FakeService& m_s;
    SpyFileIo m_io;
    FakeDir m_dir{99};
    bool m_applierHolds = false;
};

// ---------------------------------------------------------------------------------------------------
// Firewall: models the rules of the reserved group, applying FirewallPlan's steps.
// ---------------------------------------------------------------------------------------------------
class FakeFirewall final : public IFirewall {
public:
    explicit FakeFirewall(FakeWorld& w) : m_w(w) {}
    QStringList rules;                    // InstanceIDs in the group, in creation order
    std::optional<BridgeConfig> syncedFrom;
    FirewallGeneration lastGeneration;
    QMap<QString, QString> fail;          // method -> error
    std::function<void(const QString& method)> onCall;
    std::chrono::milliseconds callDuration{0};
    // The interface's prerequisite (Interfaces.h: connect() does the COM setup and ConnectServer): sync,
    // rollback, sweep and removeAll are logged, then fail as the native adapter's would, until a connect
    // succeeded. The connection lasts as long as the Rig.
    bool connected = false;
    int disconnects = 0;                  // the host released the session after an operation (not logged)

    void connect(Deadline) override;
    void disconnect() override
    {
        connected = false;
        ++disconnects;
    }
    void sync(const BridgeConfig&, const FirewallGeneration&, Deadline) override;
    void rollback(const FirewallGeneration&, Deadline) override;
    void sweep(const FirewallGeneration& keep, Deadline) override;
    void removeAll(Deadline) override;

private:
    void call(const QString& method);
    void requireConnection(const QString& method) const;
    FakeWorld& m_w;
};

// ---------------------------------------------------------------------------------------------------
// One Applier-side rig: the world, every fake, and the Platform over them.
// ---------------------------------------------------------------------------------------------------
struct Rig {
    Rig();
    ~Rig();
    FakeWorld world;
    FakeFolder folder{world};
    SpyFileIo io{world, world.files, QStringLiteral("io")};
    FakeService service{world};
    FakeStatus status{service};
    FakeEngineLock engineLock{world, service};
    FakeFirewall firewall{world};
    std::unique_ptr<IStartToken> token;
    std::unique_ptr<Platform> platform;

    // A typical installed station: trusted folder with config A committed, a valid start token,
    // matching lock pair, service Current and running config A.
    void installedRunning(const QByteArray& committed);
    QByteArray file(const QString& name) const { return world.files.contents(name); }
    bool has(const QString& name) const { return world.files.exists(name); }
};

// Configs used throughout.
QByteArray configA();   // port 4001 -> COM3, allow 10.0.0.5
QByteArray configB();   // port 4001 -> COM4, allow 0.0.0.0/0
QByteArray configC();   // port 4002 -> COM5
QByteArray digest(const QByteArray&);   // sha256Hex as bytes
// WS-5 round 6: does `text` claim the engine stopped ("stopped", "verified", "the bridge is off", "no engine
// process")? Sentences that say a stop was NOT proven, and those about the operation itself, are not claims.
bool claimsStop(QString text);

// ---------------------------------------------------------------------------------------------------
// Runner side
// ---------------------------------------------------------------------------------------------------
class FakeEngine final : public IBridgeEngine {
public:
    explicit FakeEngine(EventLog& log) : m_log(log) {}
    std::optional<BridgeConfig> started;
    int stops = 0;
    void start(const BridgeConfig& c) override;
    void stop(std::function<void()> done) override;
    QList<PortStatus> snapshot() const override;

private:
    EventLog& m_log;
};

class FakeSideChannel final : public ISideChannel {
public:
    explicit FakeSideChannel(EventLog& log) : m_log(log) {}
    bool throwOnPublish = false;
    QList<StatusReport> published;
    void log(QtMsgType, const QString&) override {}
    void publishStatus(const StatusReport&) override;
    bool flush(std::chrono::milliseconds) override { return true; }

private:
    EventLog& m_log;
};

// The watchdog's rules are exactly DeadlineState's; expiry is observed, never acted on.
class FakeWatchdog final : public IDeadlineWatchdog {
public:
    FakeWatchdog(EventLog& log, FakeClock& clock) : m_log(log), m_clock(clock) {}
    void arm(std::chrono::steady_clock::time_point d, int code) override;
    void extend(std::chrono::steady_clock::time_point d) override;
    void tighten(std::chrono::steady_clock::time_point d, int code) override;
    void expireNow() override;
    // Called at the start of every extend(), on the caller's thread: a successful check has just returned (the
    // monitor extends right after it). A test advances the clock here for a pause between check and listen.
    std::function<void()> onExtend;
    // Called at the start of every tighten(), with its 0-based call number (e.g. hold the monitor before a retry).
    std::function<void(int n)> onTighten;
    // Like the real watchdog, it converts each steady deadline to running time AT THE CALL
    // (RunningTime::fromSteady over the fake clock) and judges expiry in running time.
    // Whether the deadline has passed at the fake clock's running time (what the watchdog thread would see).
    bool expired() const;
    bool expiredNow = false;    // expireNow() was called
    std::optional<int> code() const;
    // The running deadline, as a time point on the steady epoch: equal to the steady deadline a test
    // computes while it has not suspended the clock (the two times then read the same count).
    std::chrono::steady_clock::time_point deadline() const;
    RunningTime::Duration runningDeadline() const;
    // The running deadline the latest extend() asked for (whether or not it moved the deadline).
    std::optional<RunningTime::Duration> lastExtend() const;
    std::atomic<int> tightenCalls{0};   // onTighten's counter
    bool armed() const;

private:
    RunningTime::Duration toRunning(std::chrono::steady_clock::time_point d) const;
    mutable std::mutex m_m;
    EventLog& m_log;
    FakeClock& m_clock;
    std::optional<RunningTime::Duration> m_deadline;
    std::optional<RunningTime::Duration> m_lastExtend;
    std::optional<int> m_code;
};

class FakeMonitor final : public IFirewallMonitor {
public:
    explicit FakeMonitor(EventLog& log, FakeClock& clock) : m_log(log), m_clock(clock) {}
    bool isEnabled = true;
    // Results returned by successive checks (the last one repeats); each check takes `checkDuration`
    // of fake time.
    QList<Check> results{Check::Match};
    std::chrono::milliseconds checkDuration{100};
    std::function<void(int n)> duringCheck;   // e.g. block the n-th check
    std::atomic<int> checks{0};
    std::atomic<int> lastTcpPort{0};          // the first port of the config the latest check compared against
    QString why = QStringLiteral("fake reason");   // lastWhy(), read by the runner after a non-match
    bool enabled() const override { return isEnabled; }
    QString lastWhy() const override { return why; }
    void threadInit() override;
    Check checkOnce(const BridgeConfig&, const FirewallGeneration&) override;

private:
    EventLog& m_log;
    FakeClock& m_clock;
};

}  // namespace asb::test
