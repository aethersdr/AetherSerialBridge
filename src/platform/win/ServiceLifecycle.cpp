#include "platform/win/ServiceLifecycle.h"

#include "platform/win/ServiceExit.h"

namespace asb::win {

ServiceLifecycle::ServiceLifecycle(ServiceStatus& status, IDeadlineWatchdog& killDeadline, IClock& clock, Hooks hooks,
                                   ExitFn exitFn)
    : m_status(status), m_kill(killDeadline), m_clock(clock), m_hooks(std::move(hooks)), m_exit(exitFn)
{
}

void ServiceLifecycle::refuse(int code)
{
    m_status.markStopped();
    // The exit primitive tightens the kill deadline to now + 2 s before it makes the STOPPED report (D59).
    m_exit(m_kill, code, &ServiceExit::reportStopped);
}

bool ServiceLifecycle::started(RunnerStart r, const QString& tokenRefusal)
{
    switch (r) {
    case RunnerStart::Ok:
    case RunnerStart::ConfigError:
        return m_status.running();   // false after a stop request: the queued stop ends the process
    case RunnerStart::StaleFirewall:
        // The firewall watchdog has already been expired by the runner; this is only the backstop. No report.
        faultDie(exitCode(r));
    case RunnerStart::NotActivated:
        // Not a failure (v1.1 station fix): the line first, bounded by the kill deadline, then STOPPED with NO_ERROR
        // (stoppedRow(6)) through the exit primitive. The write may block; the deadline then ends the process.
        m_kill.tighten(m_clock.now() + kSayBudget, exitCode(r));
        if (m_hooks.say) m_hooks.say(notActivatedLine(tokenRefusal));
        break;
    case RunnerStart::IdentityRefused:
    case RunnerStart::Untrusted:
    case RunnerStart::EngineLockBusy:
    case RunnerStart::DataFolderUnknown:
        break;
    }
    refuse(exitCode(r));
    return false;
}

quint32 ServiceLifecycle::control(quint32 control)
{
    switch (control) {
    case svc::kControlStop:
    case svc::kControlShutdown:
    case svc::kControlPreshutdown:
        if (m_stopping.exchange(true)) return svc::kNoError;   // already stopping: nothing changes
        // 1. The non-reporting kill deadline, BEFORE the first potentially blocking stop report (D59).
        m_kill.arm(m_clock.now() + kKillAfterStop, 0);
        // 2. StopPending (checkpoint 1). From here on Running can never be reported.
        m_status.stopPending();
        // 3. The engine stops on its own thread.
        if (m_hooks.postToMain) m_hooks.postToMain([this] { stopOnMain(); });
        return svc::kNoError;
    case svc::kControlInterrogate:
        return svc::kNoError;
    default:
        return svc::kErrorCallNotImplemented;
    }
}

void ServiceLifecycle::stopOnMain()
{
    if (m_hooks.runnerStop) m_hooks.runnerStop([this] { finishStop(); });
    else finishStop();
}

void ServiceLifecycle::finishStop()
{
    if (m_finishing.exchange(true)) return;
    // Flush the log and status, waiting at most 2 s; the exit follows whether or not the worker finished (a
    // blocked worker simply ends with the process).
    if (m_hooks.shutdownSideChannel) m_hooks.shutdownSideChannel(kSideChannelBudget);
    m_status.markStopped();
    if (m_hooks.beforeExit) m_hooks.beforeExit();
    m_exit(m_kill, 0, &ServiceExit::reportStopped);
}

}  // namespace asb::win
