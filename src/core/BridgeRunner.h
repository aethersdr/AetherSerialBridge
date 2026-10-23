#pragma once

// The managed engine's body (`--service`; S15, S18, D52, D56). start():
//   1. engine.lock, exclusive and non-blocking; then its lock id (read through the held handle) against
//      engine.lock.id — busy, missing, insecure or a different id -> exit 4 (INV-103, INV-116)
//   2. Service-mode trust of the data folder — anything but Trust -> exit 3, nothing written (INV-22)
//   3. start.token — missing, malformed, revoked, ... -> exit 6, with zero config reads (INV-113)
//   4. config.json through the trusted folder (bytes B; never config.pending.json, INV-43); validate —
//      invalid -> zero ports and `configError` in status (INV-34)
//   5. [firewall monitor enabled] arm the watchdog at now + 5 s (code 7), start the monitor thread, wait
//      for the startup check; a match lets the engine start only if that check began <= 1 s ago (else
//      re-check, three stale results -> exit 7); a mismatch or failure -> expireNow (INV-119, D56). The
//      5 s wait and the 35 s a good check grants are running time, so a suspend never ends the process (N9-1)
//   6. engine.start; status every second through the side channel, carrying pid, instanceId,
//      startToken, startedAt, configSha256 = sha256(B), configError, ports. A failing status write never
//      stops bridging (INV-8).
// The engine-lock hold lives until the process dies; stop() never releases it. The runner starts the
// monitor thread and never joins it (process exit ends it). The monitor never logs and never touches
// the side channel or the event loop. Production never destroys a runner: it lives until process exit.

#include "core/AppLocations.h"
#include "core/IBridgeEngine.h"
#include "core/Interfaces.h"
#include "core/Exit.h"
#include "core/RunningTime.h"

#include <QByteArray>
#include <QObject>
#include <QTimer>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

namespace asb {

enum class RunnerStart {
    Ok,
    IdentityRefused,   // exit 2 (the host's own identity check; never returned by start())
    Untrusted,         // exit 3
    EngineLockBusy,    // exit 4: busy, missing, insecure, unsupported or wrong lock identity
    NotActivated,      // exit 6: no valid start token. Not a failure: the host reports a normal stop (see below)
    StaleFirewall,     // exit 7: startup check failed, overdue, or stale three times
    ConfigError,       // runs zero ports; `configError` published
    DataFolderUnknown, // exit 3: the Windows ProgramData folder cannot be determined (serviceLocations)
};
// The process exit code for a start() result that ends the process (0 for Ok / ConfigError).
int exitCode(RunnerStart);

// v1.1 station fix (a stopped service restarted every 30 s for hours): a start that finds no valid start token is
// not a failure. The token is revoked by Stop (which keeps the start type, so a boot, a recovery restart already
// scheduled, or a manual start can still run the process) and by an apply before it blocks starts; only a Start or
// Save & apply writes a new one, so no restart by the service manager can ever succeed. The process still exits 6,
// but Windows reports SERVICE_STOPPED with NO_ERROR (stoppedRow(6)) and the systemd unit counts 6 as a clean exit
// (SuccessExitStatus=6), so neither runs its failure actions. This is the one line the host says first; `why` is
// the start token's own reason (BridgeRunner::tokenRefusal()).
QString notActivatedLine(const QString& why);

// The service's (and the CLI's) file locations, resolved before anything else at startup. When
// AppLocations cannot determine the Windows ProgramData folder (nullopt), startup is refused:
// failure = DataFolderUnknown (exit 3) with error = kWindowsDataFolderError. There is no fallback: no guessed
// folder, no %ProgramData% from the environment, no current directory (WS-3 AppLocations.h "FAILURE").
struct ServiceLocations {
    std::optional<AppLocations> locations;
    RunnerStart failure = RunnerStart::Ok;
    QString error;
};
ServiceLocations serviceLocations(bool windows, const std::function<QString()>& programData);
ServiceLocations serviceLocations();   // this OS, the real lookup

class BridgeRunner {
public:
    // Creates a thread running the function (production: a detached std::thread, never joined).
    using Spawn = std::function<void(std::function<void()>)>;
    // Blocks the monitor thread until `deadline`; false = give up (test teardown only).
    using SleepUntil = std::function<bool(Deadline)>;
    // The reason the process is about to exit 7, said once just before the runner or the monitor ends the process
    // (v1.1 station fix: exit 7 named nothing). Called on the runner's thread (startup) or the monitor thread
    // (periodic), always while a watchdog deadline at most 5 s away is armed (the check's start + 5 s; on the
    // three-stale path, tightened to now + 5 s first), so a call that blocks cannot delay the exit past it. Production writes bridge.log, else the Application event log.
    using LastWords = std::function<void(const QString&)>;

