#include "support/Fakes.h"

#include "core/AtomicJson.h"
#include "core/ConfigFiles.h"
#include "core/ConfigJson.h"
#include "core/DataDirectoryTrust.h"
#include "core/FirewallPlan.h"
#include "core/LockIdentity.h"
#include "core/StartToken.h"

#include <QDateTime>
#include <QRegularExpression>

namespace asb::test {

// ---------------------------------------------------------------------------------------------------
void EventLog::add(const QString& e)
{
    const int phase = phaseOf ? phaseOf() : -1;
    {
        std::lock_guard g(m_m);
        m_events << e;
        m_phases << phase;
    }
    if (onAdd) onAdd(e);
}

int EventLog::phaseAt(int i) const
{
    std::lock_guard g(m_m);
    return i >= 0 && i < m_phases.size() ? m_phases[i] : -1;
}

QStringList EventLog::all() const
{
    std::lock_guard g(m_m);
    return m_events;
}

void EventLog::clear()
{
    std::lock_guard g(m_m);
    m_events.clear();
    m_phases.clear();
}

int EventLog::indexOf(const QString& prefix, int from) const
{
    std::lock_guard g(m_m);
    for (int i = qMax(0, from); i < m_events.size(); ++i)
        if (m_events[i].startsWith(prefix)) return i;
    return -1;
}

int EventLog::count(const QString& prefix) const
{
    std::lock_guard g(m_m);
    int n = 0;
    for (const QString& e : m_events)
        if (e.startsWith(prefix)) ++n;
    return n;
}

// ---------------------------------------------------------------------------------------------------
Deadline FakeClock::now() const
{
    return Deadline(std::chrono::steady_clock::duration(m_ticks.load()));
}

void FakeClock::sleepFor(std::chrono::milliseconds d)
{
    advance(d);
    if (onSleep) onSleep();
}

void FakeClock::advance(std::chrono::nanoseconds d)
{
    m_running += std::chrono::duration_cast<RunningTime::Duration>(d).count();
    m_ticks += std::chrono::duration_cast<std::chrono::steady_clock::duration>(d).count();
}

RunningTime::Duration FakeClock::running() const
{
    return RunningTime::Duration(m_running.load());
}

void FakeClock::suspend(std::chrono::nanoseconds d)
{
    m_ticks += std::chrono::duration_cast<std::chrono::steady_clock::duration>(d).count();
}

RunningTime::Clocks FakeClock::clocks()
{
    return {[this] { return running(); }, [this] { return now(); }};
}

// ---------------------------------------------------------------------------------------------------
class FakeApplyHold final : public LockHold {
public:
    FakeApplyHold(FakeApplyLock& l, EventLog* log) : m_l(l), m_log(log) {}
    ~FakeApplyHold() override
    {
        if (m_l.marked) {
            m_l.marked = false;
            m_l.present = false;
            if (m_log) m_log->add(QStringLiteral("applyLock:deleted"));
        }
        m_l.release();
    }
    bool markForDeletion(QString* err) override
    {
        if (m_log) m_log->add(QStringLiteral("applyLock:markForDeletion"));
        if (m_l.failMark) {
            *err = *m_l.failMark;
            return false;
        }
        m_l.marked = true;
        return true;
    }
    bool clearDeletion(QString* err) override
    {
        if (m_log) m_log->add(QStringLiteral("applyLock:clearDeletion"));
        if (m_l.failClear) {
            *err = *m_l.failClear;
            return false;
        }
        m_l.marked = false;
        return true;
    }

private:
    FakeApplyLock& m_l;
    EventLog* m_log;
};

std::unique_ptr<LockHold> FakeApplyLock::acquire(std::chrono::seconds)
{
    m_refusal.clear();
    if (refuse) {
        m_refusal = *refuse;
        if (m_log) m_log->add(QStringLiteral("applyLock:refused"));
        return nullptr;
    }
    if (alwaysTimeout) {
        if (m_log) m_log->add(QStringLiteral("applyLock:timeout"));
        return nullptr;
    }
    std::unique_lock g(m_m);
    ++waiting;
    m_cv.wait(g, [this] { return !m_held.load(); });
    --waiting;
    m_held = true;
    present = true;   // FILE_OPEN_IF
    if (m_log) m_log->add(QStringLiteral("applyLock:acquired"));
    return std::make_unique<FakeApplyHold>(*this, m_log);
}

void FakeApplyLock::release()
{
    {
        std::lock_guard g(m_m);
        m_held = false;
        if (m_log) m_log->add(QStringLiteral("applyLock:released"));
    }
    m_cv.notify_all();
}

void FakeWorld::touch(const QString& call)
{
    log.add(call);
    if (checkLock && !applyLock.held()) {
        std::lock_guard g(violationsMutex);
        violations << call;
    }
}

// ---------------------------------------------------------------------------------------------------
FakeFolder::FakeFolder(FakeWorld& w) : m_w(w) {}

Inspection FakeFolder::inspect(TrustMode mode)
{
    m_w.touch(QStringLiteral("folder:inspect %1").arg(int(mode)));
    ++inspections;
    directoryOpen = true;
    Inspection i;
    i.folderId = m_dir.id();
    std::optional<FsEntry> dir;
    QList<FsEntry> children;
    if (state != DirState::Missing) {
        FsEntry d;
        d.name = QStringLiteral("AetherSerialBridge");
        d.owner = owner;
        d.kind = state == DirState::Directory ? FsEntry::Kind::Directory
                 : state == DirState::Link    ? FsEntry::Kind::Link
                                              : FsEntry::Kind::File;
        if (folderWritableByOthers) d.acl << AceInfo{QStringLiteral("users"), AceJudge::Modify, AceKind::Allow};
        dir = d;
        if (state == DirState::Directory) {
            for (const QString& n : m_w.files.names()) {
                FsEntry c;
                c.name = n;
                i.children << n;
                switch (*m_w.files.kind(n)) {
                case FakeFileIo::Kind::File:
                    c.kind = FsEntry::Kind::File;
                    c.owner = m_w.files.owner(n);
                    break;
                case FakeFileIo::Kind::Symlink:
                    c.kind = FsEntry::Kind::Link;
                    c.owner = QStringLiteral("root");
                    break;
                case FakeFileIo::Kind::Directory:
                    c.kind = FsEntry::Kind::Directory;
                    c.owner = QStringLiteral("root");
                    break;
                }
                if (writableByOthers.contains(n))
                    c.acl << AceInfo{QStringLiteral("users"), AceJudge::Modify, AceKind::Allow};
                children << c;
            }
        }
    }
    const DataDirectoryTrust::Decision d = DataDirectoryTrust::decide(dir, children, policy);
    i.action = d.action;
    i.reason = d.reason;
    return i;
}

std::optional<QByteArray> FakeFolder::readFile(const Inspection& insp, const QString& name, qint64 cap, QString* err)
{
    m_w.touch(QStringLiteral("folder:read %1").arg(name));
    reads << name;
    if (onRead) onRead(name);
    auto fail = [err](const QString& e) -> std::optional<QByteArray> {
        if (err) *err = e;
        return std::nullopt;
    };
    if (state != DirState::Directory || !(insp.folderId == m_dir.id())) return fail(QStringLiteral("identity changed"));
    const auto kind = m_w.files.kind(name);
    if (!kind) return fail(fileNotFoundError());
    if (*kind == FakeFileIo::Kind::Symlink) return fail(QStringLiteral("ELOOP"));
    if (*kind != FakeFileIo::Kind::File) return fail(QStringLiteral("not a regular file"));
    if (!policy.isTrustedOwner(m_w.files.owner(name))) return fail(QStringLiteral("owned by an untrusted account"));
    if (configOwned.contains(name) && m_w.files.owner(name) != QStringLiteral("root"))
        return fail(QStringLiteral("not owned by the configuration owner"));
    if (writableByOthers.contains(name)) return fail(QStringLiteral("writable by others"));
    const QByteArray bytes = m_w.files.contents(name);
    if (bytes.size() > cap) return fail(QStringLiteral("larger than %1 bytes").arg(cap));
    return bytes;
}

void FakeFolder::createLockedDown()
{
    m_w.touch(QStringLiteral("folder:create"));
    if (onCall) onCall(QStringLiteral("create"));
    ++creates;
    if (failCreate) throw PlatformError(failCreate->toStdString());
    state = DirState::Directory;
    owner = QStringLiteral("root");
    folderWritableByOthers = false;
    m_dir = FakeDir(++m_ino);
}

QStringList FakeFolder::normalise(const Inspection& insp)
{
    m_w.touch(QStringLiteral("folder:normalise"));
    if (onCall) onCall(QStringLiteral("normalise"));
    ++normalises;
    // A failure after normaliseChanges were made, as the Windows adapter reports it (they stay on disk).
    if (failNormalise) throw PlatformErrorAfterChanges(failNormalise->toStdString(), normaliseChanges);
    if (!(insp.folderId == m_dir.id())) throw PlatformError("identity changed");
    return normaliseChanges;
}

bool FakeFolder::removeIfEmpty(const Inspection&, QString* err)
{
    m_w.touch(QStringLiteral("folder:removeIfEmpty"));
    if (failRemoveFolder) {
        *err = *failRemoveFolder;
        return false;
    }
    if (!m_w.files.names().isEmpty()) {
        *err = QStringLiteral("the folder is not empty");
        return false;
    }
    state = DirState::Missing;
    return true;
}

QString FakeFolder::renameAside(const Inspection& insp)
{
    m_w.touch(QStringLiteral("folder:renameAside"));
    if (onCall) onCall(QStringLiteral("renameAside"));
    ++renames;
    if (failRenameAside) throw PlatformError(failRenameAside->toStdString());
    if (!(insp.folderId == m_dir.id())) throw PlatformError("identity changed");
    // Everything moves with the folder; nothing is deleted.
    for (const QString& n : m_w.files.names()) {
        aside.insert(n, m_w.files.contents(n));
        m_w.files.eraseEntry(n);
    }
    m_w.files.syncDir(FakeDir(), nullptr);
    state = DirState::Missing;
    asidePath = QStringLiteral("AetherSerialBridge.untrusted-20261014T000000Z-1a2b");
    return asidePath;
}

// ---------------------------------------------------------------------------------------------------
namespace {

class SpyTempFile final : public TempFile {
public:
    SpyTempFile(SpyFileIo& io, std::unique_ptr<TempFile> inner, int serial, QString name)
        : m_io(io), m_inner(std::move(inner)), m_serial(serial), m_name(std::move(name))
    {
    }
    bool write(const QByteArray& b, QString* err) override
    {
        m_io.before(QStringLiteral("write"), m_name);
        return m_inner->write(b, err);
    }
    bool flush(QString* err) override
    {
        m_io.before(QStringLiteral("flush"), m_name);
        return m_inner->flush(err);
    }
    QByteArray readBack(qint64 cap, QString* err) override
    {
        m_io.before(QStringLiteral("readBack"), m_name);
        QByteArray b = m_inner->readBack(cap, err);
        if (m_io.tamperReadBack) b = m_io.tamperReadBack(b);
        auto& rec = m_io.temps[m_serial];
        rec.readBack = true;
        rec.readBytes = b;
        return b;
    }
    FileId id() const override { return m_inner->id(); }
    Commit commitAs(const QString& name) override
    {
        m_io.before(QStringLiteral("commitAs"), name);
        if (m_io.refuse) {
            if (const QString e = m_io.refuse(QStringLiteral("commitAs"), name); !e.isEmpty()) {
                m_io.world().log.add(QStringLiteral("%1:refused commitAs %2: %3").arg(m_io.tag(), name, e));
                Commit refused;
                refused.error = e;
                return refused;
            }
        }
        Commit c = m_inner->commitAs(name);
        if (c.replaced) m_io.temps[m_serial].committedAs = name;
        m_io.world().log.add(QStringLiteral("%1:committed %2 replaced=%3 durable=%4")
                                 .arg(m_io.tag(), name)
                                 .arg(c.replaced)
                                 .arg(c.durable));
        return c;
    }

private:
    SpyFileIo& m_io;
    std::unique_ptr<TempFile> m_inner;
    int m_serial;
    QString m_name;
};

class SpyExistingFile final : public ExistingFile {
public:
    SpyExistingFile(SpyFileIo& io, std::unique_ptr<ExistingFile> inner, QString name)
        : m_io(io), m_inner(std::move(inner)), m_name(std::move(name))
    {
    }
    bool truncate(QString* err) override
    {
        m_io.before(QStringLiteral("truncate"), m_name);
        return m_inner->truncate(err);
    }
    bool writeAll(const QByteArray& b, QString* err) override
    {
        m_io.before(QStringLiteral("writeAll"), m_name);
        return m_inner->writeAll(b, err);
    }
    bool flush(QString* err) override
    {
        m_io.before(QStringLiteral("flushExisting"), m_name);
        return m_inner->flush(err);
    }
    bool removeByHandle(QString* err) override
    {
        m_io.before(QStringLiteral("removeByHandle"), m_name);
        return m_inner->removeByHandle(err);
    }

private:
    SpyFileIo& m_io;
    std::unique_ptr<ExistingFile> m_inner;
    QString m_name;
};

}  // namespace

void SpyFileIo::before(const QString& op, const QString& name)
{
    m_w.touch(QStringLiteral("%1:%2 %3").arg(m_tag, op, name));
    if (beforeHook) beforeHook(op, name);
    ++m_ops;
    if (m_crashAt >= 0 && m_ops >= m_crashAt) {
        m_crashAt = -1;
        m_w.log.add(QStringLiteral("CRASH before %1 %2").arg(op, name));
        throw SimulatedCrash{op + QLatin1Char(' ') + name};
    }
    if (!m_crashOp.isEmpty() && op == m_crashOp && name.startsWith(m_crashName)) {
        m_crashOp.clear();
        m_w.log.add(QStringLiteral("CRASH before %1 %2").arg(op, name));
        throw SimulatedCrash{op + QLatin1Char(' ') + name};
    }
}

std::unique_ptr<TempFile> SpyFileIo::createTemp(const DirHandle& dir, OwnerPolicy policy, const NameGen& gen, QString* err)
{
    dirIds << dir.id();
    QString name;
    const NameGen capture = [&]() {
        name = gen();
        return name;
    };
    before(QStringLiteral("createTemp"), QStringLiteral("(temp)"));
    std::unique_ptr<TempFile> t = m_inner.createTemp(dir, policy, capture, err);
    if (!t) return nullptr;
    temps << TempRecord{int(temps.size()), name, false, {}, {}};
    return std::make_unique<SpyTempFile>(*this, std::move(t), int(temps.size()) - 1, name);
}

bool SpyFileIo::remove(const DirHandle& dir, const QString& name, QString* err)
{
    dirIds << dir.id();
    before(QStringLiteral("remove"), name);
    if (refuse) {
        if (const QString e = refuse(QStringLiteral("remove"), name); !e.isEmpty()) {
            m_w.log.add(QStringLiteral("%1:refused remove %2: %3").arg(m_tag, name, e));
            if (err) *err = e;
            return false;
        }
    }
    return m_inner.remove(dir, name, err);
}

bool SpyFileIo::syncDir(const DirHandle& dir, QString* err)
{
    dirIds << dir.id();
    before(QStringLiteral("syncDir"), QString());
    return m_inner.syncDir(dir, err);
}

std::unique_ptr<ExistingFile> SpyFileIo::openExisting(const DirHandle& dir, const QString& name, QString* err)
{
    dirIds << dir.id();
    before(QStringLiteral("openExisting"), name);
    std::unique_ptr<ExistingFile> f = m_inner.openExisting(dir, name, err);
    if (!f) return nullptr;
    return std::make_unique<SpyExistingFile>(*this, std::move(f), name);
}

// ---------------------------------------------------------------------------------------------------
FakeService::FakeService(FakeWorld& w) : m_w(w)
{
    installation.state = InstallState::NotInstalled;
}

void FakeService::call(const QString& method)
{
    m_w.touch(QStringLiteral("service:") + method);
    if (callDuration.count()) m_w.clock.advance(callDuration);
    if (onCall) onCall(method);
    if (fail.contains(method)) throw PlatformError(fail.value(method).toStdString());
}

Installation FakeService::detect(Deadline)
{
    call(QStringLiteral("detect"));
    // As the adapters classify it: no service, the program still there = an uninstall to finish.
    Installation i = installation;
    i.programRemains = i.state == InstallState::NotInstalled && programPresent;
    return i;
}

ServiceQuery FakeService::query(Deadline)
{
    call(QStringLiteral("query"));
    ServiceQuery q;
    if (installation.state == InstallState::NotInstalled) return q;
    q.state = engineRunning ? ServiceQuery::Running : ServiceQuery::Stopped;
    q.pid = engineRunning ? pid : 0;
    if (!engineRunning) {
        q.exitCode = lastExitCode;
        q.endedUnreported = lastEndedUnreported;
    }
    if (stateOverride) q.state = *stateOverride;
    if (!engineRunning && stopRequested && neverReleases) {   // the process outlives its stop request
        q.state = ServiceQuery::StopPending;
        q.pid = pid;
    }
    return q;
}

void FakeService::inhibitStarts(Deadline)
{
    call(QStringLiteral("inhibitStarts"));
    startsBlocked = true;
}

void FakeService::allowStarts(Deadline)
{
    call(QStringLiteral("allowStarts"));
    startsBlocked = false;
}

void FakeService::requestStop(Deadline)
{
    call(QStringLiteral("requestStop"));
    if (!engineRunning) return;
    stopRequested = true;
    engineRunning = false;   // SCM/systemd report it stopped ...
    if (neverReleases || releaseAfterPolls > 0) return;   // ... but the process may still hold the lock
    engineHoldsLock = false;
}

void FakeService::onLockPoll()
{
    if (engineHoldsLock && stopRequested && !neverReleases && releaseAfterPolls > 0 && --releaseAfterPolls == 0)
        engineHoldsLock = false;
}

void FakeService::installOrConfigure(InstallState, Deadline)
{
    m_w.touch(QStringLiteral("service:installOrConfigure"));
    if (callDuration.count()) m_w.clock.advance(callDuration);
    if (onCall) onCall(QStringLiteral("installOrConfigure"));
    if (partialInstallRunning) {
        installation.state = InstallState::Current;
        startsBlocked = false;
        launchEngine();
        throw PlatformError("the service was created but configuring it failed");
    }
    if (fail.contains(QStringLiteral("installOrConfigure")))
        throw PlatformError(fail.value(QStringLiteral("installOrConfigure")).toStdString());
    installation.state = InstallState::Current;
    installation.otherBuild = false;   // this build's program is installed now
    installation.installedSha256.clear();
    startsBlocked = true;
    programPresent = folderPresent = true;
}

void FakeService::launchEngine()
{
    // The process starts; it takes engine.lock (exit 4 when it cannot), then needs a valid token (exit 6).
    if (engineHoldsLock) return;
    const bool lockExists = m_w.lockFiles.exists(QStringLiteral("engine.lock"));
    m_w.log.add(QStringLiteral("engine:launch"));
    if (!lockExists) {
        m_w.log.add(QStringLiteral("engine:exit 4"));
        lastExitCode = 4;
        return;
    }
    if (m_lockedByApplier && *m_lockedByApplier) {
        m_w.log.add(QStringLiteral("engine:exit 4"));
        lastExitCode = 4;
        return;
    }
    // An engine.lock with the wrong owner fails the service's handle check (exit 4).
    if (m_w.lockFiles.owner(QStringLiteral("engine.lock")) == QStringLiteral("attacker")) {
        m_w.log.add(QStringLiteral("engine:exit 4 (insecure lock)"));
        lastExitCode = 4;
        return;
    }
    // D58: the ids in engine.lock and engine.lock.id must match (BridgeRunner::start, exit 4).
    const auto lockId = LockIdentity::parse(m_w.lockFiles.contents(QStringLiteral("engine.lock")));
    const auto recordId = LockIdentity::parse(m_w.lockFiles.contents(QStringLiteral("engine.lock.id")));
    if (!lockId || !recordId || *lockId != *recordId) {
        m_w.log.add(QStringLiteral("engine:exit 4 (lock identity)"));
        lastExitCode = 4;
        return;
    }
    QString why;
    const auto token = StartToken::parse(m_w.files.contents(QString::fromLatin1(ConfigFiles::kStartToken)), &why);
    if (!token) {
        m_w.log.add(QStringLiteral("engine:exit 6"));
        lastExitCode = 6;
        return;
    }
    if (staleFirewallAtStart) {
        m_w.log.add(QStringLiteral("engine:exit 7 (unreported)"));
        lastExitCode.reset();
        lastEndedUnreported = true;
        return;
    }
    lastExitCode.reset();
    lastEndedUnreported = false;
    engineRunning = true;
    engineHoldsLock = true;
    stopRequested = false;
    pid = m_nextPid++;
    m_w.log.add(QStringLiteral("engine:running pid=%1").arg(pid));
    const QByteArray config = m_w.files.contents(QString::fromLatin1(ConfigFiles::kConfig));
    StatusReport r;
    r.pid = pid;
    r.startToken = token->token;
    r.configSha256 = sha256Hex(config);
    r.instanceId = QByteArrayLiteral("11111111111111111111111111111111");
    r.startedAt = QDateTime::currentDateTimeUtc();
    r.updated = r.startedAt;
    switch (ack) {
    case AckMode::Correct:
        break;
    case AckMode::Never:
        return;
    case AckMode::ConfigError:
        r.configError = QStringLiteral("'nope' is not a serial port name");
        break;
    case AckMode::StaleToken:
        r.startToken = previousToken;
        break;
    case AckMode::WrongPid:
        r.pid = pid + 1;
        break;
    case AckMode::WrongDigest:
        r.configSha256 = QString(64, QLatin1Char('0'));
        break;
    }
    status = r;
}

void FakeService::start(Deadline)
{
    call(QStringLiteral("start"));
    if (startsBlocked) throw PlatformError("the service is disabled");
    launchEngine();
    if (startThrowsAfterRunning) throw PlatformError("the service did not report Running in time");
}

void FakeService::uninstall(Deadline)
{
    call(QStringLiteral("uninstall"));
    installation.state = InstallState::NotInstalled;
    engineRunning = false;
    // Linux: the program goes with the unit. "uninstall!" = the unit removed, then the program's removal failed (the
    // order SystemdService::uninstall removes them in).
    if (!separateProgram && fail.contains(QStringLiteral("uninstall!")))
        throw PlatformError(fail.value(QStringLiteral("uninstall!")).toStdString());
    if (!separateProgram) programPresent = false;
}

void FakeService::reconcileOutstandingJobs(Deadline)
{
    call(QStringLiteral("reconcileOutstandingJobs"));
}

IServiceControl::ProgramRemoval FakeService::removeProgram(Deadline)
{
    call(QStringLiteral("removeProgram"));
    ProgramRemoval r;
    if (!programPresent) return r;   // Absent
    if (fail.contains(QStringLiteral("removeProgram!"))) {   // kept: in use by another process
        r.kind = ProgramRemoval::Kind::Kept;
        r.error = fail.value(QStringLiteral("removeProgram!"));
        r.changes << QStringLiteral("kept %1 (%2)").arg(installedExePath, r.error);
        return r;
    }
    programPresent = false;
    if (programIsOwnImage) {
        programScheduled = true;
        r.kind = ProgramRemoval::Kind::ScheduledAtRestart;
        r.changes << QStringLiteral("renamed %1 aside; it is deleted when Windows restarts").arg(installedExePath);
        return r;
    }
    r.kind = ProgramRemoval::Kind::Removed;
    r.changes << QStringLiteral("removed %1").arg(installedExePath);
    return r;
}

QStringList FakeService::removeProgramFolder()
{
    // Called after the locks were released (not under apply.lock by design): logged without the lock check.
    m_w.log.add(QStringLiteral("service:removeProgramFolder"));
    if (!folderPresent) return {};
    QStringList left = folderOthers;
    if (programPresent) left << QStringLiteral("aether-serial-bridge.exe");
    if (m_w.applyLock.present) left << QStringLiteral("apply.lock");
    for (const QString& n : m_w.lockFiles.names()) left << n;
    if (!left.isEmpty()) return {QStringLiteral("kept %1 (it still holds %2)").arg(programFolder(), left.join(QStringLiteral(", ")))};
    folderPresent = false;   // a renamed-aside running image is scheduled with the folder (MOVEFILE_DELAY_UNTIL_REBOOT)
    return {QStringLiteral("removed %1").arg(programFolder())};
}

QString FakeService::writeAppsEntry()
{
    call(QStringLiteral("writeAppsEntry"));
    if (fail.contains(QStringLiteral("writeAppsEntry!"))) return fail.value(QStringLiteral("writeAppsEntry!"));
    ++entryWrites;
    entry.present = true;
    entry.uninstallExe = installedExePath;
    entry.version = version;
    return {};
}

void FakeService::removeAppsEntry(Deadline)
{
    call(QStringLiteral("removeAppsEntry"));
    entry = Entry{};
}

// ---------------------------------------------------------------------------------------------------
class FakeEngineHold final : public LockHold {
public:
    FakeEngineHold(FakeEngineLock& l, FakeWorld& w) : m_l(l), m_w(w) {}
    ~FakeEngineHold() override
    {
        if (m_l.marked) {
            m_l.marked = false;
            m_w.lockFiles.eraseEntry(QStringLiteral("engine.lock"));
            m_w.log.add(QStringLiteral("lockio:deleted engine.lock"));
        }
        m_l.release();
    }
    bool markForDeletion(QString* err) override
    {
        m_w.touch(QStringLiteral("lockio:markForDeletion engine.lock"));
        if (m_l.failMark) {
            *err = *m_l.failMark;
            return false;
        }
        m_l.marked = true;
        return true;
    }
    bool clearDeletion(QString* err) override
    {
        m_w.log.add(QStringLiteral("lockio:clearDeletion engine.lock"));
        if (m_l.failClear) {
            *err = *m_l.failClear;
            return false;
        }
        m_l.marked = false;
        return true;
    }

private:
    FakeEngineLock& m_l;
    FakeWorld& m_w;
};

FakeEngineLock::FakeEngineLock(FakeWorld& w, FakeService& s)
    : m_w(w), m_s(s), m_io(w, w.lockFiles, QStringLiteral("lockio"))
{
    s.m_lockedByApplier = &m_applierHolds;
    m_io.refuse = [this](const QString&, const QString& name) {
        if (windowsSharing && name == QLatin1String("engine.lock") && (m_applierHolds || m_s.engineHoldsLock))
            return QStringLiteral("ERROR_SHARING_VIOLATION");
        return QString();
    };
}

void FakeEngineLock::plantPair(const QByteArray& id)
{
    plantLock(QByteArrayLiteral("{\"lockId\":\"") + id + QByteArrayLiteral("\"}"));
    plantRecord(QByteArrayLiteral("{\"lockId\":\"") + id + QByteArrayLiteral("\"}"));
}

void FakeEngineLock::plantLock(const QByteArray& content)
{
    m_w.lockFiles.plantFile(QStringLiteral("engine.lock"), content, QStringLiteral("root"));
}

void FakeEngineLock::plantInsecureLock(const QByteArray& content)
{
    m_w.lockFiles.plantFile(QStringLiteral("engine.lock"), content, QStringLiteral("attacker"));
}

bool FakeEngineLock::lockFileInsecure() const
{
    return m_w.lockFiles.owner(QStringLiteral("engine.lock")) == QStringLiteral("attacker");
}

void FakeEngineLock::plantRecord(const QByteArray& content)
{
    m_w.lockFiles.plantFile(QStringLiteral("engine.lock.id"), content, QStringLiteral("root"));
}

QByteArray FakeEngineLock::lockContent() const
{
    return m_w.lockFiles.contents(QStringLiteral("engine.lock"));
}

QByteArray FakeEngineLock::recordContent() const
{
    return m_w.lockFiles.contents(QStringLiteral("engine.lock.id"));
}

std::unique_ptr<LockHold> FakeEngineLock::tryAcquire(LockResult* why)
{
    m_w.log.add(QStringLiteral("engineLock:tryAcquire"));
    LockResult r = LockResult::Acquired;
    if (unsupportedFs) r = LockResult::Unsupported;
    else if (insecure) r = LockResult::Insecure;
    else if (!m_w.lockFiles.exists(QStringLiteral("engine.lock"))) r = LockResult::Missing;
    else if (lockFileInsecure()) r = LockResult::InsecureFile;
    else if (m_applierHolds || m_s.engineHoldsLock) r = LockResult::Busy;
    if (why) *why = r;
    if (r != LockResult::Acquired) return nullptr;
    m_applierHolds = true;
    directoryOpen = true;
    return std::make_unique<FakeEngineHold>(*this, m_w);
}

std::unique_ptr<LockHold> FakeEngineLock::acquireUntil(Deadline deadline, LockResult* why)
{
    m_w.touch(QStringLiteral("engineLock:acquireUntil"));
    auto result = [&](LockResult r) -> std::unique_ptr<LockHold> {
        if (why) *why = r;
        m_w.log.add(QStringLiteral("engineLock:result %1").arg(int(r)));
        return nullptr;
    };
    if (unsupportedFs) return result(LockResult::Unsupported);
    if (insecure) return result(LockResult::Insecure);
    if (!m_w.lockFiles.exists(QStringLiteral("engine.lock"))) return result(LockResult::Missing);
    if (lockFileInsecure()) return result(LockResult::InsecureFile);
    for (;;) {
        if (!m_applierHolds && !m_s.engineHoldsLock) break;
        if (m_w.clock.now() >= deadline) return result(LockResult::Busy);
        m_w.clock.sleepFor(std::chrono::milliseconds(100));
        m_s.onLockPoll();
    }
    m_applierHolds = true;
    if (why) *why = LockResult::Acquired;
    m_w.log.add(QStringLiteral("engineLock:acquired"));
    return std::make_unique<FakeEngineHold>(*this, m_w);
}

void FakeEngineLock::release()
{
    m_applierHolds = false;
    directoryOpen = false;
    m_w.log.add(QStringLiteral("engineLock:released"));
}

LockFileRead FakeEngineLock::readFile(const QString& name) const
{
    LockFileRead r;
    const auto kind = m_w.lockFiles.kind(name);
    if (!kind) {
        r.status = LockFileRead::Status::Missing;
        return r;
    }
    if (*kind != FakeFileIo::Kind::File) {
        r.status = LockFileRead::Status::Failed;
        r.error = QStringLiteral("not a regular file");
        return r;
    }
    if (m_w.lockFiles.owner(name) == QStringLiteral("attacker")) {
        r.status = LockFileRead::Status::Failed;
        r.error = QStringLiteral("wrong owner");
        return r;
    }
    r.status = LockFileRead::Status::Ok;
    r.bytes = m_w.lockFiles.contents(name);
    return r;
}

LockFileRead FakeEngineLock::readHeldLock()
{
    m_w.log.add(QStringLiteral("engineLock:readHeld"));
    if (onRead) onRead(QStringLiteral("held"));
    if (!m_applierHolds) {
        LockFileRead r;
        r.error = QStringLiteral("not held");
        return r;
    }
    return readFile(QStringLiteral("engine.lock"));
}

LockFileRead FakeEngineLock::peekLock()
{
    m_w.log.add(QStringLiteral("engineLock:peek"));
    if (peekUnreadable && m_w.lockFiles.exists(QStringLiteral("engine.lock"))) {
        LockFileRead r;
        r.status = LockFileRead::Status::Unreadable;
        return r;
    }
    return readFile(QStringLiteral("engine.lock"));
}

LockFileRead FakeEngineLock::readRecord()
{
    m_w.log.add(QStringLiteral("engineLock:readRecord"));
    if (onRead) onRead(QStringLiteral("record"));
    return readFile(QStringLiteral("engine.lock.id"));
}

bool FakeEngineLock::filesystemSupported(QString* why)
{
    if (unsupportedFs) {
        if (why) *why = *unsupportedFs;
        return false;
    }
    return true;
}

QStringList FakeEngineLock::children()
{
    return m_w.lockFiles.names();
}

// ---------------------------------------------------------------------------------------------------
void FakeFirewall::call(const QString& method)
{
    m_w.touch(QStringLiteral("firewall:") + method);
    if (callDuration.count()) m_w.clock.advance(callDuration);
    if (onCall) onCall(method);
    if (fail.contains(method)) throw PlatformError(fail.value(method).toStdString());
}

namespace {
void apply(QStringList& rules, const QList<FirewallPlan::Step>& steps)
{
    for (const FirewallPlan::Step& s : steps) {
        if (s.kind == FirewallPlan::Step::Kind::CreateDisabled) rules << s.instanceId;
        if (s.kind == FirewallPlan::Step::Kind::Delete) rules.removeAll(s.instanceId);
    }
}
}  // namespace

void FakeFirewall::requireConnection(const QString& method) const
{
    if (!connected)
        throw PlatformError(QStringLiteral("%1 without a connection (connect() was not called)").arg(method).toStdString());
}

void FakeFirewall::connect(Deadline)
{
    call(QStringLiteral("connect"));
    connected = true;
}

void FakeFirewall::sync(const BridgeConfig& c, const FirewallGeneration& g, Deadline)
{
    call(QStringLiteral("sync"));
    requireConnection(QStringLiteral("sync"));
    QString err;
    const auto plan = FirewallPlan::build(c, g, QStringLiteral("C:/Program Files/AetherSerialBridge/aether-serial-bridge.exe"), &err);
    if (!plan) throw PlatformError(err.toStdString());
    apply(rules, FirewallPlan::syncSteps(*plan, rules));
    syncedFrom = c;
    lastGeneration = g;
}

void FakeFirewall::rollback(const FirewallGeneration& g, Deadline)
{
    call(QStringLiteral("rollback"));
    requireConnection(QStringLiteral("rollback"));
    apply(rules, FirewallPlan::rollbackSteps(g, rules));
}

void FakeFirewall::sweep(const FirewallGeneration& keep, Deadline)
{
    call(QStringLiteral("sweep"));
    requireConnection(QStringLiteral("sweep"));
    apply(rules, FirewallPlan::sweepSteps(keep, rules));
}

void FakeFirewall::removeAll(Deadline)
{
    call(QStringLiteral("removeAll"));
    requireConnection(QStringLiteral("removeAll"));
    rules.clear();
}

// ---------------------------------------------------------------------------------------------------
Rig::Rig()
{
    token = std::make_unique<FileStartToken>(folder, io);
    platform = std::make_unique<Platform>(Platform{folder, service, firewall, world.applyLock, engineLock, *token, status, io});
}

Rig::~Rig() = default;

void Rig::installedRunning(const QByteArray& committed)
{
    world.files.plantFile(QString::fromLatin1(ConfigFiles::kConfig), committed, QStringLiteral("root"));
    const FirewallGeneration g0{QByteArrayLiteral("0123456789abcdef")};
    const QByteArray t0 = QByteArrayLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    world.files.plantFile(QString::fromLatin1(ConfigFiles::kStartToken), StartToken::format(t0, g0), QStringLiteral("root"));
    engineLock.plantPair(QByteArrayLiteral("5555555555555555aaaaaaaaaaaaaaaa"));
    service.installation.state = InstallState::Current;
    service.programPresent = service.folderPresent = true;
    service.previousToken = t0;
    service.launchEngine();
    const auto cfg = fromJson(committed, nullptr);
    QString err;
    const auto plan = FirewallPlan::build(*cfg, g0, QStringLiteral("x.exe"), &err);
    for (const auto& r : plan->rules) firewall.rules << r.instanceId;
    world.log.clear();
}

QByteArray configA()
{
    return QByteArrayLiteral(R"({
  "version": 1,
  "ports": [
    {
      "enabled": true,
      "name": "Amp",
      "comPort": "COM3",
      "baudRate": 38400,
      "tcpPort": 4001,
      "bindAddress": "0.0.0.0",
      "allowedClients": "10.0.0.5"
    }
  ]
})");
}

