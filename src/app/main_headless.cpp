// aether-serial-bridge: the console executable (D1). Service host, CLI and TUI; no QtGui code
// (INV-37). Identity (D20), then dispatch, then the mode: the service host, a command (WS-9 runCommand on the
// HeadlessHost), or the TUI (WS-12).

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app/Dispatch.h"
#include "app/Identity.h"
#include "app/ServiceHost.h"
#include "core/BuildInfo.h"
#include "tui/Tui.h"
#include "platform/win/ProcessPolicy.h"
#ifdef _WIN32
#include "platform/win/ServiceMain.h"
#endif

#include "app/HeadlessHost.h"
#include "cli/Commands.h"

int main(int argc, char** argv) {
    // INV-67 (WS-7): the DLL and plugin policy is the first statement (SetDefaultDllDirectories(SYSTEM32), plugin
    // paths cleared: the headless executable loads no plugin). A policy that cannot be applied ends the process.
    if (!asb::win::applyProcessPolicy(asb::win::pluginPolicyFor(/*guiExecutable=*/false))) return 5;

#ifdef ASB_TEST_SEAMS
    // Test builds only. Deliberately BEFORE the identity, to prove that a path resolved first is
    // refused (INV-70). Never present in a release build.
    if (argc == 2 && std::strcmp(argv[1], "--probe-path-before-identity") == 0) {
        asb::app::probePathBeforeIdentity();
        return 0;
    }
#endif

    // D20 / INV-70: the identity is set before any Q*Application and before any path use.
    asb::app::applyIdentity();

#ifdef ASB_TEST_SEAMS
    if (argc == 3 && std::strcmp(argv[1], "--identity-probe") == 0) {
        return asb::app::runIdentityProbe(argv[2], argc, argv, "headless", nullptr);
    }
#ifdef __linux__
    // The systemd suite's Applier operations, until WS-9's CLI exists (WS-8). Test builds only.
    if (argc >= 3 && std::strcmp(argv[1], "--test-op") == 0) {
        return asb::app::runTestOperation(argc, argv);
    }
#endif
#endif

    using namespace asb::app;
    const std::vector<std::string> args(argv + 1, argv + argc);
    const Decision d = decide(BuildKind::Headless, currentEnv(), args);

    switch (d.action) {
    case Action::Help:
        std::fputs(helpText(BuildKind::Headless).c_str(), d.exitCode == kExitOk ? stdout : stderr);
        return d.exitCode;
    case Action::Version:
        std::printf("aether-serial-bridge %s\n", std::string(asb::core::version()).c_str());
        return kExitOk;
    case Action::Tui: {
        // WS-12: the mode entry (guard first, INV-112) and the terminal check are tui::run's. The process ends through
        // exitProcess: under the guard no at-exit work may create a process.
        HeadlessHost host(argc, argv);
        asb::tui::Options opts;
        opts.notATerminal = [] {
            std::fputs(helpText(BuildKind::Headless).c_str(), stderr);
            return kExitUsage;
        };
        asb::tui::exitProcess(asb::tui::run(host, opts));
    }
    case Action::Service:
        // The managed engine: only here, and each host refuses (exit 2) unless the service manager started it
        // (INV-90, INV-100). Neither returns in production.
#ifdef _WIN32
        asb::win::runService(argc, argv);
#else
        return asb::app::runLinuxService(argc, argv);
#endif
    case Action::Command: {
        QStringList qargs;
        for (const std::string& a : args) qargs << QString::fromLocal8Bit(a.c_str());
        HeadlessHost host(argc, argv);
        return asb::cli::runCommand(d.parsed, qargs, host);
    }
    case Action::Gui:           // decide() never returns these two for the headless build
    case Action::SelfTestInit:
    case Action::NotImplemented:
    case Action::UsageError:
        break;
    }
    if (!d.message.empty()) {
        std::fprintf(stderr, "aether-serial-bridge: error: %s.\n", d.message.c_str());
    }
    if (d.action == Action::UsageError) {
        std::fputs("Run --help to see the commands.\n", stderr);
    }
    return d.exitCode;
}
