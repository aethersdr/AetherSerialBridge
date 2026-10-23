// asb_stage_client (test builds; v1.1 WS-15): the Linux window's two routes to a root helper, without the window, for
// the systemd suite's bootstrap_* cases. A copy of PkexecClient's launch over QProcess (like tst_HelperRoundTrip's
// launcher), driven by the window's own decisions (cli::HelperClient, cli::routeFor). It prints one JSON line:
//   {"rc":int,"started":bool,"crashed":bool,"out":str,"err":str,"kind":str,"message":str,"op":str,"route":str,
//    "launched":bool,"clearsDirty":bool}
//
//   asb_stage_client window <install|update|apply|untick|start|stop|repair|discard|uninstall>
//                    --program FILE [--digest HEX] [--config FILE] [--remove-settings] [--pending-sha HEX]
//       What the window does for that action: the op and route from routeFor() with the installed program's stamp,
//       the staged program's bytes checked locally against --digest (default: the file's own) before any launch.
//       --config is parsed and written back as the window writes it (core::toJson).
//
//   asb_stage_client raw --program FILE --request FILE [--digest TEXT] [--plen TEXT] [--rlen TEXT] [--extra-arg A]
//                    [--send-bytes N] [--append-bytes N] [--hold-open-ms N]
//       The staged launch with the framing under the test's control (no local check): the script's own refusals.
//
// Common: [--gate] prints {"gate":<pid>} and waits for a line on stdin before starting (pkttyagent registers for this
// pid, pkexec's parent); [--prefix ARG]... replaces "/usr/bin/pkexec" (e.g. strace, as root, for the root side's trace).

#include "cli/HelperClient.h"
#include "cli/StagedHelper.h"
#include "core/ConfigJson.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>

#include <cstdio>
#include <iostream>
#include <map>
#include <string>

#include <unistd.h>

using namespace asb;
using namespace asb::cli;

namespace {

struct Options {
    QStringList prefix{QStringLiteral("/usr/bin/pkexec")};
    bool gate = false;
    // raw framing
    QString plen, rlen;   // verbatim in argv when given ("12a", "0123")
    qint64 sendBytes = -1, appendBytes = 0;
    int holdOpenMs = -1;
    QStringList extraArgs;
};

class ToolProcess final : public IHelperProcess {
public:
    explicit ToolProcess(const Options& o) : m_o(o) {}
    void run(const QByteArray& request, std::function<void(const Exit&)> done) override
    {
        launch({QString::fromLatin1(INSTALLED), QStringLiteral("--helper")}, request, std::move(done));
    }
    void runStaged(const QByteArray& expectedSha256, const QByteArray& program, const QByteArray& request,
                   std::function<void(const Exit&)> done) override
    {
        QStringList args = staged::pkexecArguments(expectedSha256, program.size(), request.size());
        if (!m_o.plen.isNull()) args[5] = m_o.plen;   // /bin/sh -c <script> asb-stage <sha> <plen> <rlen>
        if (!m_o.rlen.isNull()) args[6] = m_o.rlen;
        args << m_o.extraArgs;
        QByteArray input = program + request;
        if (m_o.sendBytes >= 0) input.truncate(m_o.sendBytes);
        if (m_o.appendBytes > 0) input += QByteArray(m_o.appendBytes, 'x');
        launch(args, input, std::move(done));
    }
    bool launched = false;
    std::optional<Exit> last;   // the process's own ending, whatever the client made of it
    static constexpr char INSTALLED[] = "/usr/local/bin/aether-serial-bridge";

private:
    void launch(const QStringList& arguments, QByteArray input, std::function<void(const Exit&)> done)
    {
        if (m_o.gate) {
            std::printf("{\"gate\":%lld}\n", static_cast<long long>(::getpid()));
            std::fflush(stdout);
            std::string line;
            std::getline(std::cin, line);
        }
        launched = true;
        auto* p = new QProcess;
        done = [this, inner = std::move(done)](const Exit& e) {
            last = e;
            inner(e);
        };
        QObject::connect(p, &QProcess::finished, p, [p, done](int code, QProcess::ExitStatus status) {
            Exit e;
            e.code = code;
            e.crashed = status == QProcess::CrashExit;
            e.out = p->readAllStandardOutput();
            e.err = p->readAllStandardError();
            p->deleteLater();
            done(e);
        });
        QObject::connect(p, &QProcess::errorOccurred, p, [p, done](QProcess::ProcessError err) {
            if (err != QProcess::FailedToStart) return;
            Exit e;
            e.started = false;
            e.err = p->errorString().toUtf8();
            QTimer::singleShot(0, [done, e] { done(e); });
        });
        const int hold = m_o.holdOpenMs;
        QObject::connect(p, &QProcess::started, p, [p, input = std::move(input), hold] {
            p->write(input);
            if (hold < 0) p->closeWriteChannel();
            else QTimer::singleShot(hold, p, [p] { p->closeWriteChannel(); });
        });
        QStringList all = m_o.prefix.mid(1);
        all << arguments;
        p->start(m_o.prefix.value(0), all);
    }
    Options m_o;
};

QString kindName(HelperClient::Outcome::Kind k)
{
    using K = HelperClient::Outcome::Kind;
    switch (k) {
    case K::Done: return QStringLiteral("Done");
    case K::Cancelled: return QStringLiteral("Cancelled");
    case K::NotAuthorized: return QStringLiteral("NotAuthorized");
    case K::NoAgent: return QStringLiteral("NoAgent");
    case K::Refused: return QStringLiteral("Refused");
    case K::Failed: return QStringLiteral("Failed");
    }
    return {};
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "asb_stage_client: cannot read %s\n", qPrintable(path));
        std::exit(2);
    }
    return f.readAll();
}

