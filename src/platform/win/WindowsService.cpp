#include "platform/win/WindowsService.h"

#include "platform/win/InstallBinary.h"
#include "platform/win/ServiceStatus.h"


namespace asb::win {

namespace {

QString serviceName() { return QString::fromLatin1(ServicePlan::kServiceName); }

[[noreturn]] void fail(const QString& what, quint32 code)
{
    throw PlatformError(QStringLiteral("%1: %2").arg(what, scmErrorText(code)).toStdString());
}

// Closes a service handle on scope exit.
struct Closer {
    ScmSeams& s;
    ScmSeams::Handle h;
    ~Closer()
    {
        if (h) s.close(h);
    }
};

QString joinPath(const QString& dir, const char* name)
{
    QString d = dir;
    if (!d.endsWith(QLatin1Char('\\'))) d += QLatin1Char('\\');
    return d + QString::fromLatin1(name);
}

}  // namespace

QString scmErrorText(quint32 code)
{
    switch (code) {
    case scm::kErrorAccessDenied: return QStringLiteral("access denied (run as administrator)");
    case scm::kErrorServiceDisabled: return QStringLiteral("the service is disabled");
    case scm::kErrorServiceDoesNotExist: return QStringLiteral("the service does not exist");
    case scm::kErrorCannotAcceptControl: return QStringLiteral("the service cannot accept the control now");
    case scm::kErrorInvalidServiceControl: return QStringLiteral("the service does not accept this control now");
    case scm::kErrorServiceNotActive: return QStringLiteral("the service is not running");
    case scm::kErrorServiceExists: return QStringLiteral("the service already exists");
    case scm::kErrorServiceMarkedForDelete: return QStringLiteral("the service is marked for deletion");
    default: return QStringLiteral("Windows error %1").arg(code);
    }
}

WindowsService::WindowsService(ScmSeams& seams, Install install, IClock& clock)
    : m_s(seams), m_install(std::move(install)), m_clock(clock)
{
}

QString WindowsService::installedExe() const { return joinPath(m_install.lockDirectory, InstallBinary::kInstalledName); }

QString WindowsService::programOf(const QString& binPath)
{
    const QString b = binPath.trimmed();
    if (b.startsWith(QLatin1Char('"'))) {
        const qsizetype end = b.indexOf(QLatin1Char('"'), 1);
        return end < 0 ? b.mid(1) : b.mid(1, end - 1);
    }
    // Unquoted: up to the first ".exe" followed by a space or the end (how SCM itself resolves it).
    const qsizetype exe = b.indexOf(QStringLiteral(".exe"), 0, Qt::CaseInsensitive);
    if (exe >= 0) return b.left(exe + 4);
    const qsizetype sp = b.indexOf(QLatin1Char(' '));
    return sp < 0 ? b : b.left(sp);
}

std::optional<ScmSeams::Handle> WindowsService::open(quint32 access, quint32* err)
{
    ScmSeams::Handle h = 0;
    *err = m_s.openService(serviceName(), access, &h);
    if (*err != scm::kOk) return std::nullopt;
    return h;
}

InstallationEvidence::WindowsEvidence WindowsService::evidence()
{
    InstallationEvidence::WindowsEvidence ev;
    ev.expectedQtExe = installedExe();
    // Read with or without a service: no service and the program still there is an unfinished uninstall
    // (Installation::programRemains).
    ev.qtExePresent = m_s.fileExists(installedExe());
    quint32 err = 0;
    const auto h = open(scm::kQueryConfig | scm::kQueryStatus, &err);
    if (!h) {
        if (err == scm::kErrorServiceDoesNotExist) return ev;   // serviceExists = false
        fail(QStringLiteral("could not query the service"), err);
    }
    Closer c{m_s, *h};
    ev.serviceExists = true;
    ScmServiceConfig cfg;
    if (const quint32 e = m_s.queryConfig(*h, &cfg); e != scm::kOk) fail(QStringLiteral("could not read the service configuration"), e);
    ev.binPath = cfg.binaryPath;
    const QString program = programOf(cfg.binaryPath);
    // The Qt service's SCM configuration is judged as installOrConfigure writes it (a foreign service's is not ours
    // to judge, and its recovery settings are never read).
    if (program.compare(installedExe(), Qt::CaseInsensitive) == 0) ev.scmConfigProblem = scmConfigProblem(*h, cfg);
    // The installed Qt exe's digest (INV-41) and the lock files.
    QByteArray whole;
    qint64 wholeSize = 0;
    if (m_s.readFileHead(installedExe(), InstallBinary::kMaxImageBytes, &whole, &wholeSize) == scm::kOk &&
        wholeSize == whole.size()) {
        const QByteArray digest = InstallBinary::sha256Hex(whole);
        ev.installedSha256 = QString::fromLatin1(digest);
        ev.qtExeDigestOk = digest == m_install.expectedSha256.toLower();
    }
    ev.lockFilesPresent = m_s.fileExists(joinPath(m_install.lockDirectory, "engine.lock")) &&
                          m_s.fileExists(joinPath(m_install.lockDirectory, "engine.lock.id"));
    ev.appsEntryPresent = m_life.entry && m_life.entry->present();   // diagnostics only: never classified (D63)
    return ev;
}

QString WindowsService::scmConfigProblem(ScmSeams::Handle h, const ScmServiceConfig& got)
{
    // Every setting installOrConfigure writes (class audit, review round 3). The service type and the error
    // control are written in the same CreateService / ChangeServiceConfig call as the program path, the account
    // and the start type, which are checked: one call, all or nothing. The description is cosmetic and written
    // before the recovery actions, so a half-done install is caught by them. The start type is either value the
    // product sets (Disabled while starts are blocked, Automatic once activated).
    const ServicePlan::ScmConfig want = ServicePlan::windowsConfig(installedExe());
    QStringList bad;
    if (got.binaryPath.compare(want.binaryPath, Qt::CaseInsensitive) != 0) bad << QStringLiteral("program path");
    if (got.account.compare(want.account, Qt::CaseInsensitive) != 0) bad << QStringLiteral("account");
    if (got.displayName != want.displayName) bad << QStringLiteral("display name");
    if (got.startType != scm::kDisabled && got.startType != scm::kAutoStart) bad << QStringLiteral("start type");
    QList<ServicePlan::FailureAction> actions;
    int reset = 0;
    bool flag = false;
    if (const quint32 e = m_s.queryFailureActions(h, &actions, &reset, &flag); e != scm::kOk)
        fail(QStringLiteral("could not read the service's recovery actions"), e);
    if (actions != want.failureActions || reset != want.failureResetSeconds) bad << QStringLiteral("recovery actions");
    if (flag != want.failureActionsOnNonCrashFailures) bad << QStringLiteral("recovery on a non-zero exit");
    return bad.join(QStringLiteral(", "));
}

Installation WindowsService::detect(Deadline) { return InstallationEvidence::classifyWindows(evidence()); }

ServiceQuery WindowsService::query(Deadline)
{
    ServiceQuery q;
    // A process of the installed image may outlive its service: STOPPED is reported before TerminateProcess
    // (Exit::now), a kill deadline may still be running, or the service was deleted under it. pid 0 is a promise
    // that none exists (WS-5 round 3), so a surviving one is reported whatever SCM says.
    // An enumeration that could not be completed is a failed query (failClosed), never "none survives"
    // (review round 1).
    auto survivor = [&]() -> qint64 {
        QList<quint32> pids;
        if (const quint32 e = m_s.processesOfImage(installedExe(), &pids); e != scm::kOk)
            fail(QStringLiteral("could not tell whether an engine process survives"), e);
        return pids.isEmpty() ? 0 : qint64(pids.front());
    };
    quint32 err = 0;
    const auto h = open(scm::kQueryStatus, &err);
    if (!h) {
        if (err == scm::kErrorServiceDoesNotExist) {   // Absent
            q.pid = survivor();
            return q;
        }
        fail(QStringLiteral("could not query the service"), err);
    }
    Closer c{m_s, *h};
    ScmStatus st;
    if (const quint32 e = m_s.queryStatus(*h, &st); e != scm::kOk) fail(QStringLiteral("could not query the service"), e);
    switch (st.state) {
    case svc::kStopped: q.state = ServiceQuery::Stopped; break;
    case svc::kStartPending: q.state = ServiceQuery::StartPending; break;
    case svc::kRunning: q.state = ServiceQuery::Running; break;
    case svc::kStopPending: q.state = ServiceQuery::StopPending; break;
    default: q.state = ServiceQuery::Other; break;
    }
    q.pid = st.processId != 0 ? qint64(st.processId) : survivor();
    // How the last run ended (the ACK-timeout result names it; user report 2026-10-16).
    if (st.state == svc::kStopped) {
        if (st.win32ExitCode == scm::kErrorServiceSpecificError) q.exitCode = int(st.serviceSpecificExitCode);
        else if (st.win32ExitCode == scm::kErrorProcessAborted) q.endedUnreported = true;
    }
    return q;
}

void WindowsService::onTime(Deadline deadline, const char* before) const
{
    if (m_clock.now() > deadline) throw PlatformError(std::string("the deadline passed before ") + before);
}

void WindowsService::applyStartType(bool automatic, Deadline deadline)
{
    const QList<ServicePlan::Step> steps =
        automatic ? ServicePlan::allowStarts(ServicePlan::Os::Windows) : ServicePlan::blockStarts(ServicePlan::Os::Windows);
    quint32 err = 0;
    const auto h = open(scm::kChangeConfig, &err);
    if (!h) {
        if (err == scm::kErrorServiceDoesNotExist && !automatic) return;   // nothing to block
        fail(QStringLiteral("could not open the service"), err);
    }
    Closer c{m_s, *h};
    for (const ServicePlan::Step& s : steps) {
        // Allowing starts is a forward step: never after the deadline (the open or the previous call may have
        // returned late). Blocking them completes whatever the time.
        if (automatic) onTime(deadline, "allowing starts");
        switch (s.kind) {
        case ServicePlan::Step::Kind::SetStartTypeDisabled:
            if (const quint32 e = m_s.changeStartType(*h, scm::kDisabled); e != scm::kOk)
                fail(QStringLiteral("could not disable the service"), e);
            break;
        case ServicePlan::Step::Kind::SetStartTypeAutomatic:
            if (const quint32 e = m_s.changeStartType(*h, scm::kAutoStart); e != scm::kOk)
                fail(QStringLiteral("could not enable the service"), e);
            break;
        case ServicePlan::Step::Kind::FlushServiceKey:
            // INV-77: the start type is durable before anything else happens.
            if (const quint32 e = m_s.flushServiceKey(serviceName()); e != scm::kOk)
                fail(QStringLiteral("could not flush the service key"), e);
            break;
        default:
            break;
        }
    }
}

void WindowsService::inhibitStarts(Deadline deadline) { applyStartType(false, deadline); }

void WindowsService::allowStarts(Deadline deadline) { applyStartType(true, deadline); }

void WindowsService::requestStop(Deadline deadline)
{
    quint32 err = 0;
    const auto h = open(scm::kStop | scm::kQueryStatus, &err);
    if (!h) {
        if (err == scm::kErrorServiceDoesNotExist) return;
        fail(QStringLiteral("could not open the service"), err);
    }
    Closer c{m_s, *h};
    for (;;) {
        const quint32 e = m_s.control(*h, svc::kControlStop);
        if (e == scm::kOk || e == scm::kErrorServiceNotActive) return;
        // A service that accepts no control yet answers ERROR_INVALID_SERVICE_CONTROL (START_PENDING, measured on
        // windows-latest) or ERROR_SERVICE_CANNOT_ACCEPT_CTRL; STOP_PENDING is already stopping.
        if (e != scm::kErrorCannotAcceptControl && e != scm::kErrorInvalidServiceControl)
            fail(QStringLiteral("could not request the stop"), e);
        ScmStatus st;
        if (m_s.queryStatus(*h, &st) == scm::kOk && (st.state == svc::kStopPending || st.state == svc::kStopped)) return;
        if (m_clock.now() >= deadline) return;   // the barrier's engine-lock wait decides
        m_clock.sleepFor(std::chrono::milliseconds(250));
    }
}

void WindowsService::installOrConfigure(InstallState, Deadline deadline)
{
    // 1. The install copy, verified by the hash of the copy (INV-41).
    if (!m_install.io || !m_install.dir || !m_install.image) throw PlatformError("the install source is not configured");
    QString why;
    const std::optional<QByteArray> image = m_install.image(&why);
    if (!image) throw PlatformError(QStringLiteral("could not read the program to install: %1").arg(why).toStdString());
    onTime(deadline, "writing the install copy");
    const InstallBinary::Result r = InstallBinary::install(*m_install.io, m_install.dir(),
                                                           QString::fromLatin1(InstallBinary::kInstalledName), *image,
                                                           m_install.expectedSha256, {},
                                                           [&] { onTime(deadline, "publishing the install copy"); });
    if (!r.ok) throw PlatformError(r.error.toStdString());
    m_installedBytes = image->size();
    if (!r.durable)
        // The copy replaced the installed program, but the folder flush that makes the replacement survive a power
        // loss failed (INV-77). Stop before any SCM change, and say what was replaced (the INV-76 shape; review
        // round 2): a later operation must not run a service whose program may roll back.
        throw PlatformErrorAfterChanges(QStringLiteral("replaced %1, but the replacement is not durable (%2); the service was "
                                                       "not changed: run the install again")
                                            .arg(installedExe(), r.error)
                                            .toStdString(),
                                        {QStringLiteral("replaced %1").arg(installedExe())});

    // 2. The service, created or reconfigured with starts BLOCKED.
    const ServicePlan::ScmConfig cfg = ServicePlan::windowsConfig(installedExe());
    ScmSeams::Handle h = 0;
    bool created = false;
    onTime(deadline, "opening the service");
    quint32 e = m_s.openService(cfg.serviceName, scm::kAllAccess, &h);
    if (e == scm::kErrorServiceDoesNotExist) {
        onTime(deadline, "creating the service");
        e = m_s.createService(cfg.serviceName, cfg.displayName, cfg.binaryPath, scm::kDisabled, cfg.account, &h);
        if (e != scm::kOk) fail(QStringLiteral("could not create the service"), e);
        created = true;
    } else if (e != scm::kOk) {
        fail(QStringLiteral("could not open the service"), e);
    }
    Closer c{m_s, h};
    if (!created) {
        onTime(deadline, "configuring the service");
        if ((e = m_s.changeConfig(h, scm::kDisabled, cfg.binaryPath, cfg.displayName, cfg.account)) != scm::kOk)
            fail(QStringLiteral("could not configure the service"), e);
    }
    onTime(deadline, "setting the description");
    if ((e = m_s.setDescription(h, cfg.description)) != scm::kOk) fail(QStringLiteral("could not set the description"), e);
    onTime(deadline, "setting the recovery actions");
    if ((e = m_s.setFailureActions(h, cfg.failureActions, cfg.failureResetSeconds)) != scm::kOk)
        fail(QStringLiteral("could not set the recovery actions"), e);
    onTime(deadline, "setting the recovery flag");
    if ((e = m_s.setFailureFlag(h, cfg.failureActionsOnNonCrashFailures)) != scm::kOk)
        fail(QStringLiteral("could not set the recovery flag"), e);
    onTime(deadline, "flushing the service key");
    if ((e = m_s.flushServiceKey(cfg.serviceName)) != scm::kOk) fail(QStringLiteral("could not flush the service key"), e);
    removeLeftoverImages();
}

void WindowsService::removeLeftoverImages()
{
    // An earlier uninstall run from the installed exe renamed it aside and scheduled it (WS-16). Whatever of that can be
    // deleted now goes; one still in use stays for the restart. Only that name pattern, nothing else in the folder.
    if (!m_install.io || !m_install.dir || !m_life.children) return;
    const QString prefix = QString::fromLatin1(InstallBinary::kInstalledName) + QString::fromLatin1(kRemovedMarker);
    for (const QString& n : m_life.children()) {
        if (!n.startsWith(prefix, Qt::CaseInsensitive)) continue;
        QString err;
        if (m_install.io->remove(m_install.dir(), n, &err)) m_changes << QStringLiteral("removed %1").arg(joinPath(m_install.lockDirectory, n.toLatin1().constData()));
    }
}

void WindowsService::start(Deadline deadline)
{
    quint32 err = 0;
    const auto h = open(scm::kStart, &err);
    if (!h) fail(QStringLiteral("could not open the service"), err);
    Closer c{m_s, *h};
    onTime(deadline, "starting the service");
    const quint32 e = m_s.start(*h);
    if (e != scm::kOk && e != scm::kErrorServiceAlreadyRunning) fail(QStringLiteral("could not start the service"), e);
}

void WindowsService::uninstall(Deadline deadline)
{
    quint32 err = 0;
    const auto h = open(scm::kDelete | scm::kQueryStatus, &err);
    if (!h) {
        if (err == scm::kErrorServiceDoesNotExist) return;
        fail(QStringLiteral("could not open the service"), err);
    }
    Closer c{m_s, *h};
    onTime(deadline, "removing the service");
    const quint32 e = m_s.remove(*h);
    if (e != scm::kOk && e != scm::kErrorServiceMarkedForDelete) fail(QStringLiteral("could not remove the service"), e);
}

IServiceControl::ProgramRemoval WindowsService::removeProgram(Deadline deadline)
{
    ProgramRemoval r;
    const QString exe = installedExe();
    if (!m_install.io || !m_install.dir) {
        r.kind = ProgramRemoval::Kind::Kept;
        r.error = QStringLiteral("the program folder is not configured");
        r.changes << QStringLiteral("kept %1 (%2)").arg(exe, r.error);
        return r;
    }
    onTime(deadline, "removing the program");
    if (m_life.injectedRemovalFailure) {
        if (const QString injected = m_life.injectedRemovalFailure(); !injected.isEmpty()) {
            r.kind = ProgramRemoval::Kind::Kept;
            r.error = injected;
            r.changes << QStringLiteral("kept %1 (%2)").arg(exe, r.error);
            return r;
        }
    }
    // This process runs from it (Apps & features, or --uninstall typed in the program folder): a running image cannot
    // be deleted, but it can be renamed; the new name is deleted at the next restart (D64 decision 8).
    if (m_life.isOwnImage && m_life.isOwnImage(exe)) {
        const QString suffix = m_life.randomSuffix ? m_life.randomSuffix() : QStringLiteral("0000000000000000");
        const QString aside = exe + QString::fromLatin1(kRemovedMarker) + suffix;
        QString err;
        if (!m_life.rename || !m_life.rename(exe, aside, &err)) {
            r.kind = ProgramRemoval::Kind::Kept;
            r.error = err.isEmpty() ? QStringLiteral("it is running and cannot be renamed") : err;
            r.changes << QStringLiteral("kept %1 (%2)").arg(exe, r.error);
            return r;
        }
        // Scheduled only once Windows has the deletion registered: otherwise the renamed image would
        // outlive the restart while the result promised it gone and the entry was removed.
        if (m_life.deleteAtRestart && m_life.deleteAtRestart(aside, &err)) {
            m_scheduledImage = aside;
            r.kind = ProgramRemoval::Kind::ScheduledAtRestart;
            r.changes << QStringLiteral("renamed %1 to %2; it is deleted when Windows restarts").arg(exe, aside);
            return r;
        }
        const QString why = err.isEmpty() ? QStringLiteral("no way to schedule it") : err;
        r.kind = ProgramRemoval::Kind::Kept;   // the lock files and the Apps & features entry stay with it
        QString back;
        if (m_life.rename(aside, exe, &back)) {
            r.error = QStringLiteral("it could not be scheduled for deletion when Windows restarts (%1)").arg(why);
            r.changes << QStringLiteral("kept %1 (%2)").arg(exe, r.error);
            return r;
        }
        // Neither scheduled nor back under its own name: say exactly where it is and what to do with it.
        r.error = QStringLiteral("it was renamed to %1 and could neither be scheduled for deletion when Windows restarts "
                                 "(%2) nor renamed back (%3): rename it back to %4 and uninstall again, or delete it "
                                 "after the next restart")
                      .arg(aside, why, back.isEmpty() ? QStringLiteral("unknown error") : back,
                           QString::fromLatin1(InstallBinary::kInstalledName));
        r.changes << QStringLiteral("kept %1 (%2)").arg(aside, r.error);
        return r;
    }
    QString err;
    if (m_install.io->remove(m_install.dir(), QString::fromLatin1(InstallBinary::kInstalledName), &err)) {
        r.kind = ProgramRemoval::Kind::Removed;
        r.changes << QStringLiteral("removed %1").arg(exe);
        return r;
    }
    if (err == fileNotFoundError()) return r;   // Absent
    r.kind = ProgramRemoval::Kind::Kept;
    r.error = err;
    r.changes << QStringLiteral("kept %1 (%2)").arg(exe, err);
    return r;
}

QStringList WindowsService::removeProgramFolder()
{
    const QString folder = m_install.lockDirectory;
    QStringList left = m_life.children ? m_life.children() : QStringList();
    const QString scheduledName = m_scheduledImage.mid(m_scheduledImage.lastIndexOf(QLatin1Char('\\')) + 1);
    if (!scheduledName.isEmpty()) left.removeAll(scheduledName);
    if (!left.isEmpty()) return {QStringLiteral("kept %1 (it still holds %2)").arg(folder, left.join(QStringLiteral(", ")))};
    QString err;
    if (!scheduledName.isEmpty()) {
        // Only the renamed running image is left: the folder goes at the restart, after it (registered in this order).
        if (m_life.deleteAtRestart && m_life.deleteAtRestart(folder, &err))
            return {QStringLiteral("%1 is removed when Windows restarts").arg(folder)};
        return {QStringLiteral("kept %1 (%2)").arg(folder, err)};
    }
    if (m_life.removeDirectory && m_life.removeDirectory(folder, &err))
        return {QStringLiteral("removed %1").arg(folder)};
    if (err == fileNotFoundError()) return {};
    return {QStringLiteral("kept %1 (%2)").arg(folder, err.isEmpty() ? QStringLiteral("cannot remove it") : err)};
}

QString WindowsService::writeAppsEntry()
{
    if (!m_life.entry) return QStringLiteral("this build writes no entry");
    IUninstallEntry::Facts f;
    f.installLocation = m_install.lockDirectory;
    f.installedExe = installedExe();
    f.version = m_life.version;
    f.exeBytes = m_installedBytes;
    f.date = m_life.today ? m_life.today() : QDate::currentDate();
    return m_life.entry->write(f);
}

void WindowsService::removeAppsEntry(Deadline)
{
    if (!m_life.entry) return;
    if (const QString e = m_life.entry->remove(); !e.isEmpty())
        throw PlatformError(QStringLiteral("could not remove the Apps & features entry: %1").arg(e).toStdString());
}

QStringList WindowsService::takeChanges()
{
    QStringList r;
    r.swap(m_changes);
    return r;
}

}  // namespace asb::win
