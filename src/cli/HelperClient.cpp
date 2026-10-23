#include "cli/HelperClient.h"

#include "core/ConfigJson.h"

#include <QTimer>

namespace asb::cli {
namespace {

using namespace helper;
using Kind = HelperClient::Outcome::Kind;

// What to quote from stderr: the helper's own "error: ..." line (the last one), else pkexec's first line ("Error
// executing command as another user: Not authorized"; pkexec adds "This incident has been reported." after it).
QString stderrLine(const QByteArray& err)
{
    const QList<QByteArray> lines = err.split('\n');
    QString first, error;
    for (const QByteArray& raw : lines) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty()) continue;
        if (first.isEmpty()) first = line;
        if (line.startsWith(QLatin1String("error: "))) error = line.mid(7);
    }
    QString l = error.isEmpty() ? first : error;
    for (const QLatin1String tail : {QLatin1String("; nothing was changed."), QLatin1String(".")})
        if (l.endsWith(tail)) l.chop(tail.size());
    return l;
}

QString detail(const QByteArray& err)
{
    const QString l = stderrLine(err);
    return l.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(l);
}

}  // namespace

HelperClient::HelperClient(IHelperProcess& p, std::chrono::milliseconds waitDeadline)
    : m_process(p), m_waitDeadline(waitDeadline)
{
}

QString HelperClient::waitingMessage()
{
    return QStringLiteral("Still working: the change is taking longer than usual. The window stays open until it ends; "
                          "the result will say what state the bridge is in.");
}

bool HelperClient::submit(const Request& req, quint64 revision, Done done, Waiting waiting)
{
    IHelperProcess& p = m_process;
    return submitVia(req, revision, Route::Installed, Context{QString(), m_nothingInstalled},
                     [&p](const QByteArray& request, std::function<void(const IHelperProcess::Exit&)> finished) {
                         p.run(request, std::move(finished));
                     },
                     std::move(done), std::move(waiting));
}

bool HelperClient::submitStaged(const Request& req, quint64 revision, const StagedProgram& program, Done done,
                                Waiting waiting)
{
    const Context ctx{staged::terminalCommand(program.commandProgram, req.op, req.removeSettings), true};
    // D60: the exact bytes that will be sent are this window's build, or nothing is launched (a corrupted or swapped
    // embedded program never reaches pkexec). Their digest was taken off the window's thread over this same buffer
    // (StagedProgram); a missing digest never matches. The script checks the digest again as root, over its own copy.
    if (program.bytesSha256.size() != 64 || program.bytesSha256 != program.expectedSha256) {
        if (m_busy) return false;
        m_busy = true;
        const quint64 gen = ++m_generation;
        const std::weak_ptr<int> alive = m_alive;
        Outcome o;
        o.op = req.op;
        o.route = Route::Staged;
        o.revision = revision;
        o.kind = Kind::Refused;
        o.result.message = o.message = fingerprintMismatchMessage();
        // Later, never inside the call (like a process that could not start).
        QTimer::singleShot(0, &m_context, [this, alive, gen, o, done] {
            if (alive.expired() || gen != m_generation || !m_busy) return;
            m_busy = false;
            ++m_generation;
            if (done) done(o);
        });
        return true;
    }
    IHelperProcess& p = m_process;
    const QByteArray bytes = program.bytes, expected = program.expectedSha256;
    return submitVia(req, revision, Route::Staged, ctx,
                     [&p, bytes, expected](const QByteArray& request, std::function<void(const IHelperProcess::Exit&)> finished) {
                         p.runStaged(expected, bytes, request, std::move(finished));
                     },
                     std::move(done), std::move(waiting));
}

bool HelperClient::submitVia(const Request& req, quint64 revision, Route route, const Context& ctx, Launch launch,
                             Done done, Waiting waiting)
{
    if (m_busy) return false;
    m_busy = true;
    const quint64 gen = ++m_generation;
    // The client's own digest of the exact bytes it sends (never the helper's echo alone; INV-107).
    const QByteArray sha = req.save ? sha256Hex(req.save->config).toLatin1() : QByteArray();
    if (waiting)
        QTimer::singleShot(m_waitDeadline, &m_context, [this, gen, waiting] {
            if (m_busy && gen == m_generation) waiting(waitingMessage());
        });
    // The process may finish after this client is gone (the window closed, the client destroyed): the completion then
    // does nothing. The sentinel lives exactly as long as the client.
    const std::weak_ptr<int> alive = m_alive;
    launch(encodeRequest(req), [this, alive, gen, req, sha, revision, route, ctx, done](const IHelperProcess::Exit& e) {
        if (alive.expired()) return;
        if (gen != m_generation || !m_busy) return;   // exactly once per submission
        m_busy = false;
        ++m_generation;   // a wait timer still pending for this submission is now stale
        if (done) done(interpret(req, sha, revision, e, route, ctx));
    });
    return true;
}

