#pragma once

// Operation lifetime (D57, S21, INV-118): one worker thread per host process runs every Applier
// operation; the host (GUI, TUI, CLI, Linux helper) stays responsive and is told the outcome on ITS
// thread.
//
//  - Ownership: the host process owns the Platform and constructs the OperationHost with it; the
//    OperationHost owns the worker thread and the Applier (not the Platform). Both live until process
//    exit (GUI/TUI) or until after `finished` (CLI/helper), so the worker never outlives them.
//  - submit() captures an immutable request and a fresh operation id; false while busy. Every mutating
//    operation — Save & apply, Start, Stop, untick, Repair, Discard pending, edit-on-disk — goes through
//    this one busy gate (the submit* helpers below).
//  - The Applier publishes {operationId, phase, deadline} in a PhaseClock; a host-thread timer delivers
//    one interim notification per phase when that deadline passes while the worker is still inside the
//    phase. An interim is never a final result, and one whose operation id is not current, or whose
//    worker already returned, is dropped, so no interim ever follows its final.
//  - The worker keeps apply.lock, any engine.lock and every native handle until the native call
//    returns; then failClosed runs on the worker, the locks are released, and `finished` is posted.
//  - busy() stays true until `finished` has been DELIVERED on the host thread, not merely posted.
//  - No cancel (a native call cannot be cancelled) and no process exit: the CLI and the helper exit in
//    their own main after `finished`.

#include "core/Applier.h"
#include "core/Interfaces.h"
#include "core/PhaseClock.h"

#include <QObject>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace asb {

class OperationHost {
public:
    // Constructed on the host thread (the thread whose event loop receives the callbacks).
    explicit OperationHost(Platform&, IClock& clock = systemClock(),
                           std::chrono::milliseconds interimPoll = std::chrono::milliseconds(250));
    ~OperationHost();   // waits for an outstanding operation to return (never abandons the worker)
    OperationHost(const OperationHost&) = delete;
    OperationHost& operator=(const OperationHost&) = delete;

    bool busy() const;
    bool submit(std::function<Result(Applier&)>, IOperationObserver&);

    bool submitRun(const std::optional<SaveRequest>&, bool install, bool repairFirst, IOperationObserver&);
    bool submitStart(IOperationObserver&, bool acceptOtherBuild = false);
    bool submitStop(IOperationObserver&);
    bool submitSaveAndRemove(const std::optional<SaveRequest>&, IOperationObserver&);
    bool submitDeactivate(const std::optional<SaveRequest>&, IOperationObserver&);
    bool submitSaveOnly(const SaveRequest&, IOperationObserver&);
    bool submitUninstall(UninstallOptions, IOperationObserver&);
    bool submitEdit(std::function<void(BridgeConfig&)>, IOperationObserver&);
    bool submitDiscardPending(IOperationObserver&);
    bool submitDiscardPendingIf(const QString& sha256, IOperationObserver&);
    bool submitRepair(IOperationObserver&);

    quint64 currentOperationId() const { return m_current; }
    // The interim check the host-thread timer runs; callable directly (tests run it at a chosen moment).
    void pollForInterim() { poll(); }
    // The worker has returned from the current operation (its final may not have been delivered yet).
    bool workerReturned() const { return m_current != 0 && m_workerDone.load() == m_current; }
    const PhaseClock& phases() const { return m_phases; }
    // Whether the worker releases the firewall provider's session after each operation (default true). False only in
    // test builds' tst_GuiNative bca3030 row, which reproduces the behaviour before the release existed.
    void setReleaseFirewallAfterOperation(bool on) { m_releaseFirewall = on; }

private:
    void poll();
    void deliverInterim(quint64 id, Phase phase, StopEvidence evidence);
    void deliverFinished(quint64 id, const Result& r);

    Platform& m_platform;
    IClock& m_clock;
    PhaseClock m_phases;
    bool m_releaseFirewall = true;
    Applier m_applier;
    QObject m_hostContext;          // lives on the host thread; queued callbacks run here
    QThread m_thread;
    QObject* m_workerContext;       // moved to m_thread
    QTimer m_timer;

    bool m_busy = false;            // host thread only
    quint64 m_current = 0;
    quint64 m_nextId = 1;
    IOperationObserver* m_observer = nullptr;
    std::atomic<quint64> m_workerDone{0};   // id of the last operation whose worker returned
    quint64 m_interimOp = 0;
    Phase m_interimPhase = Phase::None;
};

}  // namespace asb
