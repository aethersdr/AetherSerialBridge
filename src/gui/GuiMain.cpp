#include "gui/GuiMain.h"

#include "core/OperationHost.h"
#ifndef _WIN32
#include "gui/HelperActions.h"
#include "gui/PkexecClient.h"
#endif

#include "core/ThemeManager.h"
#include "gui/Actions.h"
#include "gui/BridgeStyle.h"
#include "gui/BridgeWindow.h"
#include "gui/CanonBody.h"
#include "gui/Dialogs.h"
#include "gui/PlatformHooks.h"
#include "gui/ThemeFollower.h"
#include "gui/ViewSource.h"
#include "gui/WindowSetup.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QJsonArray>
#include <QMap>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>

#ifdef _WIN32
#include "platform/win/NativeSeams.h"
#include "platform/win/WbemSession.h"
#include <thread>
#endif

namespace asb::gui {
namespace {

// Initialisation shared by the window and the self-test, in the master §5.1 order after the QApplication.
void initialiseUi(QApplication& app, win::PluginPolicy plugins)
{
    win::applyQtLibraryPolicy(plugins);
    applyApplicationIcon(app);
    applyFusionStyle(app);
    registerInterFonts();
    (void)AetherSDR::ThemeManager::instance();
}

#ifdef _WIN32
// The process's COM security (WS-7 "COM setup") is set once, on a helper thread that joins the multithreaded
// apartment, before the QApplication exists: Qt makes the GUI thread a single-threaded apartment (OleInitialize),
// which must not be changed under it. The operation worker joins the multithreaded apartment itself
// (WbemSession::connect). A failure is not fatal: the firewall steps then fail closed with the COM error.
void comSecurityOnHelperThread()
{
    std::thread t([] { win::initialiseComSecurity(nullptr); });
    t.join();
}
#endif

// Answers nothing (the self-test never asks).
class SilentAsker final : public IAsker {
public:
    bool confirm(const QString&, const QString&, const QString&, const QString&) override { return false; }
    void notify(const QString&, const QString&) override {}
};

class PaintWatch final : public QObject {
public:
    bool painted = false;

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (e->type() == QEvent::Paint) painted = true;
        return QObject::eventFilter(o, e);
    }
};

#ifdef ASB_TEST_SEAMS
// Test builds only (tst_GuiNative; the release audit asserts test seams are absent). ASB_TEST_GUI_OPS="stop,save,start"
// runs those window operations one after another once the window is up, each after the previous result was
// delivered, and prints "asb-gui-op <op> <Outcome>: <message>" per result and "asb-gui-ops done" on stdout. Questions
// are answered from ASB_TEST_GUI_ANSWERS ("updateBuild=0,exposure=1"; an id not listed is answered no) and printed
// ("asb-gui-ask <id>"). ASB_TEST_GUI_NO_FIREWALL_CHECK=1 leaves the window's stale-rule check out.
void testLine(const QString& line)
{
    const QByteArray b = line.toUtf8() + '\n';
    std::fwrite(b.constData(), 1, size_t(b.size()), stdout);
    std::fflush(stdout);
}

// ASB_TEST_GUI_ANSWERS: "id=1" (a yes/no question, or the affirmative choice of a lifecycle question: its last
// choice), "id:choice" (a lifecycle question's choice by id, e.g. build:keep, build:cancel, uninstall:uninstall). An id
// not listed is answered no / Cancelled. The checkbox of the next question is ticked by the op uninstall-settings.
class EnvAsker final : public IAsker {
public:
    EnvAsker()
    {
        for (const QString& kv : qEnvironmentVariable("ASB_TEST_GUI_ANSWERS").split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const qsizetype colon = kv.indexOf(QLatin1Char(':'));
            const qsizetype eq = kv.indexOf(QLatin1Char('='));
            if (colon > 0 && (eq < 0 || colon < eq)) m_choices.insert(kv.left(colon).trimmed(), kv.mid(colon + 1).trimmed());
            else if (eq > 0) m_answers.insert(kv.left(eq).trimmed(), kv.mid(eq + 1).trimmed() == QLatin1String("1"));
        }
    }
    bool tickNext = false;   // the next question's checkbox (uninstall-settings)
    bool confirm(const QString& id, const QString&, const QString&, const QString&) override
    {
        testLine(QStringLiteral("asb-gui-ask %1").arg(id));
        return m_answers.value(id, false);
    }
    Answer ask(const Question& q) override
    {
        Answer a;
        if (m_choices.contains(q.id)) {
            a.choice = m_choices.value(q.id);
            if (a.choice == QLatin1String("cancel")) a.choice.clear();
        } else if (m_answers.value(q.id, false) && !q.choices.isEmpty()) {
            a.choice = q.choices.last().id;
        }
        a.checked = !q.checkbox.isEmpty() && tickNext;
        tickNext = false;
        testLine(QStringLiteral("asb-gui-ask %1 -> %2%3")
                     .arg(q.id, a.choice.isEmpty() ? QStringLiteral("cancelled") : a.choice,
                          a.checked ? QStringLiteral(" (box ticked)") : QString()));
        return a;
    }
    void notify(const QString& id, const QString& text) override
    {
        testLine(QStringLiteral("asb-gui-notice %1: %2").arg(id, QString(text).replace(QLatin1Char('\n'), QLatin1Char(' '))));
    }

private:
    QMap<QString, bool> m_answers;
    QMap<QString, QString> m_choices;
};

// "start-on-check": the window's Start is pressed the moment its own firewall check begins (the trace's "start"),
// i.e. while that check is in flight (tst_GuiNative). Armed by the op; fired from the trace.
std::atomic<bool> g_startOnCheck{false};
std::function<void()> g_onCheckStart;   // GUI thread only

// The GUI thread's heartbeat (INV-129, tst_GuiNative): a 100 ms timer on the window's thread notes the longest gap
// between its ticks while a firewall check runs; when a check ends, "asb-gui-heartbeat max-gap <ms>" says it. A GUI
// thread blocked on the check shows as a gap as long as the block.
std::atomic<bool> g_checkRunning{false};
QElapsedTimer g_heartbeatClock;   // GUI thread only
qint64 g_lastTick = -1;
qint64 g_maxGap = 0;

void heartbeatTick()
{
    const qint64 now = g_heartbeatClock.elapsed();
    if (g_lastTick >= 0 && g_checkRunning) g_maxGap = std::max(g_maxGap, now - g_lastTick);
    g_lastTick = now;
}

[[maybe_unused]] void testTrace(const char* what)   // Windows only (the firewall checks)
{
    testLine(QStringLiteral("asb-gui-firewall-check %1").arg(QLatin1String(what)));
    if (std::strcmp(what, "start") == 0) {
        QMetaObject::invokeMethod(qApp, [] {
            g_maxGap = 0;
            g_lastTick = g_heartbeatClock.isValid() ? g_heartbeatClock.elapsed() : -1;
            g_checkRunning = true;
        }, Qt::QueuedConnection);
        if (g_startOnCheck.exchange(false))
            QMetaObject::invokeMethod(qApp, [] { if (g_onCheckStart) g_onCheckStart(); }, Qt::QueuedConnection);
    } else {
        QMetaObject::invokeMethod(qApp, [] {
            heartbeatTick();   // the time since the last tick counts: a blocked thread ticks only after the check
            g_checkRunning = false;
            testLine(QStringLiteral("asb-gui-heartbeat max-gap %1").arg(g_maxGap));
        }, Qt::QueuedConnection);
    }
}

void runTestOps(BridgeWindow& window, EnvAsker& asker)
{
    auto ops = std::make_shared<QStringList>(qEnvironmentVariable("ASB_TEST_GUI_OPS").split(QLatin1Char(','), Qt::SkipEmptyParts));
    auto current = std::make_shared<QString>();
    auto next = std::make_shared<std::function<void()>>();
    BridgeWindow* w = &window;
    EnvAsker* a = &asker;
    g_heartbeatClock.start();
    auto* heartbeat = new QTimer(w);
    heartbeat->setInterval(100);
    QObject::connect(heartbeat, &QTimer::timeout, w, [] { heartbeatTick(); });
    heartbeat->start();
    *next = [w, a, ops, current, next] {
        if (ops->isEmpty()) {
            testLine(QStringLiteral("asb-gui-ops done"));
            return;
        }
        *current = ops->takeFirst().trimmed();
        if (*current == QLatin1String("stop")) w->stopService();
        else if (*current == QLatin1String("start")) w->startService();
        else if (*current == QLatin1String("save")) w->saveAndApply();
        else if (*current == QLatin1String("reload")) w->reload(true);
        // v1.1 WS-17 (both OSes): the lifecycle.
        else if (*current == QLatin1String("install")) w->installService();
        else if (*current == QLatin1String("update")) w->updateService();
        else if (*current == QLatin1String("uninstall")) w->uninstallService();
        else if (*current == QLatin1String("uninstall-settings")) {
            a->tickNext = true;
            w->uninstallService();
        } else if (*current == QLatin1String("discard")) w->discardPending();
        else if (*current == QLatin1String("restart-admin")) {
            // Not an operation: started -> this window closes (the process ends); otherwise the footer says why.
            testLine(QStringLiteral("asb-gui-op restart-admin pressed"));
            w->restartAsAdmin();
            if (!w->isVisible()) {
                testLine(QStringLiteral("asb-gui-op restart-admin started"));
                return;
            }
        } else if (*current == QLatin1String("wait")) {
            // A pause between ops while the native test reads the system's state: until the file named by
            // ASB_TEST_GUI_CONTINUE appears (the test creates it; the window removes it), else 2 s.
            testLine(QStringLiteral("asb-gui-op wait"));
            const QString file = qEnvironmentVariable("ASB_TEST_GUI_CONTINUE");
            auto* poll = new QTimer(w);
            poll->setInterval(100);
            auto started = std::make_shared<QElapsedTimer>();
            started->start();
            QObject::connect(poll, &QTimer::timeout, w, [poll, file, started, next] {
                const bool go = file.isEmpty() ? started->elapsed() >= 2000 : QFile::exists(file);
                if (!go) return;
                if (!file.isEmpty()) QFile::remove(file);
                poll->stop();
                poll->deleteLater();
                testLine(QStringLiteral("asb-gui-op wait done"));
                (*next)();
            });
            poll->start();
            return;
        }
        if (*current == QLatin1String("start-on-check")) {
            testLine(QStringLiteral("asb-gui-op start-on-check armed"));
            g_onCheckStart = [w, current, next] {
                testLine(QStringLiteral("asb-gui-op start-on-check pressed"));
                w->startService();
                if (!w->busy()) {
                    testLine(QStringLiteral("asb-gui-op %1 not-submitted: %2").arg(*current, w->footerText()));
                    QTimer::singleShot(0, w, [next] { (*next)(); });
                }
            };
            g_startOnCheck = true;
            return;   // the result line comes from operationFinished (or not-submitted above)
        }
        if (!w->busy()) {   // nothing was submitted (refused before the operation, or not an operation)
            testLine(QStringLiteral("asb-gui-op %1 not-submitted: %2").arg(*current, w->footerText()));
            QTimer::singleShot(0, w, [next] { (*next)(); });
        }
    };
    QObject::connect(w, &BridgeWindow::operationFinished, w, [w, current, next](const Result& r) {
        testLine(QStringLiteral("asb-gui-op %1 %2: %3").arg(*current, enumName(r.outcome), QString(r.message).replace(QLatin1Char('\n'), QLatin1Char(' '))));
        QTimer::singleShot(0, w, [next] { (*next)(); });
    });
    QTimer::singleShot(1000, w, [next] { (*next)(); });
}
#endif

}  // namespace

