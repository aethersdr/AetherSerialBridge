#include "tui/TuiApp.h"

#include "cli/ConfigEditor.h"
#include "core/BuildInfo.h"
#include "core/BuildStamp.h"
#include "core/ConfigJson.h"
#include "core/PhaseClock.h"
#include "core/StatusPrinter.h"
#include "core/TrustedConfig.h"
#include "tui/TuiHelp.h"
#include "tui/TuiText.h"
#include "tui/TuiView.h"

#include <QDir>
#include <QScopeGuard>

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace asb::tui {

namespace {

using ftxui::Event;

constexpr std::chrono::seconds kQueryBudget{2};
constexpr qint64 kLogMaxBytes = 4 * 1024 * 1024;   // bridge.log is at most 2 MB and two lines (master §4.7)
constexpr int kLogLines = 400;

class CapturingConsole final : public cli::Console {
public:
    void out(const QString& line) override { lines << line; }
    void err(const QString& line) override { lines << line; }
    QStringList lines;
};

QString describeInstallation(const Installation& i)
{
    switch (i.state) {
    case InstallState::NotInstalled: return QStringLiteral("not installed");
    case InstallState::IncompleteInstall:
        if (i.otherBuild) return QStringLiteral("a different build is installed");
        return QStringLiteral("the install did not finish%1; Save & apply finishes it")
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

QString titleFor(Outcome o)
{
    switch (o) {
    case Outcome::Ok: return QStringLiteral("Done");
    case Outcome::SavedNotApplied: return QStringLiteral("Saved, not applied");
    case Outcome::NothingChanged: return QStringLiteral("Nothing was changed");
    case Outcome::Uncommitted: return QStringLiteral("Not applied");
    case Outcome::CommittedNotRunning: return QStringLiteral("Saved, but the bridge is not running");
    case Outcome::StateUnknown: return QStringLiteral("The bridge's state is not known");
    }
    return {};
}

Tone toneFor(Outcome o)
{
    switch (o) {
    case Outcome::Ok: return Tone::Good;
    case Outcome::SavedNotApplied:
    case Outcome::NothingChanged: return Tone::Warn;
    default: break;
    }
    return Tone::Bad;
}

// Every interface, by the parsed value (0:0:0:0:0:0:0:0 and 0.00.0.0 included), never by the text.
bool isAllInterfaces(const QString& bind)
{
    return listensOnEveryInterface(bind.trimmed());
}

QString sha(const QByteArray& b)
{
    return sha256Hex(b);
}

std::string chr(const Event& e)
{
    return e.is_character() ? e.character() : std::string();
}

}  // namespace

// ---------------------------------------------------------------------------------------------------
TuiApp::TuiApp(cli::Host& host, Platform* platform, const QString& platformError, const Env& env)
    : m_host(host), m_platform(platform), m_platformError(platformError), m_env(env)
{
    m_s.elevated = env.elevated && !env.readOnlyExecutable;
    m_s.windows = env.windows;
    m_s.utf8 = env.utf8;
    m_s.color = env.color;
    m_s.dataFolder = host.dataFolder();
    m_s.program = env.program;
    m_s.readOnlyExecutable = env.readOnlyExecutable;
    m_s.appImage = env.appImage;
    m_s.headlessProgram = env.headlessProgram;
    if (env.readOnlyExecutable) m_s.readOnlyReason = readOnlyGuiExecutableBanner(m_s);
    else if (!env.elevated) m_s.readOnlyReason = QString::fromLatin1(text::kReadOnly);
    if (m_platform && m_s.elevated) m_ops = std::make_unique<OperationHost>(*m_platform, host.clock(), host.interimPoll());

    reload();
    refreshStatus();
    maybeOfferUpdate();

    if (env.statusPollMs > 0) {
        m_statusTimer.setInterval(env.statusPollMs);
        QObject::connect(&m_statusTimer, &QTimer::timeout, [this] { refreshStatus(); });
        m_statusTimer.start();
    }
    if (env.servicePollMs > 0) {
        m_serviceTimer.setInterval(env.servicePollMs);
        QObject::connect(&m_serviceTimer, &QTimer::timeout, [this] { refreshService(); });
        m_serviceTimer.start();
    }
    m_stepTimer.setInterval(100);
    QObject::connect(&m_stepTimer, &QTimer::timeout, [this] {
        if (!m_ops) return;
        const PhaseSnapshot snap = m_ops->phases().load();
        const QString step = snap.phase == Phase::None ? QString() : phaseName(snap.phase);
        if (step != m_s.busyStep) {
            m_s.busyStep = step;
            notify();
        }
    });
}

TuiApp::~TuiApp() = default;

void TuiApp::setColorMode(ColorMode mode)
{
    m_s.color = mode;
    notify();
}

ftxui::Element TuiApp::render(int width, int height) const
{
    m_viewWidth = width;
    m_viewHeight = height;
    return tui::render(m_s, width, height);
}

void TuiApp::closeFolderIdle()
{
    if (m_platform && !busy()) m_platform->folder.closeIdle();
}

std::optional<QByteArray> TuiApp::pendingDigest()
{
    if (!m_platform) return std::nullopt;
    const auto closeFolder = qScopeGuard([this] { closeFolderIdle(); });
    const OpenedFolder o = TrustedConfig::open(m_platform->folder, TrustMode::ReadOnly);
    if (!o.usable) return std::nullopt;
    const ConfigFiles::Loaded pending = TrustedConfig::readPending(m_platform->folder, o.inspection);
    if (!pending.present) return std::nullopt;
    return sha(pending.bytes).toLatin1();
}

void TuiApp::refreshDiskEvidence()
{
    // Everything the TUI compares with the disk besides the working copy: the trust state, and whether a pending
    // file (different from config.json) is there. m_source follows a persisted save in finished(); the installation
    // and activation are loadInstallation()'s. The working copy and its dirty edits are not touched.
    if (!m_platform) return;
    const auto closeFolder = qScopeGuard([this] { closeFolderIdle(); });
    const OpenedFolder o = TrustedConfig::open(m_platform->folder, TrustMode::ReadOnly);
    if (o.missing) {
        m_s.pendingPresent = false;
        return;
    }
    if (!o.usable) {
        m_s.load = Load::Untrusted;
        m_s.pendingPresent = false;
        return;
    }
    const ConfigFiles::Loaded committed = TrustedConfig::readCommitted(m_platform->folder, o.inspection);
    const ConfigFiles::Loaded pending = TrustedConfig::readPending(m_platform->folder, o.inspection);
    m_s.pendingPresent = pending.present && pending.config && pending.bytes != committed.bytes;
}

bool TuiApp::busy() const
{
    return m_ops && m_ops->busy();
}

void TuiApp::notify()
{
    if (changed) changed();
}

bool TuiApp::clearsDirty(const Result& r, const QByteArray& submittedBytes)
{
    return r.persisted != Persisted::None && !r.persistedSha256.isEmpty() &&
           r.persistedSha256 == sha(submittedBytes).toLatin1();
}

// ---- loading ------------------------------------------------------------------------------------------------
void TuiApp::reload()
{
    m_s.pendingPresent = false;
    m_s.loadError.clear();
    m_s.work = BridgeConfig{};
    m_source = Source{};
    if (!m_platform) {
        m_s.load = Load::Error;
        m_s.loadError = m_platformError.isEmpty() ? QStringLiteral("The data folder cannot be determined.") : m_platformError;
    } else {
        const auto closeFolder = qScopeGuard([this] { closeFolderIdle(); });
        const OpenedFolder o = TrustedConfig::open(m_platform->folder, TrustMode::ReadOnly);
        if (o.missing) {
            m_s.load = Load::NotCreated;
        } else if (!o.usable) {
            // INV-33: nothing in an untrusted folder is read, so nothing from it is shown.
            m_s.load = Load::Untrusted;
        } else {
            const ConfigFiles::Loaded committed = TrustedConfig::readCommitted(m_platform->folder, o.inspection);
            const ConfigFiles::Loaded pending = TrustedConfig::readPending(m_platform->folder, o.inspection);
            if (!committed.config) {
                m_s.load = Load::Error;
                m_s.loadError = committed.error;
            } else {
                m_s.load = Load::Ok;
                m_s.work = *committed.config;
                if (committed.present) m_source = Source{Source::Committed, sha(committed.bytes).toLatin1()};
                // D48: a pending file equal to config.json is not "saved, not applied".
                if (pending.present && pending.bytes != committed.bytes) {
                    if (pending.config) {
                        m_s.work = *pending.config;
                        m_source = Source{Source::Pending, sha(pending.bytes).toLatin1()};
                        m_s.pendingPresent = true;
                    } else {
                        say(pending.error, Tone::Bad);
                    }
                }
            }
        }
    }
    m_tracker.reset();
    m_s.dirty = false;
    m_s.selected = std::clamp(m_s.selected, 0, int(m_s.work.ports.size()));
    loadInstallation();
    // Another build not asked about yet: the update question, once the ports screen is up (maybeOfferUpdate). The
    // service poll never asks: a question appearing under the user's keys could take a y meant for something else.
    if (m_otherBuild && m_offeredSha != m_installedSha256) m_offerPending = true;
    notify();
}

void TuiApp::loadInstallation()
{
    // Every read here can fail, and a failure is "not known", never "off": an unknown auto-start must not route
    // Save & apply to the untick (SaveAndRemove / Deactivate), which would remove or mask the service
    // (the same finding on the GUI).
    bool installKnown = false, installed = false;
    QString unknownWhy;
    if (!m_platform) {
        m_s.serviceLine = QStringLiteral("unknown");
        m_s.serviceTone = Tone::Warn;
        unknownWhy = m_platformError.isEmpty() ? QStringLiteral("no platform") : m_platformError;
    } else {
        const Deadline deadline = m_host.clock().now() + kQueryBudget;
        try {
            const Installation inst = m_platform->service.detect(deadline);
            m_installState = inst.state;
            m_otherBuild = inst.state == InstallState::IncompleteInstall && inst.otherBuild;
            m_installedSha256 = inst.installedSha256;
            m_installedVersion = inst.installedVersion;
            installKnown = true;
            installed = inst.state == InstallState::Current || inst.state == InstallState::IncompleteInstall;
            QString line = describeInstallation(inst);
            Tone tone = inst.state == InstallState::NotInstalled ? Tone::Plain : Tone::Warn;
            if (installed) {
                // The query is shown only; a failure leaves the installation, and so auto-start, as detected.
                try {
                    const ServiceQuery q = m_platform->service.query(deadline);
                    line = inst.state == InstallState::Current ? describeQuery(q) : line + QStringLiteral("; ") + describeQuery(q);
                    if (inst.state == InstallState::Current)
                        tone = q.state == ServiceQuery::Running ? Tone::Good : Tone::Plain;
                } catch (const std::exception& e) {
                    line = QStringLiteral("installed; state unknown (%1)").arg(QString::fromLocal8Bit(e.what()));
                    tone = Tone::Warn;
                }
            }
            m_s.serviceLine = line;
            m_s.serviceTone = tone;
        } catch (const std::exception& e) {
            m_s.serviceLine = QStringLiteral("unknown (%1)").arg(QString::fromLocal8Bit(e.what()));
            m_s.serviceTone = Tone::Warn;
            unknownWhy = QStringLiteral("the service could not be queried: %1").arg(QString::fromLocal8Bit(e.what()));
        }
    }
    // "Start automatically" is the activation: installed with a start token. A failed apply, an untick and Stop
    // all leave the service without one (master §4.6). Only a missing token means "off"; a token that cannot be
    // read or parsed is not known.
    bool activationKnown = installKnown;
    bool activated = false;
    if (installKnown && installed) {
        // The token is read through the folder's handle: inspect it again first, as reload() did (an operation closes
        // the handle when it ends, v1.1 INV-128, so the last reload's handle may be gone by the time this runs).
        QString why;
        const auto closeFolder = qScopeGuard([this] { closeFolderIdle(); });
        m_platform->folder.inspect(TrustMode::ReadOnly);
        if (m_platform->token.read(&why)) {
            activated = true;
        } else if (why != QLatin1String("start.token is missing")) {
            activationKnown = false;
            unknownWhy = QStringLiteral("the activation (start.token) could not be read: %1").arg(why);
        }
    }
    m_s.installKnown = installKnown;
    m_s.installed = installed;
    m_s.otherBuild = installKnown && m_otherBuild;
    m_s.disabledNote = installed && activationKnown && !activated && m_s.load != Load::Untrusted;
    if (!m_s.dirty) {
        m_s.autoStart = activated;
        m_s.autoStartKnown = activationKnown;
        m_s.autoStartWhy = activationKnown ? QString() : unknownWhy;
    }
}

void TuiApp::refreshStatus()
{
    // Not while an operation runs: its ACK reads the status through the same reader (and the same folder object).
    if (!m_platform || busy()) return;
    std::optional<StatusReport> r = m_platform->status.read();
    if (r != m_s.status) {
        m_s.status = std::move(r);
        notify();
    }
}

void TuiApp::refreshService()
{
    // Never while an operation runs: the worker owns the service manager connection until its final result.
    if (busy() || !m_platform) return;
    const QString before = m_s.serviceLine;
    const bool autoStart = m_s.autoStart, known = m_s.autoStartKnown, note = m_s.disabledNote;
    loadInstallation();
    if (before != m_s.serviceLine || autoStart != m_s.autoStart || known != m_s.autoStartKnown || note != m_s.disabledNote)
        notify();
}

// ---- small helpers ------------------------------------------------------------------------------------------------
void TuiApp::say(const QString& message, Tone tone)
{
    m_s.message = message;
    m_s.messageTone = tone;
}

bool TuiApp::refuseReadOnly()
{
    if (m_s.elevated) return false;
    say(m_s.readOnlyReason, Tone::Warn);
    notify();
    return true;
}

bool TuiApp::refuseBusy()
{
    if (!busy()) return false;
    say(QString::fromLatin1(text::kBusyAction), Tone::Warn);
    notify();
    return true;
}

bool TuiApp::refuseUnloaded()
{
    if (!m_platform || !m_ops) {
        say(m_s.loadError.isEmpty() ? m_platformError : m_s.loadError, Tone::Bad);
    } else if (m_s.load == Load::Untrusted) {
        say(QString::fromLatin1(text::kUntrustedEdit), Tone::Bad);
    } else if (m_s.load == Load::Error) {
        say(m_s.loadError, Tone::Bad);
    } else {
        return false;
    }
    notify();
    return true;
}

void TuiApp::markEdited()
{
    m_tracker.markEdited();
    m_s.dirty = true;
    m_s.message.clear();
}

void TuiApp::ask(const QString& question, std::function<void(bool)> then, const QString& decision, bool noActs)
{
    m_s.questionNoActs = noActs;
    if (m_s.screen != Screen::Confirm) m_answerReturn = m_s.screen;
    m_s.question = question;
    // The decision in one short sentence: what the screen shows when the whole question does not fit, so Yes never
    // authorises something the user cannot see.
    m_s.questionShort = decision;
    m_onAnswer = std::move(then);
    m_s.screen = Screen::Confirm;
    notify();
}

void TuiApp::showResult(const QString& title, const QStringList& lines, Tone tone)
{
    m_s.resultTitle = title;
    m_s.resultLines = lines;
    m_s.resultTone = tone;
    m_s.screen = Screen::Result;
    notify();
}

void TuiApp::showPage(const QString& title, const QStringList& lines, const QList<int>& headings)
{
    m_s.pageTitle = title;
    m_s.pageLines = lines;
    m_s.pageHeadings = headings;
    m_s.pageTop = 0;
    m_s.screen = Screen::Page;
    notify();
}

void TuiApp::startBusy(Op op)
{
    m_op = op;
    m_s.busy = true;
    m_s.busyStep.clear();
    m_s.interim.clear();
    m_s.message.clear();
    m_stepTimer.start();
    notify();
}

// ---- operation callbacks ---------------------------------------------------------------------------------------
void TuiApp::interim(const Interim& i)
{
    // A status line only (D57): the operation is still outstanding, nothing is final.
    m_s.interim = i.message.isEmpty() ? QStringLiteral("Still processing (%1).").arg(i.phase) : i.message;
    notify();
}

void TuiApp::finished(const Result& r)
{
    m_stepTimer.stop();
    m_s.busy = false;
    m_s.busyStep.clear();
    m_s.interim.clear();
    const Op op = m_op;
    m_op = Op::None;
    bool persistedThese = false;
    if (op == Op::Save && m_submitted) {
        persistedThese = clearsDirty(r, *m_submitted);
        if (persistedThese) {
            m_tracker.markSaved(m_submittedRev);
            // The submitted bytes are now what is on disk (Staged: config.pending.json; Committed: config.json). The
            // editor may keep edits made during the run, but its evidence of the disk follows the save, so the next
            // Save & apply does not ask to "replace" the user's own pending file (INV-102).
            m_source = Source{r.persisted == Persisted::Staged ? Source::Pending : Source::Committed, r.persistedSha256};
        }
    }
    m_submitted.reset();
    m_s.dirty = m_tracker.isDirty();

    QStringList lines{r.message};
    QStringList extra;
    for (const QString& line : m_host.takeReports())
        if (!r.message.contains(line) && !r.fileChanges.contains(line) && !extra.contains(line)) extra << line;
    if (!extra.isEmpty()) lines << QStringLiteral("Also changed: %1.").arg(extra.join(QStringLiteral("; ")));
    if (op == Op::Save && m_s.dirty)
        lines << (persistedThese ? QStringLiteral("Changes made while it ran are not saved yet.")
                                 : QStringLiteral("Your changes are still in the editor, not saved."));
    m_pendingResult = PendingResult{titleFor(r.outcome), lines, toneFor(r.outcome)};
    m_s.resultPending = m_pendingResult->title;
    loadInstallation();
    // An open form or question is the user's input: the result waits until they are back on the ports screen
    // (flushPendingResult), and so does the reload, which would replace the ports a form refers to.
    flushPendingResult();
    if (m_s.hangup || m_quitAfterFinished) m_quit = true;
    notify();
}

void TuiApp::flushPendingResult()
{
    if (!m_pendingResult || m_s.screen == Screen::Edit || m_s.screen == Screen::Confirm) return;
    const PendingResult r = *m_pendingResult;
    m_pendingResult.reset();
    m_s.resultPending.clear();
    // What is on disk now: reloaded when the editor is clean; when it keeps unsaved edits, the edits stay and only
    // the evidence of the disk is refreshed (refreshDiskEvidence, then the installation and activation).
    m_s.dirty = m_tracker.isDirty();
    if (!m_s.dirty) {
        reload();
    } else {
        refreshDiskEvidence();
        loadInstallation();
    }
    showResult(r.title, r.lines, r.tone);
}

// ---- quitting -----------------------------------------------------------------------------------------------------
void TuiApp::requestQuit()
{
    if (busy()) {
        // INV-118: never quit (or hide) while an operation is outstanding.
        say(QString::fromLatin1(text::kBusyQuit), Tone::Warn);
        notify();
        return;
    }
    // Unsaved means the editor's changes and a port form's unaccepted ones (also behind a question it opened).
    const bool formOpen = m_s.screen == Screen::Edit || (m_s.screen == Screen::Confirm && m_answerReturn == Screen::Edit);
    if (m_s.dirty || (formOpen && m_s.form.changed)) {
        ask(QString::fromLatin1(text::kQuitDirty), [this](bool yes) {
            if (yes) m_quit = true;
        });
        return;
    }
    m_quit = true;
    notify();
}

void TuiApp::terminate()
{
    if (!busy()) {
        m_quit = true;
    } else {
        m_quitAfterFinished = true;
        say(QStringLiteral("Quitting when the operation finishes."), Tone::Warn);
    }
    notify();
}

void TuiApp::hangup()
{
    m_s.hangup = true;   // the terminal is gone: nothing more is drawn
    terminate();
}

// ---- keys ----------------------------------------------------------------------------------------------------------
bool TuiApp::onEvent(const Event& e)
{
    if (e == Event::Custom) return false;
    if (e.is_mouse() || e.is_cursor_position()) return false;
    // Ctrl+Z would suspend the process with the terminal in raw mode; the TUI does not suspend.
    if (e == Event::CtrlZ) return true;
    switch (m_s.screen) {
    case Screen::Ports: onPorts(e); break;
    case Screen::Edit: onEdit(e); break;
    case Screen::Confirm: onConfirm(e); break;
    case Screen::Result: onResult(e); break;
    case Screen::Page: onPage(e); break;
    }
    // Back on the ports screen with a result that arrived while a form or a question was open: show it now.
    if (m_pendingResult && m_s.screen == Screen::Ports) {
        flushPendingResult();
        notify();
    }
    maybeOfferUpdate();
    return true;
}

bool TuiApp::onPorts(const Event& e)
{
    m_s.message.clear();
    const std::string c = chr(e);
    if (e == Event::CtrlC || c == "q") {
        requestQuit();
        return true;
    }
    if (e == Event::ArrowUp || c == "k") moveSelection(-1);
    else if (e == Event::ArrowDown || c == "j") moveSelection(1);
    else if (e == Event::Return) {
        if (m_s.selected >= int(m_s.work.ports.size())) toggle();
        else openForm(m_s.selected);
    } else if (c == "a") addPort();
    else if (c == "d") deletePort();
    else if (c == " ") toggle();
    else if (c == "s") saveAndApply();
    else if (c == "r") reloadAsked();
    else if (c == "t") start();
    else if (c == "x") stop();
    else if (c == "p") showPortList();
    else if (c == "l") showLog();
    else if (c == "R") repair();
    else if (c == "P") loadPending();
    else if (c == "D") discardPending();
    else if (c == "?") showHelp();
    else if (c == "u" && m_otherBuild) offerUpdate();
    notify();
    return true;
}

void TuiApp::moveSelection(int delta)
{
    // The view scrolls the table so the selected row is always drawn (it alone knows how many rows fit).
    m_s.selected = std::clamp(m_s.selected + delta, 0, int(m_s.work.ports.size()));
}

void TuiApp::toggle()
{
    if (refuseReadOnly()) return;
    if (m_s.load == Load::Untrusted || m_s.load == Load::Error) {
        refuseUnloaded();
        return;
    }
    const int n = int(m_s.work.ports.size());
    if (m_s.selected >= n) {
        // From "not known", Space is the user's explicit choice: on (off is one more Space, and asks on save).
        m_s.autoStart = m_s.autoStartKnown ? !m_s.autoStart : true;
        m_s.autoStartKnown = true;
        m_s.autoStartWhy.clear();
    }
    else m_s.work.ports[m_s.selected].enabled = !m_s.work.ports[m_s.selected].enabled;
    markEdited();
}

void TuiApp::addPort()
{
    if (refuseReadOnly()) return;
    if (m_s.load == Load::Untrusted || m_s.load == Load::Error) {
        refuseUnloaded();
        return;
    }
    openForm(-1);
}

void TuiApp::deletePort()
{
    if (refuseReadOnly()) return;
    const int n = int(m_s.work.ports.size());
    if (m_s.selected >= n) return;
    m_s.work.ports.removeAt(m_s.selected);
    m_s.selected = std::clamp(m_s.selected, 0, int(m_s.work.ports.size()));
    markEdited();
}

void TuiApp::openForm(int index)
{
    EditForm f;
    f.index = index;
    PortMapping p;
    if (index >= 0) {
        p = m_s.work.ports[index];
    } else {
        int tcp = 4000;
        for (const PortMapping& q : m_s.work.ports) tcp = std::max(tcp, q.tcpPort);
        p.tcpPort = tcp + 1;
        p.bindAddress = QStringLiteral("127.0.0.1");
    }
    f.text[int(Field::Name)] = p.name;
    f.text[int(Field::Device)] = p.comPort;
    f.text[int(Field::Baud)] = QString::number(p.baudRate);
    f.text[int(Field::Format)] = cli::ConfigEditor::formatText(p);
    f.text[int(Field::Listen)] = p.bindAddress;
    f.text[int(Field::TcpPort)] = p.tcpPort > 0 ? QString::number(p.tcpPort) : QString();
    f.text[int(Field::Allowed)] = p.allowedClients;
    f.enabled = p.enabled;
    f.dtr = p.dtrEnable;
    f.rts = p.rtsEnable;
    m_s.form = f;
    m_s.screen = Screen::Edit;
}

bool TuiApp::onEdit(const Event& e)
{
    EditForm& f = m_s.form;
    const Field field = Field(f.focus);
    const std::string c = chr(e);
    if (e == Event::CtrlC) {
        requestQuit();
    } else if (e == Event::Escape) {
        if (f.changed) {
            ask(QString::fromLatin1(text::kDiscardPort), [this](bool yes) {
                if (yes) m_s.screen = Screen::Ports;
            });
        } else {
            m_s.screen = Screen::Ports;
        }
    } else if (e == Event::Tab || e == Event::ArrowDown) {
        f.focus = (f.focus + 1) % kFieldCount;
    } else if (e == Event::TabReverse || e == Event::ArrowUp) {
        f.focus = (f.focus + kFieldCount - 1) % kFieldCount;
    } else if (e == Event::Return) {
        if (!m_s.elevated) m_s.screen = Screen::Ports;   // read only: the form was a view
        else acceptForm();
    } else if (e == Event::Backspace || !c.empty()) {
        if (!m_s.elevated) {
            f.error = m_s.readOnlyReason;
        } else if (isToggle(field)) {
            if (c == " " || c == "x") {
                bool& b = field == Field::Enabled ? f.enabled : field == Field::Dtr ? f.dtr : f.rts;
                b = !b;
                f.changed = true;
                f.error.clear();
            }
        } else {
            QString& t = f.text[f.focus];
            if (e == Event::Backspace) {
                if (!t.isEmpty()) t.chop(t.size() >= 2 && t.at(t.size() - 1).isLowSurrogate() ? 2 : 1);
            } else if (static_cast<unsigned char>(c[0]) >= 0x20 && c != "\x7f") {
                t += QString::fromStdString(c);
            }
            f.changed = true;
            f.error.clear();
        }
    }
    notify();
    return true;
}

void TuiApp::acceptForm()
{
    EditForm& f = m_s.form;
    PortMapping p = f.index >= 0 ? m_s.work.ports[f.index] : PortMapping{};
    p.name = f.text[int(Field::Name)].trimmed();
    p.comPort = f.text[int(Field::Device)].trimmed();
    p.bindAddress = f.text[int(Field::Listen)].trimmed();
    p.allowedClients = f.text[int(Field::Allowed)].trimmed();
    p.enabled = f.enabled;
    p.dtrEnable = f.dtr;
    p.rtsEnable = f.rts;
    QStringList errors;
    bool ok = false;
    const int baud = f.text[int(Field::Baud)].trimmed().toInt(&ok);
    if (!ok) errors << QStringLiteral("Baud must be a whole number.");
    else p.baudRate = baud;
    const int tcp = f.text[int(Field::TcpPort)].trimmed().toInt(&ok);
    if (!ok) errors << QStringLiteral("TCP port must be a whole number.");
    else p.tcpPort = tcp;
    try {
        const cli::ConfigEditor::Format fmt = cli::ConfigEditor::parseFormat(f.text[int(Field::Format)].trimmed());
        p.dataBits = fmt.dataBits;
        p.parity = fmt.parity;
        p.stopBits = fmt.stopBits;
    } catch (const cli::EditRefused& e) {
        errors << QString::fromStdString(e.what()) + QLatin1Char('.');
    }
    if (errors.isEmpty()) {
        // This port on its own (the whole configuration, duplicates included, is validated by Save & apply).
        BridgeConfig one;
        one.ports << p;
        errors = one.validate();
    }
    if (!errors.isEmpty()) {
        f.error = errors.join(QLatin1Char(' '));
        return;
    }
    if (f.index < 0 || f.index >= int(m_s.work.ports.size())) {
        m_s.work.ports << p;
        m_s.selected = int(m_s.work.ports.size()) - 1;
    } else {
        m_s.work.ports[f.index] = p;
    }
    markEdited();
    m_s.screen = Screen::Ports;
}

bool TuiApp::onConfirm(const Event& e)
{
    const std::string c = chr(e);
    if (e == Event::CtrlC) {
        requestQuit();
        return true;
    }
    // Esc always cancels: the question is closed and nothing it leads to happens (no answer is given to it). y, and n
    // where No also acts (the build question: keep the installed build and apply), authorise only a decision the user
    // can see: on a screen too small for it they do nothing.
    const bool visible = decisionVisible(m_s, m_viewWidth, m_viewHeight);
    bool yes = false, cancel = false;
    if (e == Event::Escape) {
        cancel = true;
    } else if (c == "y" || c == "Y") {
        if (!visible) return true;
        yes = true;
    } else if (c == "n" || c == "N") {
        if (m_s.questionNoActs && !visible) return true;
    } else {
        return true;
    }
    std::function<void(bool)> then = std::move(m_onAnswer);
    m_onAnswer = nullptr;
    m_s.screen = m_answerReturn;
    if (!yes && m_s.screen == Screen::Ports) say(QString::fromLatin1(text::kNothingChanged));
    if (then && !cancel) then(yes);
    notify();
    return true;
}

bool TuiApp::onResult(const Event& e)
{
    const std::string c = chr(e);
    if (e == Event::CtrlC) requestQuit();
    else if (e == Event::Return || e == Event::Escape || c == "q") m_s.screen = Screen::Ports;
    notify();
    return true;
}

bool TuiApp::onPage(const Event& e)
{
    const std::string c = chr(e);
    const int page = 10;
    if (e == Event::CtrlC) requestQuit();
    else if (e == Event::Escape || c == "q") m_s.screen = Screen::Ports;
    else if (e == Event::ArrowUp || c == "k") m_s.pageTop -= 1;
    else if (e == Event::ArrowDown || c == "j") m_s.pageTop += 1;
    else if (e == Event::PageUp) m_s.pageTop -= page;
    else if (e == Event::PageDown) m_s.pageTop += page;
    // Bounded by the page as drawn (wrapped to the last rendered width), so the last line can be reached and
    // scrolling back starts at once.
    m_s.pageTop = std::clamp(m_s.pageTop, 0, pageMaxTop(m_s, m_viewWidth, m_viewHeight));
    notify();
    return true;
}

// ---- operations -----------------------------------------------------------------------------------------------------
TuiApp::SaveIntent TuiApp::captureIntent(const QByteArray& bytes, quint64 rev) const
{
    SaveIntent in;
    in.bytes = bytes;
    in.rev = rev;
    in.action = m_s.autoStart ? SaveIntent::Action::Run
                              : (m_s.windows ? SaveIntent::Action::SaveAndRemove : SaveIntent::Action::Deactivate);
    in.install = in.action == SaveIntent::Action::Run && m_installState != InstallState::Current;
    // The untick asks unless the service is known not to be installed (an unknown installation may be removed).
    in.untickAsks = in.action != SaveIntent::Action::Run && !(m_s.installKnown && !m_s.installed);
    // The installed program is replaced only with consent when it is a complete installation of another build (the
    // station outage of 2026-10-16); an install that did not finish for another reason is finished as before.
    in.buildAsks = in.action == SaveIntent::Action::Run && m_otherBuild;
    // The build the question names: if another one is found while the questions are open, they start again.
    if (in.buildAsks) in.installedSha256 = m_installedSha256;
    return in;
}

bool TuiApp::refuseUnknownAutoStart()
{
    if (m_s.autoStartKnown) return false;
    // Unknown never means "off": Save & apply would otherwise untick (remove or mask the service).
    say(QStringLiteral("Auto-start is not known (%1). Nothing was changed. Press r to read it again, or choose it on the "
                       "auto-start row (Space).")
            .arg(m_s.autoStartWhy),
        Tone::Bad);
    notify();
    return true;
}

void TuiApp::saveAndApply(bool restarted, bool update, const QString& askedSha256)
{
    if (refuseReadOnly() || refuseBusy() || refuseUnloaded() || refuseUnknownAutoStart()) return;
    const QStringList errors = m_s.work.validate();
    if (!errors.isEmpty()) {
        showResult(QStringLiteral("Not saved"), QStringList{QStringLiteral("Fix these before saving:")} + errors, Tone::Bad);
        return;
    }
    // The decision is taken now, with the bytes and the revision, before any question: what Save & apply will do
    // (Run, SaveAndRemove, Deactivate) and which questions that needs. The questions are about this decision, and
    // submitSave() checks it against fresh state before anything is submitted.
    SaveIntent in = captureIntent(toJson(m_s.work), m_tracker.revision());
    in.restarted = restarted;
    if (update) {
        // The update question was about replacing this program: anything else is not what y agreed to.
        if (!in.buildAsks) {
            say(m_otherBuild ? QStringLiteral("Nothing was changed: auto-start is off. Turn it on (Space on the auto-start "
                                              "row), then press u to update.")
                             : QStringLiteral("Nothing was changed: a different build is no longer installed."),
                Tone::Warn);
            notify();
            return;
        }
        in.buildAnswered = true;
        // y replaces only the build the question named: another one found since then makes
        // submitSave's check restart the chain, which asks the build question about it.
        in.installedSha256 = askedSha256;
    }
    askExposure(in);
}

void TuiApp::askExposure(const SaveIntent& in)
{
    QStringList exposed;
    for (const PortMapping& p : m_s.work.ports) {
        // Linux records the allowed clients but does not enforce them (D7).
        if (p.enabled && isAllInterfaces(p.bindAddress) && (!m_s.windows || p.allowedClients.trimmed().isEmpty()))
            exposed << p.displayName().trimmed();
    }
    if (exposed.isEmpty()) {
        askUntick(in);
        return;
    }
    // The compact form names at most two ports and counts the rest: with every name in it, a dozen exposed ports
    // overflowed even the compact form, and y was never offered (v1.1 debt, WS-14). The whole question keeps them all.
    const QString few = exposed.size() <= 2 ? exposed.join(QStringLiteral(", "))
                                            : QStringLiteral("%1 ports (%2, %3, +%4 more)")
                                                  .arg(QString::number(exposed.size()), exposed.at(0), exposed.at(1),
                                                       QString::number(exposed.size() - 2));   // one pass: a name may hold %4
    const QString device = exposed.size() == 1 ? QStringLiteral("the device") : QStringLiteral("the devices");
    const QString q = m_s.windows
                          ? QStringLiteral("%1 will listen on every network interface with no client restriction. Anyone who "
                                           "can reach this PC could operate that device. Save anyway?")
                          : QStringLiteral("%1 will listen on every network interface, and Linux does not enforce allowed "
                                           "clients. Anyone who can reach this machine could operate that device. Save anyway?");
    const QString names = exposed.join(QStringLiteral(", "));
    ask(
        q.arg(names),
        [this, in](bool yes) {
            if (yes) askUntick(in);
        },
        // The compact form keeps the consequence, not only the action: who can reach the device, and on Linux that
        // the allowed clients are not enforced (said so in every UI).
        (m_s.windows ? QStringLiteral("%1: every network interface, no client restriction: anyone who can reach this PC "
                                      "can use %2. Save anyway?")
                     : QStringLiteral("%1: every network interface; Linux ignores allowed clients: anyone who can reach "
                                      "it can use %2. Save anyway?"))
            .arg(few, device));
}

void TuiApp::askUntick(const SaveIntent& in)
{
    if (!in.untickAsks) {
        askBuild(in);
        return;
    }
    const QString q = m_s.windows ? QStringLiteral("Turning off auto-start removes the service: these ports stop being bridged "
                                                   "until you turn it back on. Continue?")
                                  : QStringLiteral("Turning off auto-start stops the bridge and keeps it from starting: these "
                                                   "ports stop being bridged until you turn it back on. Continue?");
    ask(
        q,
        [this, in](bool yes) {
            if (yes) askBuild(in);
        },
        m_s.windows ? QStringLiteral("Turn off auto-start? The service is removed; its ports stay unbridged until you turn "
                                     "it back on.")
                    : QStringLiteral("Turn off auto-start? The bridge stops; its ports stay unbridged until you turn it "
                                     "back on."));
}

void TuiApp::askBuild(const SaveIntent& in)
{
    // The update question already asked this one (y: update; keepBuild stays false).
    if (!in.buildAsks || in.buildAnswered) {
        askSupersede(in);
        return;
    }
    m_offeredSha = m_installedSha256;   // asked here: a reload does not ask about this build again
    QString installed = m_installedSha256.isEmpty() ? QStringLiteral("program digest unknown")
                                                    : QStringLiteral("program sha256 %1").arg(m_installedSha256.left(12));
    // Its build stamp names the release when the program has one (v1.1 D61): wording only.
    const QString version = core::describeInstalledVersion(
        m_installedVersion, QString::fromLatin1(core::version().data(), qsizetype(core::version().size())));
    if (!version.isEmpty()) installed += QStringLiteral("; ") + version;
    const QString mine = m_host.thisBuild();
    // No (n) keeps the installed program and applies the settings to it; Esc cancels and changes nothing; only y
    // replaces it.
    ask(
        QStringLiteral("The installed bridge is a different build (%1). Update it to this build (%2)? The ports restart. "
                       "No keeps the installed program and applies the settings to it.")
            .arg(installed, mine),
        [this, in](bool update) {
            SaveIntent d = in;
            d.keepBuild = !update;
            m_s.message.clear();   // No continues the save: it is not "nothing was changed"
            askSupersede(d);
        },
        QStringLiteral("Update the installed bridge (a different build) to this build? The ports restart. No keeps it "
                       "and applies the settings to it."),
        /*noActs=*/true);
}

void TuiApp::askSupersede(const SaveIntent& in)
{
    // A pending file that appeared or changed since the working copy was loaded is only replaced with consent
    // (INV-102); read now, ReadOnly.
    bool need = false;
    {
        // Closed again before the question or the submission below (the worker opens its own).
        const auto closeFolder = qScopeGuard([this] { closeFolderIdle(); });
        const OpenedFolder o = TrustedConfig::open(m_platform->folder, TrustMode::ReadOnly);
        if (o.usable) {
            const ConfigFiles::Loaded committed = TrustedConfig::readCommitted(m_platform->folder, o.inspection);
            const ConfigFiles::Loaded pending = TrustedConfig::readPending(m_platform->folder, o.inspection);
            if (pending.present && pending.bytes != committed.bytes) {
                const QByteArray p6 = sha(pending.bytes).toLatin1();
                const bool fromThat = m_source.kind == Source::Pending && m_source.sha256 == p6;
                need = !fromThat && sha(in.bytes).toLatin1() != p6;
            }
        }
    }
    if (!need) {
        askChanged(in, false);
        return;
    }
    ask(
        QString::fromLatin1(text::kSupersede),
        [this, in](bool yes) {
            if (yes) askChanged(in, true);
        },
        QStringLiteral("Replace the saved, not applied settings that changed since they were loaded?"));
}

void TuiApp::askChanged(const SaveIntent& in, bool supersede)
{
    // After a restart the chain ends with the decision itself, so a changed decision is never submitted unasked.
    if (!in.restarted) {
        submitSave(in, supersede);
        return;
    }
    QString what;
    switch (in.action) {
    case SaveIntent::Action::Run: what = QStringLiteral("save these settings, apply them and start the bridge"); break;
    case SaveIntent::Action::SaveAndRemove: what = QStringLiteral("save these settings and remove the service"); break;
    case SaveIntent::Action::Deactivate: what = QStringLiteral("save these settings and keep the bridge stopped"); break;
    }
    // This question states the change itself: the restart notice is not repeated under it.
    m_s.message.clear();
    ask(QStringLiteral("The service changed while you were answering. Save & apply will now %1. Continue?").arg(what),
        [this, in, supersede](bool yes) {
            if (yes) {
                SaveIntent confirmed = in;
                confirmed.restarted = false;
                submitSave(confirmed, supersede);
            }
        },
        QStringLiteral("Save & apply will now %1. Continue?").arg(what));
}

void TuiApp::submitSave(const SaveIntent& in, bool supersede)
{
    if (refuseBusy() || refuseUnknownAutoStart()) return;
    // The decision the questions were about, against the state now: a poll may have read the service again while
    // a question was open. Anything that changes the route (or the questions it needs) starts the questions again.
    const SaveIntent now = captureIntent(in.bytes, in.rev);
    if (now.action != in.action || now.install != in.install || (now.untickAsks && !in.untickAsks) ||
        now.buildAsks != in.buildAsks || now.installedSha256 != in.installedSha256) {
        say(QStringLiteral("The service changed while you were answering; nothing was changed yet."), Tone::Warn);
        saveAndApply(/*restarted=*/true);
        return;
    }
    SaveRequest req;
    req.config = in.bytes;
    req.source = m_source;
    req.supersedePending = supersede;
    req.keepInstalledBuild = in.keepBuild;
    bool ok = false;
    switch (in.action) {
    case SaveIntent::Action::Run: ok = m_ops->submitRun(req, in.install && !in.keepBuild, /*repairFirst=*/false, *this); break;
    case SaveIntent::Action::SaveAndRemove: ok = m_ops->submitSaveAndRemove(req, *this); break;
    case SaveIntent::Action::Deactivate: ok = m_ops->submitDeactivate(req, *this); break;
    }
    if (!ok) {
        say(QString::fromLatin1(text::kBusyAction), Tone::Warn);
        return;
    }
    m_submitted = in.bytes;
    m_submittedRev = in.rev;
    startBusy(Op::Save);
}

void TuiApp::start()
{
    if (refuseReadOnly() || refuseBusy()) return;
    if (!m_ops) {
        refuseUnloaded();
        return;
    }
    // Start never replaces the program: a complete installation of another build is started as it is (as the window).
    if (m_ops->submitStart(*this, /*acceptOtherBuild=*/true)) startBusy(Op::Start);
}

void TuiApp::stop()
{
    if (refuseReadOnly() || refuseBusy()) return;
    if (!m_ops) {
        refuseUnloaded();
        return;
    }
    if (m_ops->submitStop(*this)) startBusy(Op::Stop);
}

void TuiApp::repair()
{
    if (refuseReadOnly() || refuseBusy()) return;
    if (!m_ops) {
        refuseUnloaded();
        return;
    }
    const QString q = QStringLiteral("Repair stops the bridge and blocks it from starting, then secures the data folder %1: "
                                     "an untrusted folder is moved aside with everything in it (nothing is deleted) and a "
                                     "fresh one is created. Continue?")
                          .arg(QDir::toNativeSeparators(m_s.dataFolder));
    ask(
        q,
        [this](bool yes) {
            if (yes && !refuseBusy() && m_ops->submitRepair(*this)) startBusy(Op::Repair);
        },
        QStringLiteral("Repair? The bridge stops; an untrusted data folder is moved aside (nothing is deleted)."));
}

void TuiApp::discardPending()
{
    if (refuseReadOnly() || refuseBusy()) return;
    if (!m_ops) {
        refuseUnloaded();
        return;
    }
    if (!m_s.pendingPresent) {
        say(QStringLiteral("There are no saved, not applied settings."));
        return;
    }
    // The question is about this pending file: its digest is recorded now and the core compares it with the file it
    // deletes (INV-127).
    const std::optional<QByteArray> asked = pendingDigest();
    ask(
        QString::fromLatin1(text::kDiscardPending),
        [this, asked](bool yes) {
            if (!yes || refuseBusy()) return;
            refreshDiskEvidence();
            if (m_s.load == Load::Untrusted) {
                say(QString::fromLatin1(text::kUntrusted) + QStringLiteral(" Nothing was changed."), Tone::Bad);
                return;
            }
            if (!asked) {
                say(QStringLiteral("The saved, not applied settings could not be read; nothing was changed."), Tone::Warn);
                return;
            }
            // The core compares the digest of the file it deletes with the one the question was about, under its own
            // lock (INV-127): a file replaced while the user answered is kept and the result says so.
            if (m_ops->submitDiscardPendingIf(QString::fromLatin1(*asked), *this)) startBusy(Op::DiscardPending);
        },
        QStringLiteral("Discard the saved, not applied settings? The bridge keeps running the applied ones."));
}

void TuiApp::reloadAsked()
{
    if (refuseBusy()) return;
    if (!m_s.dirty) {
        reload();
        return;
    }
    ask(QString::fromLatin1(text::kReloadDirty), [this](bool yes) {
        if (yes) reload();
    });
}

void TuiApp::loadPending()
{
    if (refuseBusy()) return;
    auto load = [this] {
        reload();
        if (!m_s.pendingPresent) say(QStringLiteral("There are no saved, not applied settings."));
    };
    if (!m_s.dirty) {
        load();
        return;
    }
    ask(QString::fromLatin1(text::kReloadDirty), [load](bool yes) {
        if (yes) load();
    });
}

// ---- updating another installed build ------------------------------------------------------------------------------
QString TuiApp::buildPhrase() const
{
    // The window's installedBuildPhrase: "Installed: version 1.0.0 (program sha256 0123456789ab). This program: ...".
    QString what = core::describeInstalledVersion(
        m_installedVersion, QString::fromLatin1(core::version().data(), qsizetype(core::version().size())));
    if (what.isEmpty()) what = QStringLiteral("a different build");
    const QString digest =
        m_installedSha256.isEmpty() ? QString() : QStringLiteral(" (program sha256 %1)").arg(m_installedSha256.left(12));
    QString mine = m_host.thisBuild();
    if (mine.isEmpty()) mine = QStringLiteral("this build");
    return QStringLiteral("Installed: %1%2. This program: %3.").arg(what, digest, mine);
}

void TuiApp::maybeOfferUpdate()
{
    if (!m_offerPending || m_s.screen != Screen::Ports || busy() || m_pendingResult) return;
    m_offerPending = false;
    if (!m_otherBuild || m_offeredSha == m_installedSha256) return;
    // Settings that cannot be read cannot be kept: no question that y could not carry out (u still explains).
    if (m_s.elevated && m_s.load != Load::Ok && m_s.load != Load::NotCreated) return;
    // Auto-start off (or not known): Save & apply would not run the update, so y could not carry it out.
    if (m_s.elevated && (!m_s.autoStart || !m_s.autoStartKnown)) return;
    offerUpdate();
}

void TuiApp::offerUpdate()
{
    m_offeredSha = m_installedSha256;
    const QString what = QStringLiteral("The installed bridge is a different build. %1").arg(buildPhrase());
    const QString effect = QStringLiteral("Updating replaces the installed program with this one and keeps the settings. "
                                          "The bridge restarts, so connected clients are dropped.");
    if (!m_s.elevated) {
        // Read-only: nothing to answer; what updating needs, and Enter (or Esc) goes back.
        const QString command = elevatedCommand(m_s);
        showResult(QStringLiteral("A different build is installed"),
                   {what, effect,
                    command.isEmpty() ? QString::fromLatin1(text::kUpdateWindowOnly)
                    : (m_s.windows ? QStringLiteral("Updating needs an administrator: open a terminal with Run as "
                                                    "administrator and run %1.")
                                   : QStringLiteral("Updating needs root: run %1."))
                          .arg(command)},
                   Tone::Warn);
        return;
    }
    if (refuseBusy()) return;
    if (m_s.load != Load::Ok && m_s.load != Load::NotCreated) {
        refuseUnloaded();
        return;
    }
    if (!m_s.autoStart || !m_s.autoStartKnown) {
        say(QStringLiteral("Auto-start is off, so the bridge cannot be updated yet: turn it on (Space on the auto-start "
                           "row), then press u."),
            Tone::Warn);
        return;
    }
    // The working copy is applied, not the running settings: say so whenever it differs from them: saved, not applied settings it was loaded from (config.pending.json), unsaved edits, or both.
    const bool pending = m_source.kind == Source::Pending;
    const QString unsaved = pending && m_s.dirty
                                ? QStringLiteral(" Your saved, not yet applied changes and your unsaved changes are applied with it.")
                            : pending   ? QStringLiteral(" Your saved, not yet applied changes are applied with it.")
                            : m_s.dirty ? QStringLiteral(" Your unsaved changes are saved and applied with it.")
                                        : QString();
    ask(QStringLiteral("%1 Update it to this build? %2%3").arg(what, effect, unsaved),
        [this, asked = m_installedSha256](bool yes) {
            if (yes) saveAndApply(/*restarted=*/false, /*update=*/true, asked);
        },
        // The compact form keeps every consequence, the unsaved changes included.
        QStringLiteral("Update the installed bridge (a different build) to this build? The settings are kept; the bridge "
                       "restarts and clients are dropped.%1")
            .arg(unsaved));
}

// ---- pages ----------------------------------------------------------------------------------------------------------
void TuiApp::showPortList()
{
    // Names only: listing never opens a device (INV-32).
    CapturingConsole c;
    const int rc = m_host.listPorts(c);
    if (rc != 0 && c.lines.isEmpty()) c.lines << QStringLiteral("The serial ports could not be listed.");
    showPage(QStringLiteral("Serial ports (listed, never opened)"), c.lines);
}

void TuiApp::showLog()
{
    QStringList lines;
    if (!m_s.windows) {
        lines << QStringLiteral("The bridge logs to the system journal. To read it:")
              << QStringLiteral("  journalctl -u aether-serial-bridge") << QString();
    }
    if (m_platform && m_s.windows && busy()) {
        lines << QStringLiteral("bridge.log is shown when the operation in progress has finished.") << QString();
    } else if (m_platform && m_s.windows) {
        // bridge.log in the data folder, read like every viewer reads it (ReadOnly, through the validated handle).
        const auto closeFolder = qScopeGuard([this] { closeFolderIdle(); });
        const OpenedFolder o = TrustedConfig::open(m_platform->folder, TrustMode::ReadOnly);
        if (o.usable) {
            QString err;
            const std::optional<QByteArray> b = m_platform->folder.readFile(o.inspection, QStringLiteral("bridge.log"), kLogMaxBytes, &err);
            if (b) {
                QStringList all = QString::fromUtf8(*b).split(QLatin1Char('\n'));
                while (!all.isEmpty() && all.last().trimmed().isEmpty()) all.removeLast();
                if (all.size() > kLogLines) all = all.mid(all.size() - kLogLines);
                for (QString& l : all) {
                    l.remove(QLatin1Char('\r'));
                    lines << l;
                }
            } else {
                lines << QStringLiteral("No log to show: %1.").arg(err == fileNotFoundError() ? QStringLiteral("bridge.log does not exist yet") : err);
            }
        } else {
            lines << QString::fromLatin1(text::kUntrusted);
        }
    }
    // The last thing each port reported (status.json), on both systems.
    lines << QStringLiteral("Latest status:");
    for (const QString& l : StatusPrinter::format(m_s.status ? &*m_s.status : nullptr).split(QLatin1Char('\n'), Qt::SkipEmptyParts))
        lines << l;
    showPage(QStringLiteral("Log"), lines);
}

void TuiApp::showHelp()
{
    const QString version = QString::fromLatin1(core::version().data(), qsizetype(core::version().size()));
    const HelpPage h = helpPage(m_s.windows, m_s.readOnlyReason, QDir::toNativeSeparators(m_s.dataFolder), version);
    showPage(QStringLiteral("Help"), h.lines, h.headings);
}

}  // namespace asb::tui
