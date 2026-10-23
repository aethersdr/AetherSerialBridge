#include "support/FakeSystemdBus.h"

using asb::posix::BusError;
using asb::posix::SystemdJob;
using asb::posix::SystemdUnitProcess;

namespace asb::test {

QStringList FakeSystemdBus::methods() const
{
    QStringList m;
    for (const Call& c : calls) m << c.method;
    return m;
}

void FakeSystemdBus::record(const QString& method, QStringList args, Ms timeout)
{
    calls << Call{method, std::move(args), timeout, m_installed};
    if (onCall) onCall(method);
    if (failures.contains(method)) throw BusError(failures.value(method), QStringLiteral("%1 failed (fake)").arg(method));
}

void FakeSystemdBus::emitEvent(const Event& e)
{
    if (m_installed) m_queue.push_back(e);   // no receiver connected: no match rule, the signal is lost
}

void FakeSystemdBus::setReceivers(Receivers r)
{
    m_receivers = std::move(r);
    m_installed = true;
    ++receiverInstalls;
}

void FakeSystemdBus::pump(Ms timeout)
{
    pumpTimeouts << timeout;
    // Signals emitted after the replies the caller already has.
    while (!m_afterReply.empty()) {
        emitEvent(m_afterReply.front());
        m_afterReply.pop_front();
    }
    // Running jobs finish after a few pumps.
    for (auto it = m_runningPumps.begin(); it != m_runningPumps.end();) {
        if (--it.value() > 0) {
            ++it;
            continue;
        }
        for (qsizetype i = 0; i < jobs.size(); ++i) {
            if (jobs[i].id != it.key()) continue;
            emitEvent(Event{false, jobs[i].id, jobs[i].jobPath, jobs[i].unit, QStringLiteral("done")});
            if (ownerChangeWhenJobsFinish) emitEvent(Event{true, 0, {}, {}, {}});
            jobs.removeAt(i);
            break;
        }
        it = m_runningPumps.erase(it);
    }
    if (m_queue.empty()) m_clock.advance(timeout);
    while (!m_queue.empty()) {
        const Event e = m_queue.front();
        m_queue.pop_front();
        if (e.owner) {
            if (m_receivers.managerOwnerChanged) m_receivers.managerOwnerChanged();
        } else if (m_receivers.jobRemoved) {
            m_receivers.jobRemoved(e.id, e.job, e.unit, e.result);
        }
    }
}

void FakeSystemdBus::subscribe(Ms t)
{
    record(QStringLiteral("Subscribe"), {}, t);
}

QString FakeSystemdBus::loadUnit(const QString& name, Ms t)
{
    record(QStringLiteral("LoadUnit"), {name}, t);
    if (!units.contains(name)) units.insert(name, Unit{});
    return unitPath(name);
}

QVariant FakeSystemdBus::unitProperty(const QString& path, const QString& iface, const QString& property, Ms t)
{
    record(QStringLiteral("Get"), {path, iface, property}, t);
    const QString name = path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
    const Unit u = units.value(name);
    if (property == QLatin1String("LoadState")) return u.load;
    if (property == QLatin1String("ActiveState")) return u.active;
    if (property == QLatin1String("MainPID")) return QVariant::fromValue(uint(u.mainPid));
    if (property == QLatin1String("ExecMainCode")) return u.execMainCode;
    if (property == QLatin1String("ExecMainStatus")) return u.execMainStatus;
    if (property == QLatin1String("UnitFileState"))
        return u.masked ? QStringLiteral("masked") : (u.enabled ? QStringLiteral("enabled") : QStringLiteral("disabled"));
    throw BusError(QStringLiteral("org.freedesktop.DBus.Error.UnknownProperty"), property);
}

QList<SystemdUnitProcess> FakeSystemdBus::unitProcesses(const QString& name, Ms t)
{
    record(QStringLiteral("GetUnitProcesses"), {name}, t);
    if (!units.contains(name) || units[name].load == QLatin1String("not-found"))
        throw BusError(QStringLiteral("org.freedesktop.systemd1.NoSuchUnit"), QStringLiteral("Unit %1 not loaded.").arg(name));
    return units[name].processes;
}

void FakeSystemdBus::maskUnitFiles(const QStringList& files, bool runtime, bool force, Ms t)
{
    record(QStringLiteral("MaskUnitFiles"), {files.join(QLatin1Char(',')), runtime ? QStringLiteral("runtime") : QStringLiteral("persistent"),
                                             force ? QStringLiteral("force") : QStringLiteral("noforce")}, t);
    for (const QString& f : files) units[f].masked = true;
}

void FakeSystemdBus::unmaskUnitFiles(const QStringList& files, bool runtime, Ms t)
{
    record(QStringLiteral("UnmaskUnitFiles"), {files.join(QLatin1Char(',')), runtime ? QStringLiteral("runtime") : QStringLiteral("persistent")}, t);
    for (const QString& f : files) units[f].masked = false;
}

void FakeSystemdBus::enableUnitFiles(const QStringList& files, bool runtime, bool force, Ms t)
{
    record(QStringLiteral("EnableUnitFiles"), {files.join(QLatin1Char(',')), runtime ? QStringLiteral("runtime") : QStringLiteral("persistent"),
                                               force ? QStringLiteral("force") : QStringLiteral("noforce")}, t);
    for (const QString& f : files) {
        if (units[f].masked) throw BusError(QStringLiteral("org.freedesktop.systemd1.UnitMasked"), QStringLiteral("%1 is masked").arg(f));
        units[f].enabled = true;
    }
}

void FakeSystemdBus::disableUnitFiles(const QStringList& files, bool runtime, Ms t)
{
    record(QStringLiteral("DisableUnitFiles"), {files.join(QLatin1Char(',')), runtime ? QStringLiteral("runtime") : QStringLiteral("persistent")}, t);
    for (const QString& f : files) units[f].enabled = false;
}

void FakeSystemdBus::reload(Ms t)
{
    record(QStringLiteral("Reload"), {}, t);
    for (auto it = units.begin(); it != units.end(); ++it)
        it->load = it->masked ? QStringLiteral("masked") : (unitFileExists(it.key()) ? QStringLiteral("loaded") : QStringLiteral("not-found"));
}

QString FakeSystemdBus::job(const QString& unit, bool start)
{
    const JobPlan plan = plans.isEmpty() ? JobPlan{} : plans.takeFirst();
    const quint32 id = m_nextJob++;
    const QString path = QStringLiteral("/org/freedesktop/systemd1/job/%1").arg(id);
    for (const auto& [otherPath, otherResult] : plan.others) emitEvent(Event{false, id + 1000, otherPath, unit, otherResult});
    if (plan.ownerChange) {
        emitEvent(Event{true, 0, {}, {}, {}});
        return path;
    }
    const Event done{false, id, path, unit, plan.result};
    if (plan.ownerChangeWithCompletion) {
        // Both delivered by the next pump: the completion first, so an adapter that accepts it before
        // looking at the owner change is caught.
        m_afterReply.push_back(done);
        m_afterReply.push_back(Event{true, 0, {}, {}, {}});
    } else if (plan.when == When::BeforeReply) {
        emitEvent(done);
    } else if (plan.when == When::AfterReply) {
        m_afterReply.push_back(done);
    }
    if (plan.when != When::Never && plan.result == QLatin1String("done")) {
        Unit& u = units[unit];
        u.active = start ? QStringLiteral("active") : QStringLiteral("inactive");
        u.mainPid = start ? 4242 : 0;
        if (!start) u.processes.clear();
    }
    return path;
}

QString FakeSystemdBus::startUnit(const QString& name, const QString& mode, Ms t)
{
    record(QStringLiteral("StartUnit"), {name, mode}, t);
    return job(name, true);
}

QString FakeSystemdBus::stopUnit(const QString& name, const QString& mode, Ms t)
{
    record(QStringLiteral("StopUnit"), {name, mode}, t);
    if (!units.contains(name) || units[name].load == QLatin1String("not-found"))
        throw BusError(QStringLiteral("org.freedesktop.systemd1.NoSuchUnit"), QStringLiteral("Unit %1 not loaded.").arg(name));
    return job(name, false);
}

QList<SystemdJob> FakeSystemdBus::listJobs(Ms t)
{
    record(QStringLiteral("ListJobs"), {}, t);
    for (const SystemdJob& j : jobs)
        if (j.state == QLatin1String("running") && !m_runningPumps.contains(j.id)) m_runningPumps.insert(j.id, runningPumps);
    return jobs;
}

void FakeSystemdBus::cancelJob(quint32 id, Ms t)
{
    record(QStringLiteral("CancelJob"), {QString::number(id)}, t);
    for (qsizetype i = 0; i < jobs.size(); ++i) {
        if (jobs[i].id != id) continue;
        emitEvent(Event{false, id, jobs[i].jobPath, jobs[i].unit, QStringLiteral("canceled")});
        jobs.removeAt(i);
        return;
    }
    throw BusError(QStringLiteral("org.freedesktop.systemd1.NoSuchJob"), QStringLiteral("no job %1").arg(id));
}

}  // namespace asb::test
