#include "cli/Commands.h"

#include "cli/ConfigEditor.h"
#include "cli/Elevation.h"
#ifndef _WIN32
#include "cli/Helper.h"
#endif
#include "core/AppLocations.h"
#include "core/AtomicJson.h"
#include "core/BuildInfo.h"
#include "core/ConfigFiles.h"
#include "core/ConfigJson.h"
#include "core/OperationHost.h"
#include "core/StatusPrinter.h"
#include "core/StatusStore.h"
#include "core/TrustedConfig.h"
#include "core/UninstallPreview.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTimer>

#include <cstdio>
#include <functional>
#include <memory>

namespace asb::cli {
namespace {

using namespace std::chrono_literals;

constexpr std::chrono::seconds kQueryBudget{15};

class StdConsole final : public Console {
public:
    void out(const QString& line) override { write(stdout, line); }
    void err(const QString& line) override { write(stderr, line); }

private:
    static void write(std::FILE* f, const QString& line)
    {
        const QByteArray b = line.toLocal8Bit() + '\n';
        std::fwrite(b.constData(), 1, size_t(b.size()), f);
        std::fflush(f);
    }
};

int usage(Console& c, const QString& message)
{
    c.err(QStringLiteral("error: %1.").arg(message));
    c.err(QStringLiteral("Run --help to see the commands."));
    return 2;
}

int fail(Console& c, const QString& message, int code = 1)
{
    c.err(QStringLiteral("error: %1").arg(message));
    return code;
}

// The QCoreApplication the Applier's worker, QtDBus and the engine need; created only after mode entry. Lives
// until the process ends (a CLI process runs one command).
void ensureApplication()
{
    if (QCoreApplication::instance()) return;
    static int argc = 1;
    static char name[] = "aether-serial-bridge";
    static char* argv[] = {name, nullptr};
    new QCoreApplication(argc, argv);
}

QString native(const QString& p)
{
    return QDir::toNativeSeparators(p);
}

// The command a hint tells the user to type.
QString program(const Host& h)
{
    return h.windows() ? QStringLiteral("aether-serial-bridge.exe") : QStringLiteral("aether-serial-bridge");
}

QString elevatedHint(const Host& h)
{
    return h.windows() ? QStringLiteral("as administrator") : QStringLiteral("with sudo");
}

QString describeInstallation(const Installation& i)
{
    switch (i.state) {
    case InstallState::NotInstalled: return QStringLiteral("not installed");
    case InstallState::IncompleteInstall:
        return QStringLiteral("installed, but the install did not finish%1; run --install")
            .arg(i.detail.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(i.detail));
    case InstallState::Foreign:
        return QStringLiteral("a service this program did not install is in the way%1")
            .arg(i.detail.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(i.detail));
    case InstallState::Current: return QStringLiteral("installed");
    }
    return {};
}

QString describeQuery(const ServiceQuery& q)
{
    switch (q.state) {
    case ServiceQuery::Absent: return QStringLiteral("not installed");
    case ServiceQuery::Stopped: return QStringLiteral("stopped");
    case ServiceQuery::StartPending: return QStringLiteral("starting");
    case ServiceQuery::Running: return QStringLiteral("running (pid %1)").arg(q.pid);
    case ServiceQuery::StopPending: return QStringLiteral("stopping");
    case ServiceQuery::Other: return QStringLiteral("in an unexpected state");
    }
    return {};
}

// ---------------------------------------------------------------------------------------------------
// One Applier operation on an OperationHost, waited for on this thread's event loop.
// ---------------------------------------------------------------------------------------------------
class Waiter final : public IOperationObserver {
public:
    Waiter(Console& c, QEventLoop& loop) : m_c(c), m_loop(loop) {}
    void interim(const Interim& i) override
    {
        // One line on stderr; never a final result (D57).
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

using Submit = std::function<bool(OperationHost&, IOperationObserver&)>;

// ASB_PHASE_TIMES=1: where an operation's time went, one stderr line after it (a diagnostic for slow stations;
// the phases are the Applier's own, sampled every 20 ms from its PhaseClock).
class PhaseTimes {
public:
    explicit PhaseTimes(const OperationHost& host) : m_host(host)
    {
        m_on = qEnvironmentVariableIsSet("ASB_PHASE_TIMES");
        if (!m_on) return;
        m_clock.start();
        m_timer.setInterval(20);
        QObject::connect(&m_timer, &QTimer::timeout, [this] { sample(); });
        m_timer.start();
    }
    void sample()
    {
        const Phase ph = m_host.phases().load().phase;
        if (ph == m_phase) return;
        close();
        m_phase = ph;
        m_since = m_clock.elapsed();
    }
    QString line()
    {
        if (!m_on) return {};
        m_timer.stop();
        close();
        QStringList parts;
        for (const auto& [name, ms] : m_spans) parts << QStringLiteral("%1 %2 s").arg(name).arg(double(ms) / 1000.0, 0, 'f', 2);
        return QStringLiteral("Phases: %1 (total %2 s).").arg(parts.join(QStringLiteral(", "))).arg(double(m_clock.elapsed()) / 1000.0, 0, 'f', 2);
    }

private:
    void close()
    {
        if (m_phase == Phase::None) return;
        const qint64 ms = m_clock.elapsed() - m_since;
        for (auto& [name, total] : m_spans)
            if (name == phaseName(m_phase)) {
                total += ms;
                return;
            }
        m_spans.append({phaseName(m_phase), ms});
    }
    const OperationHost& m_host;
    bool m_on = false;
    QElapsedTimer m_clock;
    QTimer m_timer;
    Phase m_phase = Phase::None;
    qint64 m_since = 0;
    QList<std::pair<QString, qint64>> m_spans;
};

Result runOperation(Host& h, Platform& p, Console& c, const Submit& submit)
{
    QEventLoop loop;
    OperationHost host(p, h.clock(), h.interimPoll());
    Waiter w(c, loop);
    PhaseTimes times(host);
    if (!submit(host, w)) {
        Result r;
        r.message = QStringLiteral("The operation could not be started.");
        return r;
    }
    // `finished` is delivered on this thread by the event loop, after the worker returned and released the locks;
    // only then does the command end (INV-118).
    while (!w.final) loop.exec();
    if (const QString t = times.line(); !t.isEmpty()) c.err(t);
    return *w.final;
}

bool succeeded(const Result& r)
{
    return r.outcome == Outcome::Ok || r.outcome == Outcome::SavedNotApplied;
}

// Prints the result and returns its exit code. What the Result's fileChanges hold beyond the message (an uninstall's
// program, entry, lock record and settings files) and what the adapters reported beyond both (lock
// files, the service's own files) follow on one line, each line once, before anything the caller prints after.
int report(Host& h, Console& c, const Result& final, const QStringList& onSuccess = {})
{
    // The adapters' reports (lock files secured or replaced: LockReport; the unit, binary and policy written or
    // removed) are folded into fileChanges, so whoever reads the Result (the helper's response, WS-9b) has them.
    const QStringList reports = h.takeReports();
    Result r = final;
    QStringList extra;
    for (const QString& line : reports)
        if (!r.message.contains(line) && !r.fileChanges.contains(line) && !extra.contains(line)) extra << line;
    QStringList shown;
    for (const QString& line : r.fileChanges)
        if (!r.message.contains(line) && !shown.contains(line)) shown << line;
    r.fileChanges << extra;
    shown << extra;
    h.resultHook(r, reports);
    const bool good = succeeded(r);
    auto say = [&](const QString& line) { good ? c.out(line) : c.err(line); };
    say(r.message);
    if (!shown.isEmpty()) say(QStringLiteral("Also changed: %1.").arg(shown.join(QStringLiteral("; "))));
    if (!good && r.message.contains(QLatin1String("engine.lock"))) {
        const QString holder = h.engineLockHolder();
        if (!holder.isEmpty()) say(QStringLiteral("engine.lock is held by %1.").arg(holder));
    }
    if (good)
        for (const QString& line : onSuccess) c.out(line);
    return good ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------------
// The commands
// ---------------------------------------------------------------------------------------------------
class Runner {
public:
    Runner(const Parsed& p, const QStringList& args, Host& h, Console& c) : m_p(p), m_args(args), m_h(h), m_c(c) {}

    int run()
    {
        switch (m_p.command) {
        case Command::Help: m_c.out(usageText(false, m_h.windows())); return 0;
        case Command::Version:
            m_c.out(QStringLiteral("aether-serial-bridge %1").arg(QString::fromLatin1(core::version().data(), qsizetype(core::version().size()))));
            return 0;
        case Command::Service: return m_h.runService();
        case Command::ListPorts: return m_h.listPorts(m_c);
        case Command::Helper:
#ifdef _WIN32
            return usage(m_c, QStringLiteral("--helper is not available on Windows"));
#else
            return runHelper(m_h, m_c);   // master §5.7 (WS-9b): its own mode entry, root only, one bounded request
#endif
        case Command::None:
        case Command::Gui:
        case Command::Tui:
        case Command::SelfTestInit:
            return usage(m_c, QStringLiteral("%1 is not a command this executable runs here").arg(commandName(m_p.command)));
        default: break;
        }
        if (m_h.managedConfigFile().isEmpty() || m_h.dataFolder().isEmpty())
            return fail(m_c, QString::fromLatin1(kWindowsDataFolderError), 3);
        if (m_p.config) {
            // Every --config use (INV-26, INV-90): a scratch config is never the managed one, nor inside its folder.
            const QString why = ScratchRun::refuseManagedPath(*m_p.config, m_h.managedConfigFile(), m_h.dataFolder(),
                                                              [this](const QString& p) { return m_h.fileKey(p); });
            if (!why.isEmpty()) return usage(m_c, why);
            // The diagnostic run writes status.json beside its config: a config by that name would be overwritten.
            const QString clash = ScratchRun::refuseStatusFileName(*m_p.config, m_h.windows(),
                                                                   [this](const QString& p) { return m_h.fileKey(p); });
            if (!clash.isEmpty()) return usage(m_c, clash);
            return scratch();
        }
        if (m_p.command == Command::UninstallFromApps) return uninstallFromApps();
        if (isMutating(m_p.command, false)) return mutate();
        switch (m_p.command) {
        case Command::Status: return status();
        case Command::ShowConfig: return showConfig();
        default: break;
        }
        return usage(m_c, QStringLiteral("%1 is not available here").arg(commandName(m_p.command)));
    }

private:
    // ---- mutating commands: mode entry, elevation, preflight, the Applier ---------------------------
    int mutate()
    {
        // Mode entry (INV-112): the guard first, before the QCoreApplication, the Platform or any Applier object.
        m_h.enterGuardedMode();
        m_h.guardProbe();
        if (!m_h.elevated()) return fail(m_c, Elevation::refusal(m_args.join(QLatin1Char(' ')), m_h.windows()) + QLatin1Char('.'));
        if (m_p.command == Command::Repair && !m_p.yes) {
            const std::optional<bool> yes = m_h.confirm(QStringLiteral(
                "--repair stops the bridge and blocks it from starting, then secures the data folder %1: an untrusted "
                "folder is moved aside with everything in it (nothing is deleted) and a fresh one is created. "
                "Continue? [y/N] ")
                                                            .arg(native(m_h.dataFolder())));
            if (!yes) return usage(m_c, QStringLiteral("--repair asks before it changes anything; run it in a terminal, or add --yes"));
            if (!*yes) {
                m_c.out(QStringLiteral("Nothing was changed."));
                return 1;
            }
        }
        ensureApplication();
        QString error;
        int code = 1;
        Platform* p = m_h.platform(&error, &code);
        if (!p) return fail(m_c, error, code);
        if (const std::optional<int> refused = preflight(*p)) return *refused;

        const Command cmd = m_p.command;
        const bool force = m_p.forceRecreate;
        if (cmd == Command::AddPort || cmd == Command::RemovePort || cmd == Command::SetEnabled) return editManaged(*p);
        Submit submit;
        QStringList onSuccess;
        switch (cmd) {
        case Command::Install:
            submit = [force](OperationHost& o, IOperationObserver& w) { return o.submitRun(std::nullopt, true, force, w); };
            break;
        case Command::Apply:
            submit = [force](OperationHost& o, IOperationObserver& w) { return o.submitRun(std::nullopt, false, force, w); };
            break;
        case Command::Start: submit = [](OperationHost& o, IOperationObserver& w) { return o.submitStart(w); }; break;
        case Command::Stop: submit = [](OperationHost& o, IOperationObserver& w) { return o.submitStop(w); }; break;
        case Command::Uninstall: return uninstall(*p);
        case Command::Repair: submit = [](OperationHost& o, IOperationObserver& w) { return o.submitRepair(w); }; break;
        case Command::DiscardPending:
            submit = [](OperationHost& o, IOperationObserver& w) { return o.submitDiscardPending(w); };
            break;
        default: return usage(m_c, QStringLiteral("%1 is not available here").arg(commandName(cmd)));
        }
        return report(m_h, m_c, runOperation(m_h, *p, m_c, submit), onSuccess);
    }

    // The ReadOnly preflight (master §4.6 "Interactive flow"): a data folder that is not trusted is refused with
    // exit 3 before the Applier is asked, with the command that fixes it. The Applier still decides for itself
    // under its lock; this only gives the untrusted case its own exit code.
    std::optional<int> preflight(Platform& p)
    {
        const Command cmd = m_p.command;
        // Repair fixes it; Stop skips P4; Uninstall touches nothing in an untrusted folder and says so (INV-125).
        if (cmd == Command::Repair || cmd == Command::Stop || cmd == Command::Uninstall) return std::nullopt;
        if ((cmd == Command::Install || cmd == Command::Apply) && m_p.forceRecreate) return std::nullopt;
        const Inspection i = p.folder.inspect(TrustMode::ReadOnly);
        if (i.action != DirAction::Recreate && i.action != DirAction::Refuse) return std::nullopt;
        const QString fix = cmd == Command::Install || cmd == Command::Apply
                                ? QStringLiteral("%1 --force-recreate, or --repair").arg(commandName(cmd))
                                : QStringLiteral("--repair");
        Result refused;
        refused.message = QStringLiteral("The data folder %1 is not trusted%2. Nothing was changed. Run %3 (%4) to move it "
                                         "aside, keeping everything in it, and start a fresh one.")
                              .arg(native(m_h.dataFolder()), i.reason.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(i.reason),
                                   fix, elevatedHint(m_h));
        m_h.resultHook(refused, {});
        m_c.err(QStringLiteral("error: ") + refused.message);
        return 3;
    }

    // ---- uninstall (WS-16; INV-125, INV-126) -------------------------------------------------------
    // What goes down, from a ReadOnly look (no lock, no Applier): the installation, the committed ports, the last status
    // and the data folder's trust.
    core::UninstallPreview preview(Platform& p)
    {
        Installation inst;
        try {
            inst = p.service.detect(m_h.clock().now() + kQueryBudget);
        } catch (const std::exception&) {
            // unknown: no ports are named
        }
        const OpenedFolder o = TrustedConfig::open(p.folder, TrustMode::ReadOnly);
        BridgeConfig committed;
        if (o.usable) {
            const ConfigFiles::Loaded c = TrustedConfig::readCommitted(p.folder, o.inspection);
            if (c.config) committed = *c.config;
        }
        return core::uninstallPreview(committed, p.status.read(), inst, o, native(m_h.dataFolder()));
    }

    // The last line: where the settings are, unless the uninstall removed them (the Applier's message says which:
    // removed, removed with the folder kept for other files, none there, some kept, or kept).
    QString settingsLine(const Result& r) const
    {
        if (r.message.contains(QLatin1String("The settings were removed"))) return QStringLiteral("The settings were removed.");
        if (r.message.contains(QLatin1String("There were no settings to remove.")))
            return QStringLiteral("There were no settings to remove.");
        if (r.message.contains(QLatin1String("Some settings could not be removed")))
            return QStringLiteral("Some settings are kept in %1.").arg(native(m_h.dataFolder()));
        return QStringLiteral("The settings are kept in %1.").arg(native(m_h.dataFolder()));
    }

    // --uninstall [--remove-settings]: the preview first, then the operation, then where the settings are.
    int uninstall(Platform& p)
    {
        const core::UninstallPreview pv = preview(p);
        for (const QString& line : pv.lines) m_c.out(line);
        const bool removeSettings = m_p.removeSettings;
        const Result r = runOperation(m_h, p, m_c, [removeSettings](OperationHost& o, IOperationObserver& w) {
            return o.submitUninstall(UninstallOptions{removeSettings}, w);
        });
        const int code = report(m_h, m_c, r);
        if (code == 0) m_c.out(settingsLine(r));
        else m_c.err(settingsLine(r));
        return code;
    }

    // The question both halves of --uninstall-from-apps ask (Settings offers no tick box: the settings stay, decision 4).
    QString appsQuestion(const core::UninstallPreview& pv, bool beforeElevation) const
    {
        QString stops = QStringLiteral("This stops the bridge");
        if (!pv.portsGoingDown.isEmpty()) {
            stops += QStringLiteral(": ") + pv.portsGoingDown.join(QStringLiteral(", "));
            if (!pv.connectedClients.isEmpty())
                stops += QStringLiteral(" (connected now: %1)").arg(pv.connectedClients.join(QStringLiteral(", ")));
        }
        return QStringLiteral("Uninstall Aether Serial Bridge? %1. It removes the service, the program and its firewall "
                              "rules. Your settings stay in %2.%3")
            .arg(stops, pv.settingsFolder,
                 beforeElevation ? QStringLiteral(" Windows will ask for administrator permission.") : QString());
    }

    // The Apps & features entry's command (D63, D64, INV-126). Not elevated (how Settings usually starts it): no guard,
    // no Applier, no OperationHost, no lock; a ReadOnly Platform for the question, then this executable's own image is
    // started elevated with fixed arguments and its exit code is ours. Elevated: the ordinary mutating route.
    int uninstallFromApps()
    {
        if (!m_h.windows())
            return usage(m_c, QStringLiteral("--uninstall-from-apps is only for Windows' Apps & features; use --uninstall"));
        if (!m_h.elevated()) {
            ensureApplication();
            QString error;
            int code = 1;
            Platform* p = m_h.platform(&error, &code);   // ReadOnly use only: it takes no lock
            core::UninstallPreview pv;
            if (p) pv = preview(*p);
            else pv.settingsFolder = native(m_h.dataFolder());
            m_h.detachConsole();
            if (!m_h.messageBox(appsQuestion(pv, true), Host::Box::Question)) return 0;   // No: nothing changed
            const Host::Relaunch r = m_h.relaunchElevated({QStringLiteral("--uninstall-from-apps"), QStringLiteral("--confirmed")});
            if (r.cancelled) {
                m_h.messageBox(QStringLiteral("Cancelled; nothing was changed."), Host::Box::Information);
                return 1;
            }
            if (!r.started) {
                m_h.messageBox(QStringLiteral("The uninstall could not be started as administrator (%1). Nothing was changed.")
                                   .arg(r.error),
                               Host::Box::Error);
                return 1;
            }
            return r.exitCode;
        }
        // Elevated: mode entry first (INV-112), then the question unless the unelevated half already asked it.
        m_h.enterGuardedMode();
        m_h.guardProbe();
        m_h.detachConsole();
        ensureApplication();
        QString error;
        int code = 1;
        Platform* p = m_h.platform(&error, &code);
        if (!p) {
            m_h.messageBox(QStringLiteral("Aether Serial Bridge could not be uninstalled: %1").arg(error), Host::Box::Error);
            return code;
        }
        if (!m_p.confirmed && !m_h.messageBox(appsQuestion(preview(*p), false), Host::Box::Question)) return 0;
        const Result r = runOperation(m_h, *p, m_c, [](OperationHost& o, IOperationObserver& w) {
            return o.submitUninstall(UninstallOptions{/*removeSettings=*/false}, w);
        });
        const int exit = report(m_h, m_c, r);
        QStringList kept;
        for (const QString& line : r.fileChanges)
            if (line.startsWith(QLatin1String("kept "))) kept << line.mid(5);
        QString text = r.message;
        if (!kept.isEmpty()) text += QStringLiteral("\n\nKept: %1.").arg(kept.join(QStringLiteral("; ")));
        text += QStringLiteral("\n\n") + settingsLine(r);
        m_h.messageBox(text, exit == 0 ? Host::Box::Information : Host::Box::Error);
        return exit;
    }

    // ---- edits ------------------------------------------------------------------------------------
    struct EditState {
        QStringList said;
        std::optional<QString> refused;
    };

    // The change itself (shared by both stores). Throws EditRefused.
    void change(BridgeConfig& c, QStringList* said) const
    {
        switch (m_p.command) {
        case Command::AddPort: ConfigEditor::addPort(c, m_p.addPort, said); break;
        case Command::RemovePort: ConfigEditor::removePort(c, m_p.portName, said); break;
        case Command::SetEnabled: ConfigEditor::setEnabled(c, m_p.portName, m_p.enable, said); break;
        default: break;
        }
    }

    QStringList afterAdd() const
    {
        QStringList notes;
        if (m_p.command != Command::AddPort) return notes;
        if (!m_p.addPort.allow.isEmpty() && !m_h.windows())
            notes << QStringLiteral("Note: on Linux --allow is recorded but not enforced. Add a firewall rule for it.");
        // A stat, never an open (INV-32). Windows COM names are not files.
        if (!m_h.windows() && !QFileInfo::exists(m_p.addPort.device))
            notes << QStringLiteral("Note: %1 does not exist right now. Is the adapter plugged in?").arg(m_p.addPort.device);
        return notes;
    }

    int editManaged(Platform& p)
    {
        auto state = std::make_shared<EditState>();
        const Runner* self = this;
        const Result r = runOperation(m_h, p, m_c, [state, self](OperationHost& o, IOperationObserver& w) {
            return o.submitEdit(
                [state, self](BridgeConfig& c) {
                    try {
                        self->change(c, &state->said);
                    } catch (const EditRefused& e) {
                        state->refused = QString::fromStdString(e.what());
                        throw;
                    }
                },
                w);
        });
        if (state->refused && r.outcome == Outcome::NothingChanged) {
            // The editor's own words lead; what the Applier's prologue already changed (the folder secured: master
            // §4.6 P4, recorded even when a later step fails) and what the adapters reported still follow
            //.
            const QStringList reports = m_h.takeReports();
            m_h.resultHook(r, reports);
            fail(m_c, *state->refused + QLatin1Char('.'));
            QStringList changed = r.fileChanges;
            for (const QString& line : reports)
                if (!changed.contains(line)) changed << line;
            if (!changed.isEmpty()) m_c.err(QStringLiteral("Changed anyway: %1.").arg(changed.join(QStringLiteral("; "))));
            return 1;
        }
        if (succeeded(r))
            for (const QString& line : state->said) m_c.out(line);
        return report(m_h, m_c, r, afterAdd());
    }

    // ---- scratch configs (--config <file>) ------------------------------------------------------------
    struct OpenScratch {
        ScratchFolder sf;
        OpenedFolder opened;
        QString file, name;
        int failure = 0;
    };

    OpenScratch openScratch()
    {
        OpenScratch s;
        const QFileInfo info(*m_p.config);
        s.file = QDir::cleanPath(info.absoluteFilePath());
        s.name = info.fileName();
        const QString dir = info.absolutePath();
        if (s.name.isEmpty()) {
            s.failure = usage(m_c, QStringLiteral("--config needs a file path, not a folder"));
            return s;
        }
        s.sf = m_h.scratchFolder(dir);
        s.opened = TrustedConfig::open(*s.sf.folder, TrustMode::Scratch);
        if (s.opened.missing) {
            s.failure = fail(m_c, QStringLiteral("the folder %1 does not exist.").arg(native(dir)));
        } else if (!s.opened.usable) {
            s.failure = fail(m_c,
                             QStringLiteral("the folder %1 is not trusted for a scratch config (%2). Use a folder of its "
                                            "own that only you (or %3) can write.")
                                 .arg(native(dir), s.opened.error, m_h.windows() ? QStringLiteral("an administrator") : QStringLiteral("root")),
                             3);
        }
        return s;
    }

    int scratch()
    {
        if (m_p.command == Command::Run) {
            const QString why = ScratchRun::refuseInstalledImage(m_h.ownExecutable(), m_h.installedExecutable(),
                                                                 [this](const QString& p) { return m_h.fileKey(p); });
            if (!why.isEmpty()) return usage(m_c, why);
            // Mode entry for the diagnostic engine (S17, INV-112, as for --service): the process-creation guard before
            // the QCoreApplication or any engine object, then the stop signals blocked before any thread exists. No
            // elevation and no managed Platform: --run touches neither the service nor the managed config.
            m_h.enterGuardedMode();
            m_h.guardProbe();
            m_h.blockStopSignals();
            ensureApplication();
        }
        OpenScratch s = openScratch();
        if (s.failure) return s.failure;
        switch (m_p.command) {
        case Command::Status: return scratchStatus(s);
        case Command::ShowConfig: return scratchShow(s);
        case Command::Run: return scratchRun(s);
        default: return scratchEdit(s);
        }
    }

    ConfigFiles::Loaded load(OpenScratch& s) { return TrustedConfig::readScratch(*s.sf.folder, s.opened.inspection, s.name); }

    int scratchEdit(OpenScratch& s)
    {
        const ConfigFiles::Loaded loaded = load(s);
        if (!loaded.config) return fail(m_c, loaded.error + QLatin1Char('.'));
        BridgeConfig config = *loaded.config;
        QStringList said;
        try {
            change(config, &said);
        } catch (const EditRefused& e) {
            return fail(m_c, QString::fromStdString(e.what()) + QLatin1Char('.'));
        }
        const WriteResult w = atomicWrite(*s.sf.io, s.sf.folder->handle(), s.name, toJson(config), OwnerPolicy::Config);
        if (!w.replaced) return fail(m_c, QStringLiteral("could not save %1: %2.").arg(native(s.file), w.error));
        for (const QString& line : said) m_c.out(line);
        for (const QString& line : afterAdd()) m_c.out(line);
        m_c.out(QStringLiteral("Saved %1.%2").arg(native(s.file), w.durable ? QString() : QStringLiteral(" Warning: it may not survive a power loss (%1).").arg(w.error)));
        return 0;
    }

    int scratchShow(OpenScratch& s)
    {
        const ConfigFiles::Loaded loaded = load(s);
        if (!loaded.config) return fail(m_c, loaded.error + QLatin1Char('.'));
        m_c.out(QStringLiteral("Config: %1%2").arg(native(s.file), loaded.present ? QString() : QStringLiteral(" (not created yet)")));
        for (const QString& line : ConfigEditor::describe(*loaded.config, program(m_h))) m_c.out(line);
        return 0;
    }

    int scratchStatus(OpenScratch& s)
    {
        QString err;
        const std::optional<QByteArray> bytes = s.sf.folder->readFile(s.opened.inspection, QString::fromLatin1(StatusStore::kFileName),
                                                                       StatusStore::kStatusMaxBytes, &err);
        m_c.out(QStringLiteral("Diagnostic run of %1 (--run --config):").arg(native(s.file)));
        if (!bytes) {
            if (err == fileNotFoundError()) {
                m_c.out(QStringLiteral("No status reported yet."));
                return 0;
            }
            return fail(m_c, QStringLiteral("could not read the status: %1.").arg(err));
        }
        const std::optional<StatusReport> rep = StatusStore::fromJson(*bytes);
        if (!rep) return fail(m_c, QStringLiteral("the status file beside the config is not a status report."));
        for (const QString& line : StatusPrinter::format(&*rep).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) m_c.out(line);
        return 0;
    }

    int scratchRun(OpenScratch& s)
    {
        const ConfigFiles::Loaded loaded = load(s);
        if (!loaded.present) return fail(m_c, QStringLiteral("%1 does not exist.").arg(native(s.file)));
        if (!loaded.config) return fail(m_c, loaded.error + QLatin1Char('.'));
        const QStringList errors = loaded.config->validate();
        if (!errors.isEmpty())
            return fail(m_c, QStringLiteral("%1 is not valid:\n  %2").arg(native(s.file), errors.join(QStringLiteral("\n  "))));
        QStringList warnings;
        const BridgeConfig loopback = ScratchRun::forceLoopback(*loaded.config, &warnings);
        for (const QString& w : warnings) m_c.err(w);
        return m_h.runScratch(s.file, loopback, m_c);
    }

    // ---- read-only views of the managed config and service ------------------------------------------
    Platform* readOnlyPlatform(int* code)
    {
        ensureApplication();
        QString error;
        Platform* p = m_h.platform(&error, code);
        if (!p) fail(m_c, error, *code);
        return p;
    }

    int showConfig()
    {
        int code = 1;
        Platform* p = readOnlyPlatform(&code);
        if (!p) return code;
        const OpenedFolder o = TrustedConfig::open(p->folder, TrustMode::ReadOnly);
        if (!o.usable && !o.missing)
            return fail(m_c, QStringLiteral("the data folder %1 is not trusted (%2); nothing in it is shown. Run --repair (%3).")
                                 .arg(native(m_h.dataFolder()), o.error, elevatedHint(m_h)),
                        3);
        const ConfigFiles::Loaded committed = o.missing ? ConfigFiles::Loaded{false, {}, BridgeConfig{}, {}}
                                                        : TrustedConfig::readCommitted(p->folder, o.inspection);
        if (!committed.config) return fail(m_c, committed.error + QLatin1Char('.'));
        m_c.out(QStringLiteral("Config: %1%2").arg(native(m_h.managedConfigFile()),
                                                   committed.present ? QString() : QStringLiteral(" (not created yet)")));
        for (const QString& line : ConfigEditor::describe(*committed.config, program(m_h))) m_c.out(line);
        if (o.missing) return 0;
        const ConfigFiles::Loaded pending = TrustedConfig::readPending(p->folder, o.inspection);
        if (!pending.present) return 0;
        m_c.out(QStringLiteral("Saved, not applied (config.pending.json): run --apply to apply it, or --discard-pending "
                               "to drop it."));
        if (!pending.config) return fail(m_c, pending.error + QLatin1Char('.'));
        for (const QString& line : ConfigEditor::describe(*pending.config, program(m_h))) m_c.out(line);
        return 0;
    }

    int status()
    {
        int code = 1;
        Platform* p = readOnlyPlatform(&code);
        if (!p) return code;
        int result = 0;
        const Deadline deadline = m_h.clock().now() + kQueryBudget;
        std::optional<ServiceQuery> running;
        try {
            const Installation inst = p->service.detect(deadline);
            QString line = describeInstallation(inst);
            if (inst.state == InstallState::Current || inst.state == InstallState::IncompleteInstall) {
                running = p->service.query(deadline);
                line = inst.state == InstallState::Current ? describeQuery(*running)
                                                           : line + QStringLiteral("; ") + describeQuery(*running);
            }
            m_c.out(QStringLiteral("Service: %1").arg(line));
        } catch (const std::exception& e) {
            // The status still prints what it can, and the command succeeds.
            m_c.out(QStringLiteral("Service: unknown (%1)").arg(QString::fromLocal8Bit(e.what())));
        }
        const std::optional<StatusReport> rep = p->status.read();
        for (const QString& line : StatusPrinter::format(rep ? &*rep : nullptr).split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            m_c.out(line);

        // The ACK fields against what is on disk (read only, INV-44).
        const OpenedFolder o = TrustedConfig::open(p->folder, TrustMode::ReadOnly);
        if (!o.usable && !o.missing) {
            m_c.out(QStringLiteral("Data folder: %1 is not trusted (%2); run --repair (%3).")
                        .arg(native(m_h.dataFolder()), o.error, elevatedHint(m_h)));
            return 3;
        }
        if (o.missing) return result;
        const ConfigFiles::Loaded committed = TrustedConfig::readCommitted(p->folder, o.inspection);
        if (rep && committed.present && !committed.bytes.isEmpty()) {
            const bool same = rep->configSha256 == sha256Hex(committed.bytes);
            m_c.out(same ? QStringLiteral("Config: the bridge reported loading the saved config.json.")
                         : QStringLiteral("Config: the bridge reported loading a config other than the saved config.json; "
                                          "run --start (%1) to load it.")
                               .arg(elevatedHint(m_h)));
        }
        if (rep && running && running->state == ServiceQuery::Running && running->pid != 0 && rep->pid != running->pid)
            m_c.out(QStringLiteral("Note: this status was written by pid %1, not by the running service (pid %2).")
                        .arg(rep->pid)
                        .arg(running->pid));
        QString why;
        const std::optional<IStartToken::Token> token = p->token.read(&why);
        if (!token)
            m_c.out(QStringLiteral("Activation: none (%1): the bridge will not start until --start or --apply succeeds.").arg(why));
        else if (rep && rep->startToken != token->token)
            m_c.out(QStringLiteral("Activation: the status above is from an earlier start, not the current activation."));
        if (TrustedConfig::readPending(p->folder, o.inspection).present)
            m_c.out(QStringLiteral("Saved, not applied: config.pending.json; run --apply to apply it, or --discard-pending."));
        return result;
    }

    const Parsed& m_p;
    const QStringList& m_args;
    Host& m_h;
    Console& m_c;
};

}  // namespace

QString Host::thisBuild()
{
    return QString::fromLatin1(core::version().data(), qsizetype(core::version().size()));
}

Console& standardConsole()
{
    static StdConsole c;
    return c;
}

int runCommand(const QStringList& args, Host& h, Console& c)
{
    const Parsed p = parse(args);
    if (!p.ok()) return usage(c, p.usageError);
    if (p.command == Command::None) {
        c.out(usageText(false, h.windows()));
        return 2;
    }
    return runCommand(p, args, h, c);
}

int runCommand(const Parsed& p, const QStringList& args, Host& h, Console& c)
{
    return Runner(p, args, h, c).run();
}

QString usageText(bool guiExecutable, bool windows)
{
    const QString exe = guiExecutable ? (windows ? QStringLiteral("AetherSerialBridge.exe") : QStringLiteral("aether-serial-bridge-gui"))
                                      : (windows ? QStringLiteral("aether-serial-bridge.exe") : QStringLiteral("aether-serial-bridge"));
    const QString admin = windows ? QStringLiteral("from an elevated terminal (Run as administrator)") : QStringLiteral("with sudo");
    const QString config = windows ? QStringLiteral("%ProgramData%\\AetherSerialBridge\\config.json")
                                   : QString::fromLatin1(kLinuxConfigFile);
    QString t = QStringLiteral("Aether Serial Bridge %1: publishes serial ports on TCP for AetherSDR.\n\n")
                    .arg(QString::fromLatin1(core::version().data(), qsizetype(core::version().size())));
    t += QStringLiteral("  %1                 %2\n").arg(exe, guiExecutable ? QStringLiteral("open the configuration window")
                                                                         : QStringLiteral("the terminal interface (when this build has one), otherwise this help"));
    if (guiExecutable)
        t += QStringLiteral("\nThis executable shows --status and --show-config; the other commands run with %1.\n")
                 .arg(windows ? QStringLiteral("aether-serial-bridge.exe") : QStringLiteral("aether-serial-bridge"));
    t += QStringLiteral("\nService (run these %1):\n").arg(admin);
    t += QStringLiteral(
        "  --install [--force-recreate]   install the service, open its firewall rules (Windows), then start it\n"
        "  --apply [--force-recreate]     apply the saved settings: stop, update the firewall, start\n"
        "  --start                        update the firewall from config.json, then start the service\n"
        "                                 (use this after a failed apply, once the error is fixed)\n"
        "  --stop                         stop the service (it starts again with --start or --apply)\n"
        "  --uninstall [--remove-settings]  stop and remove the service, its firewall rules and the program; the\n"
        "                                 settings are kept unless --remove-settings (only the files it writes)\n"
        "  --repair [--yes]               stop the service and secure the data folder; an untrusted folder is moved\n"
        "                                 aside with everything in it (asks first unless --yes)\n"
        "  --discard-pending              drop the saved, not applied settings (config.pending.json)\n"
        "    --force-recreate             with --install/--apply: if the data folder is not trusted, move it aside\n"
        "                                 (nothing is deleted) and start a fresh one\n");
    t += QStringLiteral(
        "\nPorts (saved as pending settings; --apply applies them):\n"
        "  --list-ports                   the serial ports on this machine (never opens them)\n"
        "  --show-config                  the configured ports, and any saved, not applied settings\n"
        "  --add-port <name> <device> <tcp-port> [--baud 38400] [--format 8N1] [--bind <ip>] [--allow <list>]\n"
        "  --remove-port <name>\n"
        "  --set-enabled <name> on|off\n"
        "\nStatus and diagnostics:\n"
        "  --status                       service state and per-port status\n"
        "  --run --config <file>          run the bridge in this terminal on a scratch config (Ctrl+C to stop);\n"
        "                                 listens on this machine only (127.0.0.1 / ::1), never the installed config\n"
        "  --config <file>                with --run, --status, --show-config and the port commands: use this\n"
        "                                 scratch config (outside the data folder) instead of the installed one\n"
        "  --help, --version\n");
    t += QStringLiteral("\nConfig: %1\n").arg(config);
    t += windows ? QStringLiteral("Log:    %ProgramData%\\AetherSerialBridge\\bridge.log\n")
                 : QStringLiteral("Status: %1\nLog:    journalctl -u aether-serial-bridge\n").arg(QString::fromLatin1(kLinuxStatusFile));
    t += QStringLiteral("\nExit codes: 0 done, 1 failed, 2 usage error, 3 the data folder is not trusted.");
    return t;
}

}  // namespace asb::cli
