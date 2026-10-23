#include "cli/Helper.h"

#include "cli/HelperInput.h"
#include "cli/StagedHelper.h"
#include "core/ConfigJson.h"
#include "core/OperationHost.h"

#include <QCoreApplication>
#include <QEventLoop>

#include <cerrno>
#include <climits>
#include <cstring>

#include <poll.h>
#include <syslog.h>
#include <unistd.h>

namespace asb::cli {
namespace {

using namespace helper;

constexpr std::chrono::seconds kInspectBudget{15};

// The running program's canonical path: the Host's ownExecutable(), and when that is the kernel's /proc/self/exe link,
// its target (already canonical: no "..", no symlink; a deleted file reads "... (deleted)" and matches nothing).
QString runningProgram(Host& h)
{
    const QString own = h.ownExecutable();
    if (own != QLatin1String("/proc/self/exe")) return own;
    char buf[PATH_MAX + 1];
    const ssize_t n = ::readlink("/proc/self/exe", buf, PATH_MAX);
    return n > 0 ? QString::fromLocal8Bit(buf, qsizetype(n)) : QString();
}

// The QCoreApplication the worker and QtDBus need: created only once the request was accepted.
void ensureApplication()
{
    if (QCoreApplication::instance()) return;
    static int argc = 1;
    static char name[] = "aether-serial-bridge";
    static char* argv[] = {name, nullptr};
    new QCoreApplication(argc, argv);
}

// The response, whole (short writes and EINTR retried; a nonblocking descriptor waited for).
bool writeAll(int fd, const QByteArray& bytes)
{
    qsizetype done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.constData() + done, size_t(bytes.size() - done));
        if (n > 0) {
            done += n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p{};
            p.fd = fd;
            p.events = POLLOUT;
            ::poll(&p, 1, 1000);
            continue;
        }
        return false;
    }
    return true;
}

// Who asked, for the audit line only (INV-50: never authority). pkexec sets PKEXEC_UID to the caller's uid.
QString requester()
{
    const QByteArray v = qgetenv("PKEXEC_UID");
    bool digits = !v.isEmpty() && v.size() <= 10;
    for (const char ch : v) digits = digits && ch >= '0' && ch <= '9';
    return digits ? QStringLiteral("uid %1").arg(QString::fromLatin1(v)) : QStringLiteral("a caller pkexec did not name");
}

class Waiter final : public IOperationObserver {
public:
    Waiter(Console& c, QEventLoop& loop) : m_c(c), m_loop(loop) {}
    void interim(const Interim& i) override
    {
        // stderr only: the single JSON response comes after `finished` (D57, INV-118).
        m_c.err(i.message.isEmpty() ? QStringLiteral("Still processing (%1)...").arg(i.phase) : i.message);
    }
    void finished(const Result& r) override
    {
        final = r;
        m_loop.quit();
    }
    std::optional<Result> final;

private:
    Console& m_c;
    QEventLoop& m_loop;
};

Result runOperation(Host& h, Platform& p, Console& c, const Request& req)
{
    QEventLoop loop;
    OperationHost host(p, h.clock(), h.interimPoll());
    Waiter w(c, loop);
    bool submitted = false;
    switch (req.op) {
    case Op::SaveAndApply: submitted = host.submitRun(req.save, /*install=*/false, /*repairFirst=*/false, w); break;
    case Op::Deactivate: submitted = host.submitDeactivate(req.save, w); break;
    case Op::Start: submitted = host.submitStart(w); break;
    case Op::Stop: submitted = host.submitStop(w); break;
    case Op::Repair: submitted = host.submitRepair(w); break;
    case Op::DiscardPending: submitted = host.submitDiscardPending(w); break;
    case Op::Install: submitted = host.submitRun(req.save, /*install=*/true, /*repairFirst=*/false, w); break;
    case Op::Uninstall: submitted = host.submitUninstall(UninstallOptions{req.removeSettings}, w); break;
    case Op::DiscardPendingIf: submitted = host.submitDiscardPendingIf(req.sha256, w); break;
    case Op::Inspect: break;
    }
    if (!submitted) {
        Result r;
        r.message = QStringLiteral("The settings and the service were not changed: the operation could not be started.");
        return r;
    }
    // `finished` is delivered here after the worker returned and released the locks; only then is the response
    // written (INV-118).
    while (!w.final) loop.exec();
    return *w.final;
}

