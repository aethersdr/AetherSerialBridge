#include "core/OperationHost.h"

#include <exception>

namespace asb {

OperationHost::OperationHost(Platform& p, IClock& clock, std::chrono::milliseconds interimPoll)
    : m_platform(p), m_clock(clock), m_applier(p, clock, &m_phases), m_workerContext(new QObject)
{
    m_workerContext->moveToThread(&m_thread);
    m_thread.start();
    m_timer.setInterval(int(interimPoll.count()));
    QObject::connect(&m_timer, &QTimer::timeout, &m_hostContext, [this] { poll(); });
    m_timer.start();
}

OperationHost::~OperationHost()
{
    m_timer.stop();
    m_thread.quit();
    m_thread.wait();   // an outstanding operation finishes first: its event is processed before quit
    delete m_workerContext;
}

bool OperationHost::busy() const
{
    return m_busy;
}

bool OperationHost::submit(std::function<Result(Applier&)> op, IOperationObserver& observer)
{
    if (m_busy) return false;
    m_busy = true;
    const quint64 id = m_nextId++;
    m_current = id;
    m_observer = &observer;
    m_interimOp = 0;
    m_interimPhase = Phase::None;
    QMetaObject::invokeMethod(
        m_workerContext,
        [this, id, operation = std::move(op)] {
            m_phases.beginOperation(id);
            Result r;
            try {
                r = operation(m_applier);
            } catch (const std::exception& e) {
                r.outcome = Outcome::StateUnknown;
                r.message = QStringLiteral("The operation failed unexpectedly: %1").arg(QString::fromLocal8Bit(e.what()));
            }
            // The Applier has released its locks; nothing is outstanding any more. The firewall provider's session is
            // released too, here on the worker that made it (user report 2026-10-16: a long-lived host holds no
            // native session between operations); the next operation connects again.
            if (m_releaseFirewall) {
                try {
                    m_platform.firewall.disconnect();
                } catch (...) {
                }
            }
            // And the lock directory's handle (v1.1 INV-128): nothing of this process stays open in the program folder
            // between operations, so an uninstall can remove it and nothing is held beside the service.
            try {
                m_platform.engineLock.closeIdle();
            } catch (...) {
            }
            // And the data folder's handle (the Applier inspected it; the next operation inspects it again).
            try {
                m_platform.folder.closeIdle();
            } catch (...) {
            }
            m_phases.clear();
            m_workerDone = id;
            QMetaObject::invokeMethod(&m_hostContext, [this, id, r] { deliverFinished(id, r); }, Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    return true;
}

void OperationHost::poll()
{
    if (!m_busy) return;
    const PhaseSnapshot s = m_phases.load();
    if (s.operationId != m_current || s.phase == Phase::None) return;
    if (m_clock.now() <= s.deadline) return;
    if (m_interimOp == s.operationId && m_interimPhase == s.phase) return;   // at most once per phase
    m_interimOp = s.operationId;
    m_interimPhase = s.phase;
    const quint64 id = s.operationId;
    const Phase phase = s.phase;
    const StopEvidence evidence = s.evidence;   // read with the phase: the worker is inside a native call
    QMetaObject::invokeMethod(&m_hostContext, [this, id, phase, evidence] { deliverInterim(id, phase, evidence); },
                              Qt::QueuedConnection);
}

void OperationHost::deliverInterim(quint64 id, Phase phase, StopEvidence evidence)
{
    // Dropped unless it belongs to the current operation and that operation's worker is still inside it.
    if (!m_busy || id != m_current || m_workerDone.load() == id || !m_observer) return;
    m_observer->interim(Interim{phaseName(phase), interimMessage(phase, evidence)});
}

void OperationHost::deliverFinished(quint64 id, const Result& r)
{
    if (id != m_current || !m_busy) return;
    IOperationObserver* observer = m_observer;
    m_observer = nullptr;
    m_busy = false;   // cleared on delivery, so `finished` may submit the next operation
    if (observer) observer->finished(r);
}

bool OperationHost::submitRun(const std::optional<SaveRequest>& req, bool install, bool repairFirst, IOperationObserver& o)
{
    return submit([req, install, repairFirst](Applier& a) { return a.run(req, install, repairFirst); }, o);
}

bool OperationHost::submitStart(IOperationObserver& o, bool acceptOtherBuild)
{
    return submit([acceptOtherBuild](Applier& a) { return a.start(acceptOtherBuild); }, o);
}

bool OperationHost::submitStop(IOperationObserver& o)
{
    return submit([](Applier& a) { return a.stop(); }, o);
}

bool OperationHost::submitSaveAndRemove(const std::optional<SaveRequest>& req, IOperationObserver& o)
{
    return submit([req](Applier& a) { return a.saveAndRemove(req); }, o);
}

bool OperationHost::submitDeactivate(const std::optional<SaveRequest>& req, IOperationObserver& o)
{
    return submit([req](Applier& a) { return a.deactivate(req); }, o);
}

bool OperationHost::submitSaveOnly(const SaveRequest& req, IOperationObserver& o)
{
    return submit([req](Applier& a) { return a.saveOnly(req); }, o);
}

bool OperationHost::submitEdit(std::function<void(BridgeConfig&)> change, IOperationObserver& o)
{
    return submit([edit = std::move(change)](Applier& a) { return a.edit(edit); }, o);
}

bool OperationHost::submitUninstall(UninstallOptions options, IOperationObserver& o)
{
    return submit([options](Applier& a) { return a.uninstall(options); }, o);
}

bool OperationHost::submitDiscardPending(IOperationObserver& o)
{
    return submit([](Applier& a) { return a.discardPending(); }, o);
}

bool OperationHost::submitDiscardPendingIf(const QString& sha256, IOperationObserver& o)
{
    return submit([sha256](Applier& a) { return a.discardPendingIf(sha256); }, o);
}

bool OperationHost::submitRepair(IOperationObserver& o)
{
    return submit([](Applier& a) { return a.repair(); }, o);
}

}  // namespace asb
