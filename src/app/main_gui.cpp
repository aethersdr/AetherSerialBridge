// AetherSerialBridge (Windows) / aether-serial-bridge-gui (Linux): the GUI executable (D1). It
// never contains the engine (INV-1). Identity, dispatch, the window (WS-10, gui/GuiMain.h), the hidden
// --self-test-init, and the read-only commands (--status, --show-config) through the shared runCommand (WS-9);
// every other command is the console executable's.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>


#include "HeadlessDigest.h"
#include <QFile>
#include "app/Dispatch.h"
#include "app/GuiMessage.h"
#ifdef ASB_TEST_SEAMS
#include "app/GuardProbe.h"
#endif
#include "app/Identity.h"
#ifndef _WIN32
#include "app/WindowProgram.h"
#endif
#include "cli/Commands.h"
#include "cli/SystemHost.h"
#include "core/BuildInfo.h"
#include "gui/GuiMain.h"
#include "platform/win/ProcessPolicy.h"
#ifdef _WIN32
#include "platform/win/SelfElevate.h"
#endif
#ifndef _WIN32
#include "tui/Tui.h"
#endif

int main(int argc, char** argv) {
    // INV-67 (WS-7): the DLL and plugin policy is the first statement. The plugin paths are cleared only when Qt
    // is static (the platform plugin is linked in); a dynamic development build still needs its plugin folder.
    const asb::win::PluginPolicy plugins = asb::win::pluginPolicyFor(/*guiExecutable=*/true);
    if (!asb::win::applyProcessPolicy(plugins)) return 5;

#ifdef ASB_TEST_SEAMS
    // Test builds only; see main_headless.cpp.
    if (argc == 2 && std::strcmp(argv[1], "--probe-path-before-identity") == 0) {
        asb::app::probePathBeforeIdentity();
        return 0;
    }
#endif

    // Hidden (v1.1 INV-121): the program digest compiled into this window, for the release gate that compares it with
    // the program the AppImage embeds. Nothing else is initialised.
    if (argc == 2 && std::strcmp(argv[1], "--print-program-digest") == 0) {
        std::printf("%s\n", asb::generated::kHeadlessExeSha256);
        return 0;
    }

    // D20 / INV-70: the identity is set before any Q*Application and before any path use.
    asb::app::applyIdentity();

    // The first per-user path use of the process. Stands in for the vendored AppSettings
    // singleton (WS-2), which resolves <AppConfigLocation>/gui.ini when first constructed: it is
    // exactly the use the identity above must precede.
    const asb::app::StartupPaths& paths = asb::app::StartupPaths::get();
    (void)paths;

#ifdef ASB_TEST_SEAMS
    if (argc == 3 && std::strcmp(argv[1], "--identity-probe") == 0) {
        return asb::app::runIdentityProbe(argv[2], argc, argv, "gui",
                                          asb::generated::kHeadlessExeSha256);
    }
#endif

    using namespace asb::app;
    const std::vector<std::string> args(argv + 1, argv + argc);
    const Decision d = decide(BuildKind::Gui, currentEnv(), args);

    switch (d.action) {
    case Action::Gui: {
        // The window (WS-10). The only place a QApplication is constructed besides the self-test (the display check
        // happened in decide()). Two hosts: the window's read-only view and, on Windows, the Applier's own Platform
        // (never shared with the view: an operation runs on its worker while the view refreshes).
#ifdef _WIN32
        asb::cli::WindowsHost::Image image;
        image.path = QStringLiteral("aether-serial-bridge.exe");   // a bare name: beside this executable
        image.expectedSha256 = QByteArray(asb::generated::kHeadlessExeSha256);
        asb::cli::SystemHost viewHost(argc, argv, image);
        asb::cli::SystemHost applyHost(argc, argv, image);
        asb::gui::GuiHosts hosts{&viewHost, &applyHost};
        hosts.thisBuild = QStringLiteral("%1, program sha256 %2")
                              .arg(QString::fromLatin1(asb::core::version().data(), qsizetype(asb::core::version().size())),
                                   QString::fromLatin1(asb::generated::kHeadlessExeSha256).left(12));
#ifdef ASB_TEST_SEAMS
        hosts.guardProbe = &asb::app::guardProbe;   // guard_installed_before_first_object_gui (INV-112)
#endif
        // Restart as administrator (WS-17 §3, D64): this program, elevated through runas, with fixed arguments; the
        // standard user's window does not wait for it (it closes once the elevated one has started).
        hosts.restartAsAdmin = [](const QString& intent) {
            asb::win::SelfElevate::Mode mode;
            mode.wait = false;
            mode.show = true;
            const asb::win::SelfElevate::Outcome o =
                asb::win::SelfElevate::relaunch({QStringLiteral("--gui"), QStringLiteral("--intent=") + intent}, mode);
            asb::gui::GuiHosts::Relaunch r;
            r.started = o.started;
            r.cancelled = o.cancelled;
            r.error = o.error;
            return r;
        };
        hosts.intent = d.parsed.intent.value_or(QString());
        return asb::gui::runGui(argc, argv, hosts, plugins);
#else
        // The view's build identity (v1.1 D61, INV-120) is the program this window's AppImage embeds: the digest
        // compiled in (build-appimage.sh passes the release program to HeadlessDigest.cmake), never this executable.
        asb::cli::SystemHost viewHost(argc, argv, QByteArray(asb::generated::kHeadlessExeSha256));
        asb::gui::GuiHosts hosts{&viewHost, nullptr};
        hosts.thisBuild = QStringLiteral("%1, program sha256 %2")
                              .arg(QString::fromLatin1(asb::core::version().data(), qsizetype(asb::core::version().size())),
                                   QString::fromLatin1(asb::generated::kHeadlessExeSha256).left(12));
        // The staged install, update and uninstall (v1.1 D60, WS-15): this window's own program, read whole on the
        // window's worker thread. The client hashes the bytes again before any launch; the locator's reason disables it.
        hosts.programSha256 = QByteArray(asb::generated::kHeadlessExeSha256);
        hosts.readProgram = [](QString* path, QByteArray* bytes, QString* problem) {
            const asb::app::WindowProgram p = asb::app::windowProgram(QByteArray(asb::generated::kHeadlessExeSha256));
            *path = p.path;
            if (!p.digestOk) {
                *problem = p.problem;
                return;
            }
            QFile f(p.path);
            if (!f.open(QIODevice::ReadOnly)) {
                *problem = QStringLiteral("%1 cannot be read: %2").arg(p.path, f.errorString());
                return;
            }
            *bytes = f.readAll();
        };
        // The same locator's verdict before any click (WS-17): read once on the window's worker, cached by the probe.
        hosts.probeProgram = [] {
            return asb::app::windowProgram(QByteArray(asb::generated::kHeadlessExeSha256)).problem;
        };
        return asb::gui::runGui(argc, argv, hosts, plugins);
#endif
    }
    case Action::SelfTestInit:
        return asb::gui::runSelfTestInit(argc, argv, plugins);
    case Action::Help:
        // Windows: no console of its own (WS-17 §5): its parent's, else a message box.
        showGuiMessage(helpText(BuildKind::Gui), d.exitCode == kExitOk ? MessageKind::Info : MessageKind::Error);
        return d.exitCode;
    case Action::Version:
        showGuiMessage("aether-serial-bridge-gui " + std::string(asb::core::version()) + "\n", MessageKind::Info);
        return kExitOk;
    case Action::Tui: {
#ifdef _WIN32
        // decide() never returns this on Windows (the console executable has the TUI).
        showGuiMessage("Use aether-serial-bridge.exe --tui in a terminal.\n", MessageKind::Error);
        return kExitUsage;
#else
        // Linux without a display: the TUI, read only here (WS-12): this executable never runs the Applier.
        asb::cli::SystemHost host(argc, argv, QByteArray(asb::generated::kHeadlessExeSha256));
        asb::tui::Options opts;
        opts.readOnlyExecutable = true;
        // Its update instruction names this build's headless program beside it, never itself (its --tui is this
        // read-only TUI again). Inside an AppImage the TUI names the AppImage's --install instead (TuiView.h).
        if (const asb::app::WindowProgram w = asb::app::windowProgram(QByteArray(asb::generated::kHeadlessExeSha256));
            w.digestOk)
            opts.headlessProgram = w.path;
        opts.notATerminal = [] {
            std::fputs(helpText(BuildKind::Gui).c_str(), stderr);
            return kExitUsage;
        };
        asb::tui::exitProcess(asb::tui::run(host, opts));
#endif
    }
    case Action::Command: {
        // Read only (decide() admits --status and --show-config here); no QApplication on this path.
        QStringList qargs;
        for (const std::string& a : args) qargs << QString::fromLocal8Bit(a.c_str());
#ifdef _WIN32
        // The console executable beside this one is what this build installs, with the digest it was built with.
        asb::cli::WindowsHost::Image image;
        image.path = QStringLiteral("aether-serial-bridge.exe");   // a bare name: beside this executable
        image.expectedSha256 = QByteArray(asb::generated::kHeadlessExeSha256);
        asb::cli::SystemHost host(argc, argv, image);
#else
        asb::cli::SystemHost host(argc, argv, QByteArray(asb::generated::kHeadlessExeSha256));
#endif
        return asb::cli::runCommand(d.parsed, qargs, host);
    }
    case Action::Service:
    case Action::NotImplemented:
    case Action::UsageError:
        break;
    }
    if (!d.message.empty()) {
        // A refused command (exit 2): on Windows its parent's console, else a message box (WS-17 §5).
        showGuiMessage("aether-serial-bridge-gui: error: " + d.message + ".\n", MessageKind::Error);
    }
    return d.exitCode;
}