// inspect: read only, no lock, no operation. The folder's ReadOnly decision and the service as root sees it.
int inspect(Host& h, Platform& p, const HelperOptions& o)
{
    InspectReport rep;
    bool ok = true;
    try {
        const Inspection i = p.folder.inspect(TrustMode::ReadOnly);
        rep.trust = trustName(i.action);
        rep.reason = i.reason;
    } catch (const std::exception& e) {
        ok = false;
        rep.trust = trustName(DirAction::Refuse);
        rep.reason = QStringLiteral("the data folder could not be inspected: %1").arg(QString::fromLocal8Bit(e.what()));
    }
    const Deadline deadline = h.clock().now() + kInspectBudget;
    rep.installation = rep.service = QStringLiteral("Unknown");
    try {
        const Installation inst = p.service.detect(deadline);
        rep.installation = installationName(inst.state);
        if (inst.state == InstallState::Current || inst.state == InstallState::IncompleteInstall)
            rep.service = serviceName(p.service.query(deadline).state);
        else if (inst.state == InstallState::NotInstalled)
            rep.service = serviceName(ServiceQuery::Absent);
    } catch (const std::exception&) {
        // "Unknown" stays: the query failed (as --status: what can be said is still said).
    }
    writeAll(o.outputFd, encodeInspect(rep, ok));
    return ok ? 0 : 1;
}

int refuse(Console& c, const QString& why)
{
    c.err(QStringLiteral("error: %1; nothing was changed.").arg(why));
    return 2;
}

}  // namespace

int runHelper(Host& h, Console& c, const HelperOptions& o)
{
    // 1. Mode entry (INV-112): the guard before anything else exists.
    h.enterGuardedMode();
    h.guardProbe();
    // 2. Root, and nothing else (INV-50).
    if (!h.elevated()) return refuse(c, QStringLiteral("--helper runs only as root, started by pkexec"));
    // 3. The request (INV-89): bounded, then exactly one value. No application, Platform or lock exists yet.
    const ReadResult in = readBounded(o.inputFd, o.maxRequest, o.inputDeadline);
    switch (in.status) {
    case ReadResult::Status::Ok: break;
    case ReadResult::Status::TooLarge:
        return refuse(c, QStringLiteral("the request is too large (more than %1 bytes)").arg(o.maxRequest));
    case ReadResult::Status::Deadline:
        return refuse(c, QStringLiteral("the request did not end within %1 s").arg(double(o.inputDeadline.count()) / 1000.0));
    case ReadResult::Status::Error: return refuse(c, QStringLiteral("could not read the request: %1").arg(in.error));
    }
    QString why;
    const std::optional<Request> req = parseRequest(in.bytes, &why);
    if (!req) return refuse(c, QStringLiteral("the request is not valid: %1").arg(why));
    // INV-123: install and uninstall only in the window's staged program (D60), never in an installed helper behind the
    // bridge's own auth_admin action (no Platform is built for the refusal, like every refusal above).
    const bool isStaged = staged::isStagedProgramPath(runningProgram(h));
    if ((req->op == Op::Install || req->op == Op::Uninstall) && !isStaged)
        return refuse(c, QStringLiteral("install and uninstall run only from the window's staged program"));

    // 4. The audit line, then the application and the Platform.
    QString audit = QStringLiteral("aether-serial-bridge --helper%1: %2 requested by %3")
                        .arg(isStaged ? QStringLiteral(", staged") : QString(), opName(req->op), requester());
    if (req->save) audit += QStringLiteral(" (config sha256 %1)").arg(sha256Hex(req->save->config));
    c.err(audit);
    if (o.syslog) {
        ::openlog("aether-serial-bridge", LOG_PID, LOG_AUTHPRIV);
        ::syslog(LOG_NOTICE, "%s", audit.toUtf8().constData());
        ::closelog();
    }
    ensureApplication();
    QString error;
    int code = 1;
    Platform* p = h.platform(&error, &code);
    if (!p) {
        Result r;
        r.message = QStringLiteral("The settings and the service were not changed: %1.").arg(error);
        writeAll(o.outputFd, encodeResult(r));
        return 1;
    }
    if (req->op == Op::Inspect) return inspect(h, *p, o);

    // 5. The operation; the response only after `finished`.
    Result r = runOperation(h, *p, c, *req);
    // What the adapters reported outside Result (lock files, the unit, binary and policy) joins fileChanges once.
    for (const QString& line : h.takeReports())
        if (!r.message.contains(line) && !r.fileChanges.contains(line)) r.fileChanges << line;
    if (!writeAll(o.outputFd, encodeResult(r)))
        c.err(QStringLiteral("error: could not write the response: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
    return succeeded(r) ? 0 : 1;
}

}  // namespace asb::cli
