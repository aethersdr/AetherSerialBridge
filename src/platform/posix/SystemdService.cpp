#include "platform/posix/SystemdService.h"

#include "core/AtomicJson.h"
#include "core/BuildStamp.h"
#include "core/InstallationEvidence.h"
#include "core/ServicePlan.h"
#include "platform/posix/PosixFileIo.h"
#include "platform/posix/SystemdUnit.h"

#include <QCryptographicHash>
#include <QSet>
#include <QThread>

#include <grp.h>
#include <unistd.h>

#include <cerrno>
#include <exception>
#include <ctime>

namespace asb::posix {
namespace {

using Ms = std::chrono::milliseconds;

constexpr char kUnitIface[] = "org.freedesktop.systemd1.Unit";
constexpr char kServiceIface[] = "org.freedesktop.systemd1.Service";

QString parentOf(const QString& path)
{
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    return slash <= 0 ? QStringLiteral("/") : path.left(slash);
}

QString nameOf(const QString& path)
{
    return path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
}

[[noreturn]] void fail(const QString& what)
{
    throw PlatformError(what.toStdString());
}

// The installed program, for build identity (v1.1 D61, INV-120). Windows hashes the installed exe whole up to the
// same cap (WindowsService::evidence).
constexpr qint64 kProgramCap = 256 * 1024 * 1024;
constexpr qint64 kHashChunk = 1024 * 1024;

struct ProgramEvidence {
    bool present = false;      // a regular file at the path (a link is not the program)
    bool unreadable = false;   // present, but not read whole: another owner, over the cap, a read error
    QString sha256;            // when read whole
    std::optional<QString> version;   // its build stamp ("" when it has none); when read whole
};

// How much older than the wall clock a file's ctime must be before its digest is kept: well over the coarsest file
// time a Linux file system keeps (1 s) plus the kernel's coarse-clock lag (a tick).
constexpr qint64 kProgramSettleNs = 2'000'000'000;

bool sameTime(const struct timespec& a, const struct timespec& b)
{
    return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

qint64 nanos(const struct timespec& t)
{
    return qint64(t.tv_sec) * 1'000'000'000 + qint64(t.tv_nsec);
}

struct timespec realtimeNow()
{
    struct timespec t {};
    ::clock_gettime(CLOCK_REALTIME, &t);
    return t;
}

ProgramEvidence readProgram(PosixSys& sys, const QString& path, uid_t owner, ProgramDigestCache& cache,
                            const std::function<struct timespec()>& wallClock)
{
    ProgramEvidence r;
    // Read before the file's times: a write after this instant gets a ctime no earlier than it, less one tick.
    const struct timespec now = wallClock ? wallClock() : realtimeNow();
    const QByteArray p = path.toLocal8Bit();
    struct stat st {};
    if (sys.fstatat(AT_FDCWD, p.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) return r;
    r.present = true;
    r.unreadable = true;   // until it has been read whole
    Fd fd(sys, sys.open(p.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (!fd) return r;
    struct stat fst {};
    if (sys.fstat(fd.get(), &fst) != 0 || !S_ISREG(fst.st_mode) || fst.st_dev != st.st_dev || fst.st_ino != st.st_ino ||
        fst.st_uid != owner || fst.st_size > kProgramCap)
        return r;
    if (cache.valid && cache.dev == fst.st_dev && cache.ino == fst.st_ino && cache.size == fst.st_size &&
        sameTime(cache.mtime, fst.st_mtim) && sameTime(cache.ctime, fst.st_ctim)) {
        r.unreadable = false;
        r.sha256 = cache.sha256;
        r.version = cache.version;
        return r;
    }
    cache.valid = false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray chunk(kHashChunk, Qt::Uninitialized);
    qint64 total = 0;
    for (;;) {
        const ssize_t n = sys.pread(fd.get(), chunk.data(), size_t(chunk.size()), off_t(total));
        if (n < 0) {
            if (errno == EINTR) continue;
            return r;
        }
        if (n == 0) break;
        total += n;
        if (total > kProgramCap) return r;
        hash.addData(QByteArrayView(chunk.constData(), qsizetype(n)));
    }
    if (total != qint64(fst.st_size)) return r;   // changed while it was read
    r.unreadable = false;
    r.sha256 = QString::fromLatin1(hash.result().toHex());
    const std::optional<core::BuildStamp> stamp = core::parseBuildStamp(total, [&sys, &fd](qint64 offset, qint64 length) {
        QByteArray b(qsizetype(length), Qt::Uninitialized);
        qint64 got = 0;
        while (got < length) {
            const ssize_t n = sys.pread(fd.get(), b.data() + got, size_t(length - got), off_t(offset + got));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            got += n;
        }
        b.truncate(qsizetype(got));
        return b;
    });
    r.version = stamp ? stamp->version : QString();
    if (nanos(fst.st_ctim) < nanos(now) - kProgramSettleNs)   // settled: no later write can share its ctime
        cache = ProgramDigestCache{true, fst.st_dev, fst.st_ino, fst.st_size, fst.st_mtim, fst.st_ctim, r.sha256, r.version};
    return r;
}

}  // namespace

bool BusError::noSuchUnit() const
{
    return m_name == QLatin1String("org.freedesktop.systemd1.NoSuchUnit");
}

SystemdService::SystemdService(ISystemdBus& bus, Options options, IClock& clock, PosixSys& sys)
    : m_bus(bus), m_opt(std::move(options)), m_clock(clock), m_sys(sys)
{
}

QStringList SystemdService::takeChanges()
{
    QStringList c;
    c.swap(m_changes);
    return c;
}

void SystemdService::step(const char* name)
{
    if (m_opt.onStep) m_opt.onStep(QString::fromLatin1(name));
}

Ms SystemdService::remaining(Deadline d, const char* what) const
{
    const auto now = m_clock.now();
    if (now >= d) fail(QStringLiteral("the deadline passed before %1 could be sent to systemd").arg(QString::fromLatin1(what)));
    // Rounded up, so a call never gets less than what is left.
    return std::chrono::ceil<Ms>(d - now);
}

void SystemdService::requireRoot() const
{
    if (m_opt.requireRoot && ::geteuid() != 0) fail(QStringLiteral("controlling the systemd service needs root"));
}

// ---------------------------------------------------------------------------------------------------
// Job tracking (INV-115)
// ---------------------------------------------------------------------------------------------------

void SystemdService::ensureReceivers()
{
    // Installed on the thread that waits (pump() runs that thread's event loop), before the submission.
    if (m_receiversInstalled && m_receiverThread == QThread::currentThread()) return;
    ISystemdBus::Receivers r;
    r.jobRemoved = [this](quint32, const QString& job, const QString& unit, const QString& result) {
        if (unit == m_opt.unitName) m_done.insert(job, result);
    };
    r.managerOwnerChanged = [this] { m_ownerChanged = true; };
    m_bus.setReceivers(std::move(r));
    m_receiversInstalled = true;
    m_receiverThread = QThread::currentThread();
}

void SystemdService::subscribeOnce(Deadline d)
{
    if (m_subscribed) return;
    m_bus.subscribe(remaining(d, "Subscribe"));
    m_subscribed = true;
}

QString SystemdService::awaitJob(const char* what, const std::function<QString(Ms)>& submit, Deadline d)
{
    // The receivers first: a JobRemoved that arrives before the reply (the job may finish at once) is kept.
    ensureReceivers();
    subscribeOnce(d);
    m_ownerChanged = false;
    const QString job = submit(remaining(d, what));
    for (;;) {
        // An owner change is checked FIRST: a pump may deliver it together with the completion, and a
        // completion from a manager that changed owner proves nothing (INV-115).
        if (m_ownerChanged)
            fail(QStringLiteral("%1: systemd's bus name changed owner while the job was running").arg(QString::fromLatin1(what)));
        // Matched by the exact job object path from the reply, never by the unit name.
        if (auto it = m_done.find(job); it != m_done.end()) {
            const QString result = it.value();
            m_done.erase(it);
            if (result != QLatin1String("done"))
                fail(QStringLiteral("%1: systemd reported the job as %2").arg(QString::fromLatin1(what), result));
            return result;
        }
        const auto now = m_clock.now();
        if (now >= d)
            fail(QStringLiteral("%1: the job did not finish before the deadline (it may still run later)").arg(QString::fromLatin1(what)));
        m_bus.pump(std::min(std::chrono::ceil<Ms>(d - now), Ms(200)));
    }
}

void SystemdService::reconcileOutstandingJobs(Deadline d)
{
    requireRoot();
    ensureReceivers();
    subscribeOnce(d);
    m_ownerChanged = false;
    QSet<quint32> cancelled;
    for (;;) {
        // Before the success return below, as in awaitJob: the pump that ended the last job may also have
        // brought an owner change.
        if (m_ownerChanged) fail(QStringLiteral("systemd's bus name changed owner while outstanding jobs were settled"));
        QList<SystemdJob> ours;
        for (const SystemdJob& j : m_bus.listJobs(remaining(d, "ListJobs")))
            if (j.unit == m_opt.unitName) ours << j;
        if (ours.isEmpty()) return;
        for (const SystemdJob& j : ours) {
            // A job still waiting (e.g. a start submitted late, R16) is cancelled; a running one is awaited.
            if (j.state != QLatin1String("waiting") || cancelled.contains(j.id)) continue;
            try {
                m_bus.cancelJob(j.id, remaining(d, "CancelJob"));
            } catch (const BusError& e) {
                if (e.name() != QLatin1String("org.freedesktop.systemd1.NoSuchJob")) throw;
            }
            cancelled.insert(j.id);
        }
        const auto now = m_clock.now();
        if (now >= d) {
            QStringList names;
            for (const SystemdJob& j : ours) names << QStringLiteral("%1 %2 (%3)").arg(j.type, j.jobPath, j.state);
            fail(QStringLiteral("outstanding systemd jobs for %1 did not finish before the deadline: %2")
                     .arg(m_opt.unitName, names.join(QStringLiteral(", "))));
        }
        m_bus.pump(std::min(std::chrono::ceil<Ms>(d - now), Ms(200)));
    }
}

// ---------------------------------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------------------------------

QString SystemdService::unitProperty(const char* iface, const char* property, Deadline d)
{
    const QString path = m_bus.loadUnit(m_opt.unitName, remaining(d, "LoadUnit"));
    return m_bus.unitProperty(path, QString::fromLatin1(iface), QString::fromLatin1(property), remaining(d, property)).toString();
}

QString SystemdService::loadState(Deadline d)
{
    return unitProperty(kUnitIface, "LoadState", d);
}

void SystemdService::verify(const char* property, const QString& want, Deadline d)
{
    const QString got = unitProperty(kUnitIface, property, d);
    if (got != want)
        fail(QStringLiteral("systemd reports %1 = %2 for %3, expected %4")
                 .arg(QString::fromLatin1(property), got, m_opt.unitName, want));
}

ServiceQuery SystemdService::query(Deadline d)
{
    ServiceQuery q;
    QList<SystemdUnitProcess> procs;
    try {
        procs = m_bus.unitProcesses(m_opt.unitName, remaining(d, "GetUnitProcesses"));
    } catch (const BusError& e) {
        if (!e.noSuchUnit()) throw;   // a unit systemd has not loaded has no processes
    }
    const QString path = m_bus.loadUnit(m_opt.unitName, remaining(d, "LoadUnit"));
    const QString load = m_bus.unitProperty(path, QString::fromLatin1(kUnitIface), QStringLiteral("LoadState"), remaining(d, "LoadState")).toString();
    const QString active = m_bus.unitProperty(path, QString::fromLatin1(kUnitIface), QStringLiteral("ActiveState"), remaining(d, "ActiveState")).toString();
    quint32 mainPid = 0;
    try {
        mainPid = m_bus.unitProperty(path, QString::fromLatin1(kServiceIface), QStringLiteral("MainPID"), remaining(d, "MainPID")).toUInt();
    } catch (const BusError&) {
        mainPid = 0;
    }
    const bool idle = active == QLatin1String("inactive") || active == QLatin1String("failed");
    if (idle && procs.isEmpty()) {
        q.state = load == QLatin1String("not-found") ? ServiceQuery::Absent : ServiceQuery::Stopped;
        q.pid = 0;
        // How the last run ended: the main process's own exit code, when it exited (ExecMainCode
        // CLD_EXITED = 1), so a stop explains itself as on Windows (exit 3, 4, 6 ...). A signal, or no run yet: unset.
        // Best effort: a property systemd does not answer leaves the code unset, as before.
        if (q.state == ServiceQuery::Stopped) {
            try {
                const int how = m_bus.unitProperty(path, QString::fromLatin1(kServiceIface), QStringLiteral("ExecMainCode"), remaining(d, "ExecMainCode")).toInt();
                if (how == 1)
                    q.exitCode = m_bus.unitProperty(path, QString::fromLatin1(kServiceIface), QStringLiteral("ExecMainStatus"), remaining(d, "ExecMainStatus")).toInt();
            } catch (const std::exception&) {
                // A bus error, or the deadline passing (remaining() throws): never fail the query for a diagnostic
                // (the engine-free proof reads this query).
                q.exitCode.reset();
            }
        }
        return q;
    }
    // Some process exists, or systemd is mid-transition: never "Stopped" (the engine-free proof needs both).
    q.pid = mainPid != 0 ? qint64(mainPid) : (procs.isEmpty() ? 0 : qint64(procs.first().pid));
    if (active == QLatin1String("active") || active == QLatin1String("reloading")) q.state = ServiceQuery::Running;
    else if (active == QLatin1String("activating")) q.state = ServiceQuery::StartPending;
    else if (active == QLatin1String("deactivating")) q.state = ServiceQuery::StopPending;
    else q.state = ServiceQuery::Other;
    return q;
}

Installation SystemdService::detect(Deadline)
{
    using E = InstallationEvidence::LinuxEvidence::EtcUnit;
    InstallationEvidence::LinuxEvidence ev;
    auto readRegular = [&](const QString& path, QByteArray* out) -> int {   // 1 read, 0 missing, -1 other
        const QByteArray p = path.toLocal8Bit();
        struct stat st {};
        if (m_sys.fstatat(AT_FDCWD, p.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) return 0;
            fail(QStringLiteral("cannot examine %1: %2").arg(path, errnoText()));
        }
        if (S_ISLNK(st.st_mode)) return 2;
        if (!S_ISREG(st.st_mode)) return -1;
        Fd fd(m_sys, m_sys.open(p.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        if (!fd) fail(QStringLiteral("cannot read %1: %2").arg(path, errnoText()));
        QString err;
        const std::optional<QByteArray> b = preadAll(m_sys, fd.get(), 256 * 1024, &err);
        if (!b) fail(QStringLiteral("cannot read %1: %2").arg(path, err));
        if (out) *out = *b;
        return 1;
    };
    const QString etc = m_opt.etcDirectory + QLatin1Char('/') + m_opt.unitName;
    switch (readRegular(etc, &ev.etcUnitContent)) {
    case 0: ev.etcUnit = E::Absent; break;
    case 1: ev.etcUnit = E::RegularFile; break;
    case 2: {
        char buf[256];
        const QByteArray p = etc.toLocal8Bit();
        const ssize_t n = m_sys.readlinkat(AT_FDCWD, p.constData(), buf, sizeof buf);
        ev.etcUnit = (n > 0 && QByteArray(buf, int(n)) == "/dev/null") ? E::MaskLink : E::Other;
        break;
    }
    default: ev.etcUnit = E::Other; break;
    }
    const int qt = readRegular(m_opt.unitDirectory + QLatin1Char('/') + m_opt.unitName, &ev.qtUnitContent);
    ev.qtUnitPresent = qt != 0;   // a link or anything else there is "present", and not ours (Foreign)
    if (qt != 1) ev.qtUnitContent.clear();
    const ProgramEvidence program = readProgram(m_sys, m_opt.binaryPath, m_opt.owner.uid, m_programCache, m_opt.wallClock);
    ev.binaryPresent = program.present;
    ev.programUnreadable = program.unreadable;
    ev.installedSha256 = program.sha256;
    ev.thisBuildKnown = !m_opt.expectedSha256.isEmpty();
    ev.programDigestOk = ev.thisBuildKnown && !program.sha256.isEmpty() &&
                         program.sha256.toLatin1() == m_opt.expectedSha256.toLower();
    if (program.present && !program.unreadable) ev.installedVersion = program.version;
    const QByteArray lock = (m_opt.lockDirectory + QStringLiteral("/engine.lock")).toLocal8Bit();
    const QByteArray rec = (m_opt.lockDirectory + QStringLiteral("/engine.lock.id")).toLocal8Bit();
    struct stat a {}, b {};
    ev.lockFilesPresent = m_sys.fstatat(AT_FDCWD, lock.constData(), &a, AT_SYMLINK_NOFOLLOW) == 0 &&
                          m_sys.fstatat(AT_FDCWD, rec.constData(), &b, AT_SYMLINK_NOFOLLOW) == 0;
    return InstallationEvidence::classifyLinux(ev);
}

// ---------------------------------------------------------------------------------------------------
// Start type (B1, R9)
// ---------------------------------------------------------------------------------------------------

void SystemdService::syncUnitDirectories()
{
    // The mask link (or the enable link) is durable before anything else happens (INV-77, INV-48).
    for (const QString& dir : {m_opt.etcDirectory, m_opt.etcDirectory + QStringLiteral("/multi-user.target.wants")}) {
        const QByteArray p = dir.toLocal8Bit();
        Fd fd(m_sys, m_sys.open(p.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!fd) {
            if (errno == ENOENT) continue;
            fail(QStringLiteral("cannot open %1: %2").arg(dir, errnoText()));
        }
        if (m_sys.fsync(fd.get()) != 0) fail(QStringLiteral("cannot flush %1: %2").arg(dir, errnoText()));
    }
}

void SystemdService::runSteps(bool block, Deadline d)
{
    using K = ServicePlan::Step::Kind;
    const QStringList unit{m_opt.unitName};
    const auto steps = block ? ServicePlan::blockStarts(ServicePlan::Os::Linux) : ServicePlan::allowStarts(ServicePlan::Os::Linux);
    for (const ServicePlan::Step& s : steps) {
        switch (s.kind) {
        case K::MaskUnit: m_bus.maskUnitFiles(unit, false, false, remaining(d, "MaskUnitFiles")); break;
        case K::DisableUnit: m_bus.disableUnitFiles(unit, false, remaining(d, "DisableUnitFiles")); break;
        case K::UnmaskUnit: m_bus.unmaskUnitFiles(unit, false, remaining(d, "UnmaskUnitFiles")); break;
        case K::EnableUnit: m_bus.enableUnitFiles(unit, false, false, remaining(d, "EnableUnitFiles")); break;
        case K::ReloadManager: m_bus.reload(remaining(d, "Reload")); break;
        case K::SyncUnitDirectory: syncUnitDirectories(); break;
        default: fail(QStringLiteral("a Windows step in the Linux plan"));
        }
    }
}

void SystemdService::inhibitStarts(Deadline d)
{
    requireRoot();
    runSteps(true, d);
    verify("LoadState", QStringLiteral("masked"), d);
}

void SystemdService::allowStarts(Deadline d)
{
    requireRoot();
    step("before-allow-starts");
    runSteps(false, d);
    verify("LoadState", QStringLiteral("loaded"), d);
    verify("UnitFileState", QStringLiteral("enabled"), d);
}

void SystemdService::requestStop(Deadline d)
{
    requireRoot();
    try {
        m_bus.stopUnit(m_opt.unitName, QStringLiteral("replace"), remaining(d, "StopUnit"));   // not awaited
    } catch (const BusError& e) {
        if (!e.noSuchUnit()) throw;   // not loaded / not installed: nothing to stop (INV-79)
    }
}

void SystemdService::start(Deadline d)
{
    requireRoot();
    step("before-start");
    awaitJob("start the service",
             [&](Ms t) { return m_bus.startUnit(m_opt.unitName, QStringLiteral("replace"), t); }, d);
    verify("ActiveState", QStringLiteral("active"), d);
}

// ---------------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------------

namespace {

// Opens `path` as a directory, creating any missing component (0755, owned by `owner`). Every component is
// opened O_NOFOLLOW relative to the previous one and judged on its fd: a directory owned by root
// or by `owner`, and not group- or other-writable unless sticky (EngineLock's rule), since what is written below
// it is run by systemd. A directory it creates is secured (fchown, fchmod) and its entry flushed, and any
// failure there fails the call. -1 with *why on failure; *created lists what it made, also on failure.
QString groupName(gid_t gid)
{
    struct group g {};
    struct group* found = nullptr;
    char buf[4096];
    if (::getgrgid_r(gid, &g, buf, sizeof buf, &found) == 0 && found && found->gr_name)
        return QString::fromLocal8Bit(found->gr_name);
    return QStringLiteral("gid %1").arg(qulonglong(gid));
}

// What the directories on the way to an installed file must be: owned by root (or the configured owner) and
// not group- or other-writable unless sticky (EngineLock's rule), since systemd runs what is installed below them
//. Findings are collected over every directory that exists, so that a refusal names the whole
// remedy: Debian's old layout has /usr/local AND /usr/local/bin at 2775 root:staff, kept by upgraded Pi OS hosts.
struct PathFindings {
    QStringList writable, details, wrongOwner;
    QStringList notAFile;   // an entry install would remove that is not a regular file (named whatever else is found)
    bool group = false, other = false;
    bool any() const { return !writable.isEmpty() || !wrongOwner.isEmpty() || !notAFile.isEmpty(); }
    void judge(const struct stat& st, const QString& path, const PosixOwner& owner)
    {
        if (st.st_uid != 0 && st.st_uid != owner.uid) {
            if (!wrongOwner.contains(path)) wrongOwner << QStringLiteral("%1 is owned by uid %2, not by root").arg(path).arg(qulonglong(st.st_uid));
            return;
        }
        if (!(st.st_mode & (S_IWGRP | S_IWOTH)) || (st.st_mode & S_ISVTX) || writable.contains(path)) return;
        writable << path;
        group = group || (st.st_mode & S_IWGRP);
        other = other || (st.st_mode & S_IWOTH);
        details << QStringLiteral("%1 is mode %2, writable by %3")
                       .arg(path)
                       .arg(st.st_mode & 07777, 4, 8, QLatin1Char('0'))
                       .arg((st.st_mode & S_IWGRP) ? QStringLiteral("group %1").arg(groupName(st.st_gid)) : QStringLiteral("every account"));
    }
    QString message() const
    {
        QStringList parts;
        if (!wrongOwner.isEmpty())
            parts << wrongOwner.join(QStringLiteral("; "));
        else if (!writable.isEmpty())
            parts << QStringLiteral("%1 can be changed by other accounts (%2), and systemd runs what is installed below it; run: chmod %3-w %4")
                         .arg(writable.first(), details.join(QStringLiteral("; ")),
                              QString(group ? QStringLiteral("g") : QString()) + (other ? QStringLiteral("o") : QString()),
                              writable.join(QLatin1Char(' ')));
        parts << notAFile;
        return parts.join(QStringLiteral("; "));
    }
};

// Read-only: judges every component of `path` that exists (stops at the first missing one).
void judgeExisting(PosixSys& sys, const QString& path, const PosixOwner& owner, PathFindings& f)
{
    Fd cur(sys, sys.open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    QString sofar;
    for (const QString& part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (!cur) return;
        sofar += QLatin1Char('/') + part;
        const QByteArray n = part.toLocal8Bit();
        Fd next(sys, sys.openat(cur.get(), n.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next) return;   // missing (install creates it) or unopenable (the step itself reports it)
        struct stat st {};
        if (sys.fstat(next.get(), &st) != 0) return;
        f.judge(st, sofar, owner);
        cur = std::move(next);
    }
}

// Opens `path` as a directory, creating any missing component (0755, owned by `owner`). Every component is
// opened O_NOFOLLOW relative to the previous one and judged on its fd (PathFindings). A directory it creates
// is secured (fchown, fchmod) and its entry flushed, and any failure there fails the call; one that appeared
// between the open and the mkdir (EEXIST) is someone else's and is only judged. -1 with *why on
// failure; *created lists what it made, also on failure.
int openDirectoryCreating(PosixSys& sys, const QString& path, const PosixOwner& owner, QStringList* created, QString* why)
{
    const auto failWith = [&](const QString& what) {
        *why = QStringLiteral("%1: %2").arg(what, errnoText());
        return -1;
    };
    Fd cur(sys, sys.open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!cur) return failWith(QStringLiteral("cannot open /"));
    QString sofar;
    PathFindings findings;
    for (const QString& part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        sofar += QLatin1Char('/') + part;
        const QByteArray n = part.toLocal8Bit();
        Fd next(sys, sys.openat(cur.get(), n.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next && errno == ENOENT && findings.any()) break;   // nothing is created below a refused directory
        if (!next && errno == ENOENT) {
            if (sys.mkdirat(cur.get(), n.constData(), 0755) == 0) {
                next = Fd(sys, sys.openat(cur.get(), n.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                if (!next) return failWith(QStringLiteral("cannot open %1").arg(sofar));
                created->append(sofar);
                if (sys.fchown(next.get(), owner.uid, owner.gid) != 0) return failWith(QStringLiteral("cannot set the owner of %1").arg(sofar));
                if (sys.fchmod(next.get(), 0755) != 0) return failWith(QStringLiteral("cannot set the mode of %1").arg(sofar));
                if (sys.fsync(cur.get()) != 0) return failWith(QStringLiteral("cannot flush the directory that holds %1").arg(sofar));
            } else if (errno == EEXIST) {
                next = Fd(sys, sys.openat(cur.get(), n.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            } else {
                return failWith(QStringLiteral("cannot create %1").arg(sofar));
            }
        }
        if (!next) return failWith(QStringLiteral("cannot open %1").arg(sofar));
        struct stat st {};
        if (sys.fstat(next.get(), &st) != 0) return failWith(QStringLiteral("cannot examine %1").arg(sofar));
        findings.judge(st, sofar, owner);
        if (!findings.wrongOwner.isEmpty()) break;
        cur = std::move(next);
    }
    if (findings.any()) {
        *why = findings.message();
        return -1;
    }
    return cur.release();
}

}  // namespace

int SystemdService::openDirectories(const QString& path)
{
    QStringList created;
    QString why;
    const int fd = openDirectoryCreating(m_sys, path, m_opt.owner, &created, &why);
    for (const QString& c : created) m_changes << QStringLiteral("created %1").arg(c);   // also when it failed
    if (fd < 0) fail(QStringLiteral("cannot open or create %1: %2").arg(path, why));
    return fd;
}

void SystemdService::writeFile(const QString& directory, const QString& name, const QByteArray& bytes, mode_t mode)
{
    const int dfd = openDirectories(directory);
    PosixDirHandle dir(m_sys, dfd);
    PosixFileIo io(m_sys, m_opt.owner, mode);
    const WriteResult w = atomicWrite(io, dir, name, bytes, OwnerPolicy::Config);
    if (w.replaced) m_changes << QStringLiteral("wrote %1/%2").arg(directory, name);
    if (!w.replaced || !w.durable) fail(QStringLiteral("cannot write %1/%2: %3").arg(directory, name, w.error));
}

void SystemdService::copyBinary()
{
    const QByteArray src = m_opt.sourceBinary.toLocal8Bit();
    Fd in(m_sys, m_sys.open(src.constData(), O_RDONLY | O_CLOEXEC));
    if (!in) fail(QStringLiteral("cannot open the program to install (%1): %2").arg(m_opt.sourceBinary, errnoText()));
    struct stat sst {};
    if (m_sys.fstat(in.get(), &sst) != 0 || !S_ISREG(sst.st_mode))
        fail(QStringLiteral("the program to install (%1) is not a regular file").arg(m_opt.sourceBinary));
    const QString dirPath = parentOf(m_opt.binaryPath), name = nameOf(m_opt.binaryPath);
    const int dfd = openDirectories(dirPath);
    PosixDirHandle dir(m_sys, dfd);
    struct stat dst {};
    const QByteArray n = name.toLocal8Bit();
    // Running the installed copy: it is already there, but only an inode that is ALSO what install would make
    // (the owner's, 0755) may be kept; anything else (a 0700 copy the DynamicUser service cannot run) is replaced
    // atomically from itself, through the fd opened above.
    if (m_sys.fstatat(dfd, n.constData(), &dst, AT_SYMLINK_NOFOLLOW) == 0 && dst.st_dev == sst.st_dev && dst.st_ino == sst.st_ino &&
        S_ISREG(dst.st_mode) && dst.st_uid == m_opt.owner.uid && dst.st_gid == m_opt.owner.gid && (dst.st_mode & 07777) == 0755)
        return;
    PosixFileIo io(m_sys, m_opt.owner, 0755);
    QString err;
    std::unique_ptr<TempFile> t = io.createTemp(dir, OwnerPolicy::Config, defaultNameGen(name), &err);
    if (!t) fail(QStringLiteral("cannot install %1: %2").arg(m_opt.binaryPath, err));
    QByteArray chunk(1 << 20, Qt::Uninitialized);
    for (;;) {
        const ssize_t r = m_sys.read(in.get(), chunk.data(), size_t(chunk.size()));
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) fail(QStringLiteral("cannot read %1: %2").arg(m_opt.sourceBinary, errnoText()));
        if (r == 0) break;
        if (!t->write(QByteArray::fromRawData(chunk.constData(), int(r)), &err))
            fail(QStringLiteral("cannot install %1: %2").arg(m_opt.binaryPath, err));
    }
    if (!t->flush(&err)) fail(QStringLiteral("cannot install %1: %2").arg(m_opt.binaryPath, err));
    const TempFile::Commit c = t->commitAs(name);
    if (c.replaced) m_changes << QStringLiteral("installed %1").arg(m_opt.binaryPath);
    if (!c.replaced || !c.durable) fail(QStringLiteral("cannot install %1: %2").arg(m_opt.binaryPath, c.error));
}

void SystemdService::ensureLockDirectory()
{
    // Created root:root 0755 when missing; an existing directory is never changed here (EngineLock judges it:
    // a wrong one is refused, never repaired, INV-116).
    const QString parent = parentOf(m_opt.lockDirectory);
    Fd p(m_sys, openDirectories(parent));
    const QByteArray n = nameOf(m_opt.lockDirectory).toLocal8Bit();
    if (m_sys.mkdirat(p.get(), n.constData(), 0755) != 0) {
        if (errno == EEXIST) return;
        fail(QStringLiteral("cannot create %1: %2").arg(m_opt.lockDirectory, errnoText()));
    }
    m_changes << QStringLiteral("created %1").arg(m_opt.lockDirectory);   // made: reported even if what follows fails
    Fd d(m_sys, m_sys.openat(p.get(), n.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!d) fail(QStringLiteral("cannot open the new %1: %2").arg(m_opt.lockDirectory, errnoText()));
    if (m_sys.fchown(d.get(), m_opt.owner.uid, m_opt.owner.gid) != 0 || m_sys.fchmod(d.get(), 0755) != 0)
        fail(QStringLiteral("cannot secure %1: %2").arg(m_opt.lockDirectory, errnoText()));
    if (m_sys.fsync(p.get()) != 0) fail(QStringLiteral("cannot flush %1: %2").arg(parent, errnoText()));
}

void SystemdService::installOrConfigure(InstallState state, Deadline d)
{
    requireRoot();
    if (state == InstallState::Foreign)
        fail(QStringLiteral("refusing to install over an installation this program did not make"));
    // Before any change: every directory install writes below is judged, and a refusal names the whole remedy at
    // once, instead of masking the unit and then failing on the first directory found.
    {
        PathFindings f;
        judgeExisting(m_sys, m_opt.unitDirectory, m_opt.owner, f);
        judgeExisting(m_sys, parentOf(m_opt.binaryPath), m_opt.owner, f);
        if (!m_opt.policy.isEmpty()) judgeExisting(m_sys, parentOf(m_opt.policyPath), m_opt.owner, f);
        if (legacyPolicyApplies()) {
            // The legacy policy install removes (removeLegacyPolicy): its directory judged like the policy's (the same
            // one by default), and anything there but a regular file refused now, before the mask.
            if (parentOf(m_opt.legacyPolicyPath) != parentOf(m_opt.policyPath))
                judgeExisting(m_sys, parentOf(m_opt.legacyPolicyPath), m_opt.owner, f);
            struct stat st {};
            const QByteArray lp = m_opt.legacyPolicyPath.toLocal8Bit();
            if (m_sys.fstatat(AT_FDCWD, lp.constData(), &st, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISREG(st.st_mode))
                f.notAFile << QStringLiteral("%1 is not a regular file (an older release's polkit policy goes there); remove it")
                                  .arg(m_opt.legacyPolicyPath);
        }
        judgeExisting(m_sys, parentOf(m_opt.lockDirectory), m_opt.owner, f);
        if (f.any()) fail(QStringLiteral("cannot install: %1").arg(f.message()));
    }
    // The mask exists before the unit file can (INV-79): a unit that is never visible unmasked.
    m_bus.maskUnitFiles({m_opt.unitName}, false, false, remaining(d, "MaskUnitFiles"));
    syncUnitDirectories();
    step("after-mask");
    writeFile(m_opt.unitDirectory, m_opt.unitName, SystemdUnit::render(m_opt.binaryPath), 0644);
    copyBinary();
    if (!m_opt.policy.isEmpty()) writeFile(parentOf(m_opt.policyPath), nameOf(m_opt.policyPath), m_opt.policy, 0644);
    removeLegacyPolicy();   // after the current policy is written: pkexec always has the action
    ensureLockDirectory();
    m_bus.reload(remaining(d, "Reload"));
    verify("LoadState", QStringLiteral("masked"), d);
}

void SystemdService::removeFile(const QString& path)
{
    const QByteArray dir = parentOf(path).toLocal8Bit();
    Fd dfd(m_sys, m_sys.open(dir.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!dfd) {
        if (errno == ENOENT) return;
        fail(QStringLiteral("cannot open %1: %2").arg(parentOf(path), errnoText()));
    }
    const QByteArray n = nameOf(path).toLocal8Bit();
    struct stat st {};
    if (m_sys.fstatat(dfd.get(), n.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return;
        fail(QStringLiteral("cannot examine %1: %2").arg(path, errnoText()));
    }
    if (!S_ISREG(st.st_mode)) fail(QStringLiteral("%1 is not a regular file; not removed").arg(path));
    if (m_sys.unlinkat(dfd.get(), n.constData(), 0) != 0) fail(QStringLiteral("cannot remove %1: %2").arg(path, errnoText()));
    m_changes << QStringLiteral("removed %1").arg(path);
    if (m_sys.fsync(dfd.get()) != 0) fail(QStringLiteral("removed %1, but %2 cannot be flushed: %3").arg(path, parentOf(path), errnoText()));
}

bool SystemdService::legacyPolicyApplies() const
{
    return !m_opt.policy.isEmpty() && !m_opt.legacyPolicyPath.isEmpty() && m_opt.legacyPolicyPath != m_opt.policyPath;
}

void SystemdService::removeLegacyPolicy()
{
    // removeFile's rule: a regular file goes (reported "removed"), a missing one is nothing, anything else fails the step
    // and is named. On install, the pre-change scan has already refused a non-file there and judged its directory; this
    // late check only catches one that appeared since. Uninstall judges no directory, as for the other files it removes.
    if (legacyPolicyApplies()) removeFile(m_opt.legacyPolicyPath);
}

void SystemdService::uninstall(Deadline d)
{
    requireRoot();
    try {
        m_bus.stopUnit(m_opt.unitName, QStringLiteral("replace"), remaining(d, "StopUnit"));
    } catch (const BusError& e) {
        if (!e.noSuchUnit()) throw;
    }
    // DisableUnitFiles refuses a unit file that does not exist ("Unit file ... does not exist", systemd 252). It is gone
    // when an earlier uninstall removed it and then failed on the program: that uninstall disabled it first, so this
    // one, which finishes it, skips the call. UnmaskUnitFiles accepts a missing unit.
    const QString unitFile = m_opt.unitDirectory + QLatin1Char('/') + m_opt.unitName;
    struct stat unitSt {};
    const bool unitFilePresent = m_sys.fstatat(AT_FDCWD, unitFile.toLocal8Bit().constData(), &unitSt, AT_SYMLINK_NOFOLLOW) == 0;
    if (unitFilePresent) m_bus.disableUnitFiles({m_opt.unitName}, false, remaining(d, "DisableUnitFiles"));
    m_bus.unmaskUnitFiles({m_opt.unitName}, false, remaining(d, "UnmaskUnitFiles"));
    syncUnitDirectories();
    removeFile(unitFile);
    removeFile(m_opt.binaryPath);
    if (!m_opt.policy.isEmpty()) removeFile(m_opt.policyPath);
    removeLegacyPolicy();
    // The lock directory: our files, then the directory itself if nothing else is in it.
    {
        const QByteArray ld = m_opt.lockDirectory.toLocal8Bit();
        Fd dfd(m_sys, m_sys.open(ld.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (dfd) {
            const QStringList names = m_sys.listDirectory(dfd.get()).value_or(QStringList{});
            for (const QString& n : names)
                if (n == QLatin1String("engine.lock") || n == QLatin1String("engine.lock.id") || isTempName(n))
                    removeFile(m_opt.lockDirectory + QLatin1Char('/') + n);
            dfd.reset();
            const QByteArray parent = parentOf(m_opt.lockDirectory).toLocal8Bit();
            Fd p(m_sys, m_sys.open(parent.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            const QByteArray n = nameOf(m_opt.lockDirectory).toLocal8Bit();
            if (p && m_sys.unlinkat(p.get(), n.constData(), AT_REMOVEDIR) == 0) {
                m_changes << QStringLiteral("removed %1").arg(m_opt.lockDirectory);
                if (m_sys.fsync(p.get()) != 0)
                    fail(QStringLiteral("removed %1, but its parent cannot be flushed: %2").arg(m_opt.lockDirectory, errnoText()));
            }
        }
    }
    removeStagingRoot();
    m_bus.reload(remaining(d, "Reload"));
    verify("LoadState", QStringLiteral("not-found"), d);
}

void SystemdService::removeStagingRoot()
{
    if (m_opt.stagingRoot.isEmpty()) return;
    const QByteArray rootPath = m_opt.stagingRoot.toLocal8Bit();
    Fd root(m_sys, m_sys.open(rootPath.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!root) {
        if (errno != ENOENT) m_changes << QStringLiteral("kept %1 (%2)").arg(m_opt.stagingRoot, errnoText());
        return;
    }
    QStringList kept;
    for (const QString& name : m_sys.listDirectory(root.get()).value_or(QStringList{})) {
        const QByteArray n = name.toLocal8Bit();
        struct stat st {};
        const QString path = m_opt.stagingRoot + QLatin1Char('/') + name;
        if (!name.startsWith(QLatin1String("staging.")) || m_sys.fstatat(root.get(), n.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISDIR(st.st_mode)) {
            kept << name;
            continue;
        }
        Fd dir(m_sys, m_sys.openat(root.get(), n.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!dir) {
            kept << name;
            continue;
        }
        bool clean = true;
        for (const QString& inner : m_sys.listDirectory(dir.get()).value_or(QStringList{})) {
            const QByteArray i = inner.toLocal8Bit();
            struct stat is {};
            if (m_sys.fstatat(dir.get(), i.constData(), &is, AT_SYMLINK_NOFOLLOW) != 0 || S_ISDIR(is.st_mode) ||
                m_sys.unlinkat(dir.get(), i.constData(), 0) != 0)
                clean = false;
        }
        dir.reset();
        if (clean && m_sys.unlinkat(root.get(), n.constData(), AT_REMOVEDIR) == 0) m_changes << QStringLiteral("removed %1").arg(path);
        else kept << name;
    }
    root.reset();
    if (!kept.isEmpty()) {
        m_changes << QStringLiteral("kept %1 (it still holds %2)").arg(m_opt.stagingRoot, kept.join(QStringLiteral(", ")));
        return;
    }
    const QByteArray parent = parentOf(m_opt.stagingRoot).toLocal8Bit();
    Fd p(m_sys, m_sys.open(parent.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    const QByteArray n = nameOf(m_opt.stagingRoot).toLocal8Bit();
    if (p && m_sys.unlinkat(p.get(), n.constData(), AT_REMOVEDIR) == 0) {
        m_changes << QStringLiteral("removed %1").arg(m_opt.stagingRoot);
        m_sys.fsync(p.get());
    } else {
        m_changes << QStringLiteral("kept %1 (%2)").arg(m_opt.stagingRoot, errnoText());
    }
}

}  // namespace asb::posix
