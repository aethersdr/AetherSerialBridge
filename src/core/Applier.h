#pragma once

// The Applier: the one guarded path for every change to the config, the service or the firewall
// (the master §4.6 transaction contract). Its operations:
// every one holds apply.lock across all of its steps (INV-24), runs the prologue P1-P6 and reports a
// Result whose `persisted` says exactly what of the submitted revision is durably on disk (INV-106).
//
// Run R1-R10 (INV-25): installed? Barrier (B0 token revoked, B1 starts blocked, B2 stop requested, B3
// engine.lock held) -> install -> stage N to pending (durable) -> temp T with N, flushed, read back as
// B, validated -> firewall synced from B with a fresh generation -> rename T over config.json (the
// commit, INV-73) -> delete pending -> sweep other generations, write a fresh start.token, release
// engine.lock, allow starts, start -> ACK: status with that token, the service's pid, sha256(B) and no
// configError (INV-109).
//
// Every native call runs on the calling thread (the OperationHost worker). A call that returns after
// its phase deadline ends the operation: failClosed runs, then the result is StateUnknown with that
// phase (D57, INV-118). Nothing here exits the process or creates one.

#include "core/BridgeConfig.h"
#include "core/Interfaces.h"
#include "core/PhaseClock.h"

#include <chrono>
#include <functional>
#include <optional>

namespace asb {

// --uninstall, the window's Uninstall…, the Apps & features entry and the Linux staged helper (WS-16, INV-125).
struct UninstallOptions {
    bool removeSettings = false;   // also the known files of a trusted data folder (never anything else)
};

class Applier {
public:
    explicit Applier(Platform&, IClock& clock = systemClock(), PhaseClock* phases = nullptr);

    // Phase deadlines (master §4.6 "Deadlines and exclusion"). Files: staging, temp, commit, token.
    static constexpr std::chrono::seconds kApplyLockTimeout{120};
    static constexpr std::chrono::seconds kBarrier{30};
    static constexpr std::chrono::seconds kInstall{60};
    static constexpr std::chrono::seconds kFirewall{60};
    static constexpr std::chrono::seconds kFiles{30};
    static constexpr std::chrono::seconds kStart{15};
    static constexpr std::chrono::seconds kAck{15};
    static constexpr std::chrono::seconds kCleanup{10};
    static constexpr std::chrono::milliseconds kAckPoll{250};

    // Save & apply, --install, --apply (no request: the pending file if present, else the committed
    // config). `repairFirst` = --force-recreate: the folder is opened in Repair mode under the same lock.
    Result run(const std::optional<SaveRequest>&, bool install, bool repairFirst = false);
    // Start button, --start: re-sync the firewall from the committed bytes, then start (INV-29).
    // `acceptOtherBuild`: a complete installation of another build (Installation::otherBuild) is started as it is.
    Result start(bool acceptOtherBuild = false);
    // Stop button, --stop: token revoked, stop requested, engine.lock acquired; start type kept.
    Result stop();
    // Windows untick: pending first, Barrier, firewall rules removed, then the service removed (INV-91).
    Result saveAndRemove(const std::optional<SaveRequest>&);
    // Linux untick: pending first, Barrier (the unit stays masked).
    Result deactivate(const std::optional<SaveRequest>&);
    // The window's Save with nothing installed (v1.1 fix after WS-17): Deactivate's pending write, only
    // while nothing is installed. An installation found under apply.lock (installed since the window read it) is
    // refused NothingChanged before anything is written: no pending file, no barrier, no service or firewall call.
    // The CLI has no route to it (its untick is SaveAndRemove / Deactivate, unchanged).
    Result saveOnly(const SaveRequest&);
    // Uninstall (WS-16; INV-125, INV-126, D62, D69), all under apply.lock: (1) the barrier, the firewall rules, the
    // service (Linux: with its program, policy and staging root); (2) Windows: the held lock files marked for deletion,
    // then the program (or its running image renamed aside and scheduled), then engine.lock.id; (3) with
    // `removeSettings`, the known files of a trusted data folder, then the folder if empty; (4) Windows: the Apps &
    // features entry, last, only once the program it names is gone. After the locks are released, the program folder if
    // it is empty. Ok when the service and the program are gone; every removal and everything kept is in fileChanges.
    Result uninstall(UninstallOptions);
    // CLI edits: pending (or committed) loaded, changed, validated, written to pending; service untouched. A change
    // that throws (the editor refusing it) is NothingChanged with its what(): nothing is written (WS-9).
    Result edit(const std::function<void(BridgeConfig&)>&);
    // Deletes config.pending.json by handle in the trusted folder (the CLI's --discard-pending: it shows nothing first).
    Result discardPending();
    // The window's and the TUI's Discard (INV-127): under apply.lock, config.pending.json is deleted only when the
    // SHA-256 of its bytes is `sha256` (lower-case hex), the digest of the pending settings the caller showed. Missing,
    // or replaced since: NothingChanged, nothing deleted.
    Result discardPendingIf(const QString& sha256);
    // Barrier (if installed), then the folder in Repair mode (normalise, or rename aside + create),
    // lock-pair recovery; the service is left with starts blocked.
    Result repair();

private:
    Platform& m_p;
    IClock& m_clock;
    PhaseClock* m_phases;
};

}  // namespace asb