    static constexpr std::chrono::seconds kCheckBudget{5};      // per check, enforced by the watchdog
    static constexpr std::chrono::seconds kCheckPeriod{30};
    // Listeners only within 1 s of a good check's END (v1.1 station fix): the time the check itself takes is not a
    // pause between check and listen. A check that took over 1 s (a station: about 1.3 s, 617 active rules) used to be
    // stale every time: three re-checks, then exit 7. The exposure bound is still the deadline, from the check's START.
    static constexpr std::chrono::seconds kFreshness{1};
    static constexpr int kStaleLimit = 3;
    static constexpr int kExitStaleFirewall = 7;

    // `clock` is steady time (it includes a suspend); `running` is running time (it does not, N9-1). A budget
    // whose expiry ends the process is measured in running time: the runner's 5 s wait for a startup check
    // and the 35 s that a good check grants. Freshness (1 s) and the 30 s schedule stay on `clock`: a suspend
    // there only makes the runner check again sooner, never exit. An empty `running.running` is
    // RunningTime::now; an empty `running.steady` is `clock.now`. Tests pass both, from one fake.
    BridgeRunner(IEngineLock&, IStartToken&, IDataFolder&, IBridgeEngine&, ISideChannel&, IFirewallMonitor&,
                 IDeadlineWatchdog&, QByteArray instanceId, IClock& clock = systemClock(), Spawn spawn = {},
                 SleepUntil sleepUntil = {}, RunningTime::Clocks running = {});
    ~BridgeRunner();

    RunnerStart start();
    // Why start() returned NotActivated (the start token's reason: missing, revoked, malformed, ...); empty otherwise.
    const QString& tokenRefusal() const { return m_tokenRefusal; }
    // Before start(). Empty (the default): nothing is said.
    void setLastWords(LastWords f) { m_shared->lastWords = std::move(f); }
    // engine.stop(done-chain), then a final status publish, then `done`; the host then calls Exit::now.
    void stop(std::function<void()> done);

    // The report the next publish would carry; publishNow() publishes it (also every second).
    StatusReport report() const;
    void publishNow();

private:
    struct Shared;
    static void monitorMain(const std::shared_ptr<Shared>&);
    bool startupChecks();   // on the runner's thread: drives the monitor thread's startup checks

    IEngineLock& m_lock;
    IStartToken& m_token;
    IDataFolder& m_folder;
    IBridgeEngine& m_engine;
    ISideChannel& m_side;
    IFirewallMonitor& m_monitor;
    IDeadlineWatchdog& m_watchdog;
    QByteArray m_instanceId;
    IClock& m_clock;
    RunningTime::Clocks m_running;
    Spawn m_spawn;
    SleepUntil m_sleepUntil;

    std::unique_ptr<LockHold> m_engineHold;   // until process death
    BridgeConfig m_config;
    FirewallGeneration m_generation;
    QByteArray m_startToken;
    QString m_tokenRefusal;
    QString m_configSha256;
    std::optional<QString> m_configError;
    QDateTime m_startedAt;
    QObject m_context;
    QTimer m_statusTimer;

    // Everything the monitor thread uses lives here, so a monitor thread that outlives the runner object
    // (it is never joined) never touches the runner's memory: the handshake, plus copies of the config,
    // the generation and the sleep function, made before the thread is created. The three seams it calls
    // (monitor, watchdog, clock) are the host's objects, not the runner's: the host must keep them alive
    // until process exit, which production does (they are never destroyed).
    struct Shared {
        // Set once before the thread is spawned, then only read.
        IFirewallMonitor* monitor = nullptr;
        IDeadlineWatchdog* watchdog = nullptr;
        IClock* clock = nullptr;
        RunningTime::Clocks running;   // a copy; its functions use only the host's clock
        SleepUntil sleepUntil;
        BridgeConfig config;
        FirewallGeneration generation;
        LastWords lastWords;
        // The handshake, under `m`.
        std::mutex m;
        std::condition_variable cv;
        int requested = 0;       // startup checks requested
        int done = 0;            // startup checks finished
        Deadline lastStart{};
        Deadline lastEnd{};      // when that check returned (steady): freshness is measured from here
        QString lastWhy;         // the monitor's reason for that result (empty on a match)
        IFirewallMonitor::Check last = IFirewallMonitor::Check::Failed;
        bool periodic = false;   // the runner accepted a result: loop every 30 s
        bool abandoned = false;  // the runner gave up (stale) or was destroyed: the thread ends
    };
    std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
};

}  // namespace asb
