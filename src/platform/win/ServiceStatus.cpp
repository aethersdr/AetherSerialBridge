#include "platform/win/ServiceStatus.h"

namespace asb::win {

NativeServiceStatus startPendingRow(quint32 checkPoint)
{
    NativeServiceStatus s;
    s.currentState = svc::kStartPending;
    s.controlsAccepted = 0;
    s.checkPoint = checkPoint;
    s.waitHint = svc::kStartWaitHintMs;
    return s;
}

NativeServiceStatus runningRow()
{
    NativeServiceStatus s;
    s.currentState = svc::kRunning;
    s.controlsAccepted = svc::kAcceptStop | svc::kAcceptShutdown | svc::kAcceptPreshutdown;
    return s;
}

NativeServiceStatus stopPendingRow(quint32 checkPoint)
{
    NativeServiceStatus s;
    s.currentState = svc::kStopPending;
    s.controlsAccepted = 0;
    s.checkPoint = checkPoint;
    s.waitHint = svc::kStopWaitHintMs;
    return s;
}

NativeServiceStatus stoppedRow(int code)
{
    NativeServiceStatus s;
    s.currentState = svc::kStopped;
    // Not activated is a normal stop for SCM: no failure actions (the table, v1.1 station fix).
    if (code != 0 && code != svc::kNotActivatedCode) {
        s.win32ExitCode = svc::kErrorServiceSpecific;
        s.serviceSpecificExitCode = static_cast<quint32>(code);
    }
    return s;
}

bool transitionAllowed(ServiceState from, ServiceState to)
{
    switch (from) {
    case ServiceState::StartPending:
        return true;   // a checkpoint, Running, StopPending or Stopped
    case ServiceState::Running:
        return to == ServiceState::StopPending;
    case ServiceState::StopPending:
        return to == ServiceState::StopPending || to == ServiceState::Stopped;
    case ServiceState::Stopped:
        return false;
    }
    return false;
}

ServiceStatus::ServiceStatus(IServiceStatusReporter& reporter) : m_reporter(reporter) {}

bool ServiceStatus::move(ServiceState to, const NativeServiceStatus& row)
{
    // Called with m_mutex held. The report is made under the mutex, so two orderly reports never cross.
    if (!transitionAllowed(m_state, to)) return false;
    m_state = to;
    m_reporter.report(row);
    return true;
}

bool ServiceStatus::startPending()
{
    std::lock_guard g(m_mutex);
    if (m_stopRequested || m_state != ServiceState::StartPending) return false;
    return move(ServiceState::StartPending, startPendingRow(++m_startCheckPoint));
}

bool ServiceStatus::running()
{
    std::lock_guard g(m_mutex);
    // Never Running after a stop request (INV-60), and only out of StartPending.
    if (m_stopRequested || m_state != ServiceState::StartPending) return false;
    return move(ServiceState::Running, runningRow());
}

bool ServiceStatus::stopPending()
{
    std::lock_guard g(m_mutex);
    if (m_state == ServiceState::Stopped) return false;
    if (m_state != ServiceState::StopPending) m_stopCheckPoint = 0;   // restart at 1 on entry
    m_stopRequested = true;
    return move(ServiceState::StopPending, stopPendingRow(++m_stopCheckPoint));
}

void ServiceStatus::markStopped()
{
    std::lock_guard g(m_mutex);
    m_stopRequested = true;
    m_state = ServiceState::Stopped;
}

ServiceState ServiceStatus::state() const
{
    std::lock_guard g(m_mutex);
    return m_state;
}

bool ServiceStatus::stopRequested() const
{
    std::lock_guard g(m_mutex);
    return m_stopRequested;
}

}  // namespace asb::win