bool HelperClient::saveAndApply(const QByteArray& configBytes, const Source& source, bool supersedePending,
                                quint64 revision, Done done, Waiting waiting)
{
    Request r;
    r.op = Op::SaveAndApply;
    r.save = SaveRequest{configBytes, source, supersedePending};
    return submit(r, revision, std::move(done), std::move(waiting));
}

bool HelperClient::deactivate(const std::optional<QByteArray>& configBytes, const Source& source, bool supersedePending,
                              quint64 revision, Done done, Waiting waiting)
{
    Request r;
    r.op = Op::Deactivate;
    if (configBytes) r.save = SaveRequest{*configBytes, source, supersedePending};
    return submit(r, revision, std::move(done), std::move(waiting));
}

bool HelperClient::start(Done done, Waiting waiting)
{
    return submit(Request{Op::Start, std::nullopt}, 0, std::move(done), std::move(waiting));
}

bool HelperClient::stop(Done done, Waiting waiting)
{
    return submit(Request{Op::Stop, std::nullopt}, 0, std::move(done), std::move(waiting));
}

bool HelperClient::repair(Done done, Waiting waiting)
{
    return submit(Request{Op::Repair, std::nullopt}, 0, std::move(done), std::move(waiting));
}

bool HelperClient::discardPending(Done done, Waiting waiting)
{
    return submit(Request{Op::DiscardPending, std::nullopt}, 0, std::move(done), std::move(waiting));
}

bool HelperClient::inspect(Done done, Waiting waiting)
{
    return submit(Request{Op::Inspect, std::nullopt}, 0, std::move(done), std::move(waiting));
}

bool HelperClient::discardPendingIf(const QString& sha256, Done done, Waiting waiting)
{
    Request r;
    r.op = Op::DiscardPendingIf;
    r.sha256 = sha256;
    return submit(r, 0, std::move(done), std::move(waiting));
}

bool HelperClient::install(const std::optional<QByteArray>& configBytes, const Source& source, bool supersedePending,
                           quint64 revision, const StagedProgram& program, Done done, Waiting waiting)
{
    Request r;
    r.op = Op::Install;
    if (configBytes) r.save = SaveRequest{*configBytes, source, supersedePending};
    return submitStaged(r, revision, program, std::move(done), std::move(waiting));
}

bool HelperClient::uninstall(bool removeSettings, const StagedProgram& program, Done done, Waiting waiting)
{
    Request r;
    r.op = Op::Uninstall;
    r.removeSettings = removeSettings;
    return submitStaged(r, 0, program, std::move(done), std::move(waiting));
}

QString HelperClient::fingerprintMismatchMessage()
{
    return QStringLiteral("The program inside this window does not match its fingerprint; nothing was run.");
}