QByteArray configB()
{
    return QByteArrayLiteral(R"({
  "version": 1,
  "ports": [
    {
      "enabled": true,
      "name": "Amp",
      "comPort": "COM4",
      "baudRate": 38400,
      "tcpPort": 4001,
      "bindAddress": "0.0.0.0",
      "allowedClients": "0.0.0.0/0"
    }
  ]
})");
}

QByteArray configC()
{
    return QByteArrayLiteral(R"({
  "version": 1,
  "ports": [
    {
      "enabled": true,
      "name": "Tuner",
      "comPort": "COM5",
      "baudRate": 38400,
      "tcpPort": 4002,
      "bindAddress": "0.0.0.0"
    }
  ]
})");
}

bool claimsStop(QString text)
{
    static const QRegularExpression negated(
        QStringLiteral(R"((could not|cannot|did not) prove (that )?(the bridge|the engine|no engine)[^.;]*|)"
                       R"(not proven stopped|prove the bridge stopped|the operation stopped)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression claim(QStringLiteral(R"(stopped|verified|is off|no engine process|terminated)"),
                                          QRegularExpression::CaseInsensitiveOption);
    return text.remove(negated).contains(claim);
}

QByteArray digest(const QByteArray& b)
{
    return sha256Hex(b).toLatin1();
}

// ---------------------------------------------------------------------------------------------------
void FakeEngine::start(const BridgeConfig& c)
{
    m_log.add(QStringLiteral("engine:start %1").arg(c.ports.size()));
    started = c;
}

void FakeEngine::stop(std::function<void()> done)
{
    m_log.add(QStringLiteral("engine:stop"));
    ++stops;
    done();
}

QList<PortStatus> FakeEngine::snapshot() const
{
    QList<PortStatus> out;
    if (!started) return out;
    for (const PortMapping& p : started->ports) {
        PortStatus s;
        s.name = p.name;
        s.comPort = p.comPort;
        s.bindAddress = p.bindAddress;
        s.tcpPort = p.tcpPort;
        s.state = BridgeState::Listening;
        out << s;
    }
    return out;
}

void FakeSideChannel::publishStatus(const StatusReport& r)
{
    m_log.add(QStringLiteral("side:publish"));
    if (throwOnPublish) throw std::runtime_error("status.json: access denied");
    published << r;
}

RunningTime::Duration FakeWatchdog::toRunning(std::chrono::steady_clock::time_point d) const
{
    return RunningTime::fromSteady(d, m_clock.clocks());
}

void FakeWatchdog::arm(std::chrono::steady_clock::time_point d, int code)
{
    const RunningTime::Duration r = toRunning(d);
    std::lock_guard g(m_m);
    m_log.add(QStringLiteral("watchdog:arm"));
    if (!m_code) {
        m_deadline = r;
        m_code = code;
    } else if (r < *m_deadline) {
        m_deadline = r;
        m_code = code;
    }
}

void FakeWatchdog::extend(std::chrono::steady_clock::time_point d)
{
    if (onExtend) onExtend();
    const RunningTime::Duration r = toRunning(d);
    std::lock_guard g(m_m);
    m_log.add(QStringLiteral("watchdog:extend"));
    m_lastExtend = r;
    if (m_code && r > *m_deadline) m_deadline = r;
}

void FakeWatchdog::tighten(std::chrono::steady_clock::time_point d, int code)
{
    if (onTighten) onTighten(tightenCalls++);
    const RunningTime::Duration r = toRunning(d);
    std::lock_guard g(m_m);
    m_log.add(QStringLiteral("watchdog:tighten"));
    if (!m_code || r < *m_deadline) {
        m_deadline = r;
        m_code = code;
    }
}

void FakeWatchdog::expireNow()
{
    std::lock_guard g(m_m);
    m_log.add(QStringLiteral("watchdog:expireNow"));
    expiredNow = true;
}

bool FakeWatchdog::expired() const
{
    std::lock_guard g(m_m);
    return expiredNow || (m_code && m_clock.running() >= *m_deadline);
}

std::optional<int> FakeWatchdog::code() const
{
    std::lock_guard g(m_m);
    return m_code;
}

RunningTime::Duration FakeWatchdog::runningDeadline() const
{
    std::lock_guard g(m_m);
    return m_deadline.value_or(RunningTime::kNever);
}

std::chrono::steady_clock::time_point FakeWatchdog::deadline() const
{
    std::lock_guard g(m_m);
    if (!m_deadline) return std::chrono::steady_clock::time_point::max();
    return std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(*m_deadline));
}

std::optional<RunningTime::Duration> FakeWatchdog::lastExtend() const
{
    std::lock_guard g(m_m);
    return m_lastExtend;
}

bool FakeWatchdog::armed() const
{
    std::lock_guard g(m_m);
    return m_code.has_value();
}

void FakeMonitor::threadInit()
{
    m_log.add(QStringLiteral("monitor:threadInit"));
}

IFirewallMonitor::Check FakeMonitor::checkOnce(const BridgeConfig& config, const FirewallGeneration&)
{
    const int n = checks++;
    m_log.add(QStringLiteral("monitor:check %1").arg(n));
    if (duringCheck) duringCheck(n);
    // Read after duringCheck: a check blocked there still reads its config afterwards.
    lastTcpPort = config.ports.isEmpty() ? -1 : config.ports.first().tcpPort;
    m_clock.advance(checkDuration);
    return results.isEmpty() ? Check::Failed : results.value(qMin(n, int(results.size()) - 1));
}

}  // namespace asb::test
