#include "core/PhaseClock.h"

namespace asb {

QString phaseName(Phase p)
{
    switch (p) {
    case Phase::None: return QStringLiteral("none");
    case Phase::Lock: return QStringLiteral("waiting for the apply lock");
    case Phase::Prologue: return QStringLiteral("checking the installation");
    case Phase::Barrier: return QStringLiteral("stopping the bridge");
    case Phase::Install: return QStringLiteral("installing the service");
    case Phase::Stage: return QStringLiteral("saving the settings");
    case Phase::Firewall: return QStringLiteral("updating the firewall");
    case Phase::Commit: return QStringLiteral("committing the settings");
    case Phase::Activate: return QStringLiteral("starting the bridge");
    case Phase::Ack: return QStringLiteral("waiting for the bridge to confirm");
    case Phase::Cleanup: return QStringLiteral("stopping the bridge after a failure");
    case Phase::Folder: return QStringLiteral("repairing the data folder");
    case Phase::Remove: return QStringLiteral("removing the service");
    }
    return {};
}

QString interimMessage(Phase p, StopEvidence e)
{
    const QString bridge = engineClaim(e, EngineClaim::Interim);
    switch (p) {
    case Phase::Firewall:
        return QStringLiteral("Windows is still processing a firewall change; %1.").arg(bridge);
    case Phase::Barrier:
    case Phase::Install:
    case Phase::Activate:
    case Phase::Remove:
    case Phase::Cleanup:
        return QStringLiteral("The service manager is still processing a request (%1); %2.").arg(phaseName(p), bridge);
    default:
        return QStringLiteral("Still %1; %2.").arg(phaseName(p), bridge);
    }
}

void PhaseClock::beginOperation(quint64 operationId)
{
    std::lock_guard g(m_m);
    m_s = PhaseSnapshot{operationId, Phase::None, {}, StopEvidence::None};
}

void PhaseClock::enter(Phase p, Deadline d)
{
    std::lock_guard g(m_m);
    m_s.phase = p;
    m_s.deadline = d;
}

void PhaseClock::setEvidence(StopEvidence e)
{
    std::lock_guard g(m_m);
    m_s.evidence = e;
}

void PhaseClock::clear()
{
    std::lock_guard g(m_m);
    m_s.phase = Phase::None;
    m_s.deadline = {};
    m_s.evidence = StopEvidence::None;
}

PhaseSnapshot PhaseClock::load() const
{
    std::lock_guard g(m_m);
    return m_s;
}

}  // namespace asb