[[noreturn]] void usage(const char* why)
{
    std::fprintf(stderr, "asb_stage_client: %s\n", why);
    std::exit(2);
}

void print(const std::optional<IHelperProcess::Exit>& e, const HelperClient::Outcome& o, bool launched)
{
    QJsonObject j;
    j[QStringLiteral("rc")] = e ? e->code : -1;
    j[QStringLiteral("started")] = e ? e->started : false;
    j[QStringLiteral("crashed")] = e ? e->crashed : false;
    j[QStringLiteral("out")] = e ? QString::fromUtf8(e->out) : QString();
    j[QStringLiteral("err")] = e ? QString::fromUtf8(e->err) : QString();
    j[QStringLiteral("kind")] = kindName(o.kind);
    j[QStringLiteral("message")] = o.message;
    j[QStringLiteral("op")] = helper::opName(o.op);
    j[QStringLiteral("route")] = o.route == Route::Staged ? QStringLiteral("staged") : QStringLiteral("installed");
    j[QStringLiteral("launched")] = launched;
    j[QStringLiteral("clearsDirty")] = o.clearsDirty;
    std::printf("%s\n", QJsonDocument(j).toJson(QJsonDocument::Compact).constData());
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QStringList a = app.arguments().mid(1);
    if (a.size() < 2) usage("usage: window <action> ... | raw ...");
    const QString mode = a.takeFirst();
    QString action = mode == QLatin1String("window") ? a.takeFirst() : QString();
    Options o;
    QString program, requestFile, configFile, digest, pendingSha;
    bool removeSettings = false, prefixGiven = false;
    while (!a.isEmpty()) {
        const QString k = a.takeFirst();
        const auto value = [&] {
            if (a.isEmpty()) usage("an option is missing its value");
            return a.takeFirst();
        };
        if (k == QLatin1String("--program")) program = value();
        else if (k == QLatin1String("--request")) requestFile = value();
        else if (k == QLatin1String("--config")) configFile = value();
        else if (k == QLatin1String("--digest")) digest = value();
        else if (k == QLatin1String("--pending-sha")) pendingSha = value();
        else if (k == QLatin1String("--remove-settings")) removeSettings = true;
        else if (k == QLatin1String("--gate")) o.gate = true;
        else if (k == QLatin1String("--plen")) o.plen = value();
        else if (k == QLatin1String("--rlen")) o.rlen = value();
        else if (k == QLatin1String("--send-bytes")) o.sendBytes = value().toLongLong();
        else if (k == QLatin1String("--append-bytes")) o.appendBytes = value().toLongLong();
        else if (k == QLatin1String("--hold-open-ms")) o.holdOpenMs = value().toInt();
        else if (k == QLatin1String("--extra-arg")) o.extraArgs << value();
        else if (k == QLatin1String("--prefix")) {
            if (!prefixGiven) o.prefix.clear();
            prefixGiven = true;
            o.prefix << value();
        } else usage(qPrintable(QStringLiteral("unknown option %1").arg(k)));
    }

    ToolProcess process(o);
    std::optional<IHelperProcess::Exit> exit;
    std::optional<HelperClient::Outcome> outcome;
    // Read once; the window mode hands the client these bytes with their digest (as the window's worker does).
    const QByteArray programBytes = program.isEmpty() ? QByteArray() : readFile(program);
    const QByteArray programSha256 = sha256Hex(programBytes).toLatin1();
    const QByteArray expected = digest.isEmpty() ? programSha256 : digest.toLatin1();

    if (mode == QLatin1String("raw")) {
        if (program.isEmpty() || requestFile.isEmpty()) usage("raw needs --program and --request");
        const QByteArray request = readFile(requestFile);
        QString why;
        std::optional<helper::Request> req = helper::parseRequest(request, &why);
        helper::Request r = req.value_or(helper::Request{helper::Op::Inspect, std::nullopt});
        const QByteArray sha = r.save ? sha256Hex(r.save->config).toLatin1() : QByteArray();
        process.runStaged(expected, programBytes, request, [&](const IHelperProcess::Exit& e) {
            exit = e;
            outcome = HelperClient::interpret(r, sha, 0, e, Route::Staged,
                                              HelperClient::Context{staged::terminalCommand(program, r.op, r.removeSettings), true});
            app.quit();
        });
        app.exec();
        print(exit, *outcome, process.launched);
        return 0;
    }
    if (mode != QLatin1String("window")) usage("the mode is window or raw");

    // The window: the op and route its routing picks, from the installed program's stamp (INV-123).
    static const std::map<QString, WindowAction> actions{
        {QStringLiteral("install"), WindowAction::Install}, {QStringLiteral("update"), WindowAction::Update},
        {QStringLiteral("apply"), WindowAction::Apply},     {QStringLiteral("untick"), WindowAction::Untick},
        {QStringLiteral("start"), WindowAction::Start},     {QStringLiteral("stop"), WindowAction::Stop},
        {QStringLiteral("repair"), WindowAction::Repair},   {QStringLiteral("discard"), WindowAction::Discard},
        {QStringLiteral("uninstall"), WindowAction::Uninstall}};
    const auto it = actions.find(action);
    if (it == actions.end()) usage("unknown window action");
    const RouteFacts facts = readInstalledFacts(QString::fromLatin1(ToolProcess::INSTALLED));
    const RoutePlan plan = routeFor(it->second, facts);
    std::optional<QByteArray> config;
    if (!configFile.isEmpty()) {
        QString why;
        const std::optional<BridgeConfig> c = fromJson(readFile(configFile), &why);
        if (!c) usage(qPrintable(QStringLiteral("the config does not parse: %1").arg(why)));
        config = toJson(*c);   // the bytes the window's editor submits
    }
    HelperClient client(process);
    client.setNothingInstalled(!facts.installedVersion.has_value());
    const HelperClient::StagedProgram staged{programBytes, programSha256, expected, program};
    const auto done = [&](const HelperClient::Outcome& out) {
        outcome = out;
        app.quit();
    };
    const Source fresh{Source::New, {}};
    bool ok = false;
    switch (plan.op) {
    case helper::Op::Install: ok = client.install(config, fresh, true, 1, staged, done); break;
    case helper::Op::Uninstall: ok = client.uninstall(removeSettings, staged, done); break;
    case helper::Op::SaveAndApply:
        if (!config) usage("apply needs --config");
        ok = client.saveAndApply(*config, fresh, true, 1, done);
        break;
    case helper::Op::Deactivate: ok = client.deactivate(config, fresh, true, 1, done); break;
    case helper::Op::Start: ok = client.start(done); break;
    case helper::Op::Stop: ok = client.stop(done); break;
    case helper::Op::Repair: ok = client.repair(done); break;
    case helper::Op::DiscardPending: ok = client.discardPending(done); break;
    case helper::Op::DiscardPendingIf:
        if (pendingSha.isEmpty()) usage("discard needs --pending-sha");
        ok = client.discardPendingIf(pendingSha, done);
        break;
    case helper::Op::Inspect: ok = client.inspect(done); break;
    }
    if (!ok) usage("the client refused to submit");
    app.exec();
    print(process.last, *outcome, process.launched);
    return 0;
}
