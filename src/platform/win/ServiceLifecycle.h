#pragma once

// The decisions of the Windows service host (WS-7; master §4.5, D59; INV-60, INV-81, INV-93). Portable: the
// native ServiceMain (ServiceMain.cpp) owns the threads, the dispatcher and the objects, and asks this class what
// to do, so the order of every step is asserted on Linux too (tst_ServiceLifecycle).
//
// Threads: the main thread runs the startup sequence and then the Qt event loop (engine); the dispatcher
// thread (StartServiceCtrlDispatcherW) calls control().
//
//  startup refusal (before Running)   refuse(code) = Exit::now(killDeadline, code, &ServiceExit::reportStopped):
//                                     the kill deadline is tightened BEFORE the STOPPED report (3, 4, 5, 6).
//  started(NotActivated)              not a failure (v1.1 station fix; BridgeRunner.h notActivatedLine): the kill
//                                     deadline is tightened to now + 5 s (code 6), THEN the one line is said
//                                     (bridge.log, else the event log: it may block, the deadline bounds it), then
//                                     refuse(6), whose report is STOPPED with NO_ERROR (stoppedRow(6)), so SCM runs
//                                     no recovery action.
//  started(Ok | ConfigError)          Running (refused once a stop was requested: INV-60).
//  STOP / SHUTDOWN / PRESHUTDOWN      on the dispatcher thread: arm the kill deadline at now + 20 s (code 0),
//                                     THEN report StopPending (the first potentially blocking stop report), then
//                                     queue the stop onto the main thread. A second request changes nothing.
//  the stop on the main thread        runner.stop(done); done = shut the side channel down (wait <= 2 s, whether
//                                     or not it finished), then Exit::now(killDeadline, 0, reportStopped).
// The kill deadline is never extended (arm/tighten only) and nothing else touches it: whatever blocks —
// teardown, a device close, the side channel, SetServiceStatus — the process ends by STOP + 20 s (INV-81).

#include "core/BridgeRunner.h"
#include "core/Exit.h"
#include "core/Interfaces.h"
#include "platform/win/ServiceStatus.h"

#include <atomic>
#include <chrono>
#include <functional>

namespace asb::win {

class ServiceLifecycle {
public:
    static constexpr std::chrono::seconds kKillAfterStop{20};
    static constexpr std::chrono::milliseconds kSideChannelBudget{2000};
    // How long the not-activated line may take before the kill deadline ends the process (faultDie(6), no report).
    // Chosen trade-off: a write that blocks for 5 s ends without the NO_ERROR report, so SCM counts that
    // one start as a failure and restarts it; only a write that keeps blocking brings the loop back. Saying the line
    // after the report would leave it under the exit primitive's 2 s, with TerminateProcess racing the write.
    static constexpr std::chrono::seconds kSayBudget{5};

    // Exit::now in production ([[noreturn]]); tests record the call and return.
    using ExitFn = void (*)(IDeadlineWatchdog& killDeadline, int code, void (*report)(int));

    struct Hooks {
        std::function<void(std::function<void()>)> postToMain;              // queue onto the main thread
        std::function<void(std::function<void()> done)> runnerStop;          // BridgeRunner::stop
        std::function<bool(std::chrono::milliseconds)> shutdownSideChannel;  // SideChannel::shutdown
        std::function<void()> beforeExit;                                    // test seam only (may be empty)
        // The not-activated line (production: bridge.log through the trusted folder, else the Application event
        // log). Empty: nothing is said.
        std::function<void(const QString&)> say;
    };

    ServiceLifecycle(ServiceStatus& status, IDeadlineWatchdog& killDeadline, IClock& clock, Hooks hooks,
                     ExitFn exitFn);

    // Before Running: ends the process through the exit primitive with this code (reported to SCM).
    void refuse(int code);
    // The runner's start result: Running for Ok/ConfigError (true when reported); every refusal ends the process
    // (refuse(exitCode(r))); StaleFirewall ends it through faultDie(7), with no report. NotActivated says
    // notActivatedLine(tokenRefusal) first, under the kill deadline (see above), and is reported as a normal stop.
    bool started(RunnerStart r, const QString& tokenRefusal = {});
    // The control handler's body (dispatcher thread). Returns the handler's Win32 result.
    quint32 control(quint32 control);
    // Main thread: the orderly stop, after the control handler queued it.
    void stopOnMain();
    // Main thread: runner.stop's `done`.
    void finishStop();

    bool stopping() const { return m_stopping.load(); }

private:
    ServiceStatus& m_status;
    IDeadlineWatchdog& m_kill;
    IClock& m_clock;
    Hooks m_hooks;
    ExitFn m_exit;
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_finishing{false};
};

}  // namespace asb::win