int runGui(int argc, char** argv, const GuiHosts& hosts, win::PluginPolicy plugins)
{
#ifdef _WIN32
    // Mode entry (INV-112 as amended by INV-130, WS-17 §3): the token's elevation is read first, then the guard for it,
    // before any Platform or Applier object exists. Elevated: the v1.0 job (limit 1); the window creates no process at
    // all. A standard user: a job of its own (limit 2, kill-on-close), so Restart as administrator's runas works, and
    // no Applier object is ever built. (The Linux GUI installs no guard: it launches pkexec.)
    WindowRole role = WindowRole::StandardUser;
    if (hosts.view) role = enterWindowGuard(*hosts.view, hosts.apply);
    if (hosts.guardProbe) hosts.guardProbe();
    comSecurityOnHelperThread();
#endif
    QApplication app(argc, argv);
    initialiseUi(app, plugins);
    ThemeFollower follower;
    follower.start(ThemeFollower::savedChoice());
    applyApplicationSheet(app);

#ifdef _WIN32
    const Installation::Flavor flavor = Installation::Flavor::Windows;
#else
    const Installation::Flavor flavor = Installation::Flavor::Linux;
#endif

    // The window's view: its own read-only Platform (never the Applier's).
    std::unique_ptr<IViewSource> view;
    QString error;
    int code = 0;
    Platform* vp = hosts.view ? hosts.view->platform(&error, &code) : nullptr;
    if (vp) {
        PlatformViewSource::Hooks hooks;
        hooks.autoStart = [] { return readAutoStart(); };
        hooks.serialPorts = [] { return serialPorts(); };
        hooks.dataFolder = hosts.view->dataFolder();
        hooks.journalLog = flavor == Installation::Flavor::Linux;
        view = std::make_unique<PlatformViewSource>(*vp, std::move(hooks));
    } else {
        view = std::make_unique<EmptyViewSource>(error.isEmpty() ? QStringLiteral("the data folder cannot be determined") : error);
    }

    // What the window may change. Windows: an elevated window runs the Applier in this process, on an OperationHost
    // worker over its own Platform (never the view's); a standard user's window is read-only and builds no Applier
    // (WindowSetup.h). Linux: the Applier runs in the pkexec helper (WS-9b), reached through HelperActions over
    // PkexecClient; install, update and uninstall run this window's own program, staged and verified (D60, WS-15).
    std::unique_ptr<OperationHost> operations;
    std::unique_ptr<IActions> actions;
#ifdef _WIN32
    {
        WindowActions made = windowActions(hosts.apply, role, flavor);
        operations = std::move(made.operations);
        actions = std::move(made.actions);
    }
#else
    PkexecClient pkexec;
    cli::HelperClient helperClient(pkexec);
    HelperActions::Staged staged;
    staged.expectedSha256 = hosts.programSha256;
    if (hosts.readProgram)
        staged.read = [read = hosts.readProgram] {
            HelperActions::ProgramFile f;
            read(&f.path, &f.bytes, &f.problem);
            return f;
        };
    staged.probe = hosts.probeProgram;
    staged.appImage = qEnvironmentVariable("APPIMAGE");
    actions = std::make_unique<HelperActions>(helperClient, std::move(staged));
#endif

    BridgeWindow::Options options;
    options.linuxWords = flavor == Installation::Flavor::Linux;
    options.thisBuild = hosts.thisBuild;
#ifdef _WIN32
    if (role == WindowRole::StandardUser && hosts.restartAsAdmin)
        options.restartAsAdmin = [restart = hosts.restartAsAdmin](const QString& intent) {
            const GuiHosts::Relaunch r = restart(intent);
            BridgeWindow::Options::Relaunch out;
            out.started = r.started;
            out.cancelled = r.cancelled;
            out.error = r.error;
            return out;
        };
    if (role == WindowRole::Elevated) options.intent = hosts.intent;   // a standard user's window ignores it
#endif
    // The window's own stale-rule checks: threads it owns, every WMI query bounded by a 3 s deadline; an operation
    // submitted while one runs is deferred until it ends, never waited for on the GUI thread
    // (INV-129). Declared before the window, so it outlives it; destroyed before the QApplication, so no verdict is
    // posted after it.
    std::unique_ptr<FirewallChecks> checks;
#ifdef _WIN32
#ifdef ASB_TEST_SEAMS
    const bool windowCheck = !qEnvironmentVariableIsSet("ASB_TEST_GUI_NO_FIREWALL_CHECK");
    // tst_GuiNative's bca3030 row: the behaviour before that change (no wait before an operation, no deadline on the check,
    // a check while the service starts, the worker's firewall session kept after an operation).
    const bool asBca3030 = qEnvironmentVariableIsSet("ASB_TEST_GUI_AS_BCA3030");
#else
    const bool windowCheck = true;
    const bool asBca3030 = false;
#endif
    if (hosts.view && windowCheck) {
        checks = std::make_unique<FirewallChecks>(firewallCheckWork(hosts.view->installedExecutable()),
                                                  asBca3030 ? std::chrono::milliseconds(24 * 3600 * 1000)
                                                            : std::chrono::milliseconds(3000));
#ifdef ASB_TEST_SEAMS
        if (qEnvironmentVariableIsSet("ASB_TEST_GUI_OPS"))
            checks->trace = &testTrace;
#endif
        FirewallChecks* c = checks.get();
        options.firewallCheck = [c](const BridgeConfig& config, const FirewallGeneration& g,
                                    std::function<void(IFirewallMonitor::Check, const QString&)> done) {
            return c->start(config, g, std::move(done));
        };
        if (!asBca3030) options.firewallIdle = [c](std::function<void()> whenIdle) { return c->whenIdle(std::move(whenIdle)); };
        options.checkWhileStarting = asBca3030;
        if (asBca3030 && operations) operations->setReleaseFirewallAfterOperation(false);
    }
#endif
    IAsker* asker = nullptr;
#ifdef ASB_TEST_SEAMS
    std::unique_ptr<EnvAsker> envAsker;
    if (qEnvironmentVariableIsSet("ASB_TEST_GUI_OPS")) {
        envAsker = std::make_unique<EnvAsker>();
        asker = envAsker.get();
#ifdef _WIN32
        // tst_GuiNative::standardUserWindow: the job this window is in, and whether it built the Applier (INV-130).
        const win::JobLimits job = win::currentJobLimits();
        testLine(QStringLiteral("asb-gui-job inJob=%1 limit=%2 killOnClose=%3")
                     .arg(int(job.inJob)).arg(job.activeProcessLimit).arg(int(job.killOnClose)));
#endif
        testLine(QStringLiteral("asb-gui-actions %1").arg(operations ? QStringLiteral("in-process") : QStringLiteral("no-applier")));
    }
#endif
    BridgeWindow window(*view, *actions, asker, options);
    window.show();
#ifdef ASB_TEST_SEAMS
    if (envAsker) testLine(QStringLiteral("asb-gui-window shown=%1").arg(int(window.isVisible())));
    if (envAsker) runTestOps(window, *envAsker);
    // Test builds only (tst_Dispatch): the window path runs its loop for this long, then ends with exit 0.
    if (const int ms = qEnvironmentVariableIntValue("ASB_TEST_GUI_QUIT_MS"); ms > 0)
        QTimer::singleShot(ms, &app, &QCoreApplication::quit);
#endif
    return app.exec();
}