HelperClient::Outcome HelperClient::interpret(const Request& req, const QByteArray& submittedSha256, quint64 revision,
                                              const IHelperProcess::Exit& e, Route route, const Context& ctx)
{
    Outcome o;
    o.op = req.op;
    o.route = route;
    o.revision = revision;
    o.submittedSha256 = submittedSha256;
    // The helper did not act on the request: nothing changed, dirty kept.
    const auto unchanged = [&o](Kind k, const QString& message) {
        o.kind = k;
        o.result = Result{};
        o.result.message = o.message = message;
        return o;
    };
    // The helper may have acted: what state the bridge is in is not known, dirty kept.
    const auto unknown = [&o](const QString& why) {
        o.kind = Kind::Failed;
        o.result = Result{};
        o.result.outcome = asb::Outcome::StateUnknown;
        o.result.message = o.message =
            QStringLiteral("The helper ended without a result (%1); the bridge's state is not known. Refresh to see it.").arg(why);
        return o;
    };

    if (!e.started)
        return unchanged(Kind::Refused, QStringLiteral("Could not run pkexec%1; nothing was changed.").arg(detail(e.err)));
    if (e.crashed) return unknown(QStringLiteral("it was killed"));
    // INV-124: the terminal command that does the same thing, when there is one (it runs as shown).
    const QString terminal = ctx.terminalCommand.isEmpty() ? QString()
                                                           : QStringLiteral(" From a terminal: %1").arg(ctx.terminalCommand);
    if (route == Route::Staged) {
        // The script's own exits: nothing ran as the bridge. Only its "asb-stage:" line is read, never the helper's.
        const QString line = staged::stageLine(e.err);
        switch (e.code) {
        case staged::kDigestMismatch: return unchanged(Kind::Refused, fingerprintMismatchMessage());
        case staged::kNothingStaged: {
            QString m = line.isEmpty() ? QStringLiteral("Nothing was changed (the staging step gave no reason).")
                                       : QStringLiteral("Nothing was changed: %1.").arg(line);
            const QString dir = staged::unsafeDirectory(line);
            if (!dir.isEmpty())
                m += QStringLiteral(" An administrator can make it writable by root only (sudo chmod g-w,o-w %1), or use "
                                    "the terminal command: %2")
                         .arg(dir, ctx.terminalCommand.isEmpty() ? QStringLiteral("(none)") : ctx.terminalCommand);
            return unchanged(Kind::Refused, m);
        }
        case staged::kNotExecutable:
            return unchanged(Kind::Refused, QStringLiteral("The staged program could not be started (%1); nothing was changed.")
                                                .arg(line.isEmpty() ? QStringLiteral("no reason given") : line));
        default: break;
        }
    }
    switch (e.code) {
    case kPkexecDismissed: return unchanged(Kind::Cancelled, QStringLiteral("Cancelled; nothing was changed."));
    case kPkexecNotAuthorized: {
        // pkexec's stderr picks the message, never the outcome: both mean nothing ran (INV-124).
        const bool noAgent = e.err.contains("No authentication agent") || e.err.contains("Error creating textual authentication agent");
        if (noAgent)
            return unchanged(Kind::NoAgent, QStringLiteral("No polkit authentication agent is running in this desktop session, so "
                                                           "nothing could ask for a password. Nothing was changed.%1")
                                                .arg(terminal));
        const QString hint = route == Route::Installed && ctx.nothingInstalled
                                 ? QStringLiteral(" Is the bridge installed (sudo aether-serial-bridge --install)?")
                                 : QString();
        return unchanged(Kind::NotAuthorized, QStringLiteral("polkit did not allow this; nothing was changed.%1%2").arg(hint, terminal));
    }
    case 2:
        return unchanged(Kind::Refused, QStringLiteral("The helper refused the request%1; nothing was changed.").arg(detail(e.err)));
    case 5:
        // The helper's process guard could not be installed: it ends at mode entry, before reading the request or
        // building any Platform (INV-112), so nothing was changed.
        return unchanged(Kind::Refused,
                         QStringLiteral("The helper could not start safely%1; nothing was changed.").arg(detail(e.err)));
    case 0:
    case 1: break;
    default: return unknown(QStringLiteral("exit %1%2").arg(e.code).arg(detail(e.err)));
    }

    bool ok = false;
    QString why;
    if (req.op == Op::Inspect) {
        const std::optional<InspectReport> rep = parseInspectResponse(e.out, &ok, &why);
        if (!rep) return unknown(QStringLiteral("its response could not be read: %1").arg(why));
        if (ok != (e.code == 0)) return unknown(QStringLiteral("its response does not match its exit code %1").arg(e.code));
        o.kind = Kind::Done;
        o.inspect = rep;
        o.result.outcome = asb::Outcome::Ok;
        // A line to show like every other ending.
        o.result.message = o.message =
            QStringLiteral("Service: %1; settings folder: %2%3.")
                .arg(rep->installation == QLatin1String("Current") ? rep->service : rep->installation, rep->trust,
                     rep->reason.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(rep->reason));
        return o;
    }
    const std::optional<Result> r = parseResultResponse(e.out, &ok, &why);
    if (!r) return unknown(QStringLiteral("its response could not be read: %1").arg(why));
    if (ok != (e.code == 0) || ok != succeeded(*r))
        return unknown(QStringLiteral("its response does not match its exit code %1").arg(e.code));
    o.kind = Kind::Done;
    o.result = *r;
    o.message = r->message;
    // INV-107: only what is durably on disk, and only the exact revision this client submitted.
    o.clearsDirty = req.save.has_value() && r->persisted != Persisted::None && !submittedSha256.isEmpty() &&
                    r->persistedSha256 == submittedSha256;
    return o;
}

}  // namespace asb::cli