int runSelfTestInit(int argc, char** argv, win::PluginPolicy plugins)
{
    // A temp configuration: Qt's test-mode locations for gui.ini and the image caches; the view reads no data folder.
    QStandardPaths::setTestModeEnabled(true);
#ifdef _WIN32
    comSecurityOnHelperThread();
#endif
    QApplication app(argc, argv);
    initialiseUi(app, plugins);
    ThemeFollower follower;
    follower.start(ThemeFollower::savedChoice());
    applyApplicationSheet(app);

    EmptyViewSource view;
#ifdef _WIN32
    ReadOnlyActions actions(Installation::Flavor::Windows, QStringLiteral("Self-test."));
#else
    ReadOnlyActions actions(Installation::Flavor::Linux, QStringLiteral("Self-test."));
#endif
    SilentAsker asker;
    BridgeWindow::Options options;
    options.refreshMs = 0;
    BridgeWindow window(view, actions, &asker, options);
    PaintWatch watch;
    window.installEventFilter(&watch);
    window.show();

    QElapsedTimer t;
    t.start();
    bool exposed = false;
    while (t.elapsed() < 10000) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        exposed = window.windowHandle() && window.windowHandle()->isExposed();
        if (exposed && watch.painted) break;
    }
    QJsonArray modules;
    for (const QString& m : loadedModules()) modules.append(m);
    QJsonObject report;
    report.insert(QStringLiteral("exposed"), exposed);
    report.insert(QStringLiteral("painted"), watch.painted);
    report.insert(QStringLiteral("modules"), modules);
    const QByteArray line = QJsonDocument(report).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
    window.removeEventFilter(&watch);
    return exposed && watch.painted ? 0 : 1;
}

}  // namespace asb::gui
