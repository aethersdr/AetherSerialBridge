#include "app/Dispatch.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "cli/Commands.h"

namespace asb::app {

namespace {

// The value of an environment variable, empty when unset (getenv is deprecated on MSVC).
std::string envValue(const char* name) {
#ifdef _WIN32
    char* buf = nullptr;
    std::size_t len = 0;
    std::string v;
    if (_dupenv_s(&buf, &len, name) == 0 && buf) {
        v = buf;
    }
    std::free(buf);
    return v;
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

Decision usage(std::string message) {
    return Decision{Action::UsageError, kExitUsage, std::move(message), {}};
}

// A terminal on both ends, and a TUI in this build (WS-12).
bool tuiFits(const DispatchEnv& env, bool needsTerm) {
    return kTuiAvailable && env.stdinIsTty && env.stdoutIsTty && (!needsTerm || env.term != "dumb");
}

std::string text(const QString& s) {
    return s.toStdString();
}

}  // namespace

Decision decide(BuildKind build, const DispatchEnv& env, const std::vector<std::string>& args) {
    const char* consoleExe = env.windows ? "aether-serial-bridge.exe" : "aether-serial-bridge";

    if (args.empty()) {
        if (build == BuildKind::Gui) {
            if (env.windows || env.hasDisplay) {
                return Decision{Action::Gui, kExitOk, {}, {}};
            }
            if (tuiFits(env, false)) {
                return Decision{Action::Tui, kExitOk, {}, {}};
            }
        } else if (tuiFits(env, true)) {
            return Decision{Action::Tui, kExitOk, {}, {}};
        }
        // Nothing to run and nobody to ask (or no TUI yet): show the help, as a usage failure.
        return Decision{Action::Help, kExitUsage, {}, {}};
    }

    QStringList qargs;
    for (const std::string& a : args) qargs << QString::fromLocal8Bit(a.c_str());
    const cli::Parsed p = cli::parse(qargs);
    if (!p.ok()) return usage(text(p.usageError));

    switch (p.command) {
    case cli::Command::Help:
        return Decision{Action::Help, kExitOk, {}, {}};
    case cli::Command::Version:
        return Decision{Action::Version, kExitOk, {}, {}};
    case cli::Command::Gui:
        if (build == BuildKind::Headless) {
            return usage("--gui is not available in the headless executable; use the GUI executable");
        }
        if (!env.windows && !env.hasDisplay) {
            return usage("--gui needs a display ($WAYLAND_DISPLAY or $DISPLAY is not set)");
        }
        return Decision{Action::Gui, kExitOk, {}, p};   // p.intent: the elevated window's --intent (WS-17)
    case cli::Command::Tui:
        if (build == BuildKind::Gui && env.windows) {
            return usage("use aether-serial-bridge.exe --tui in a terminal");
        }
        return Decision{Action::Tui, kExitOk, {}, {}};
    case cli::Command::Service:
        // The managed config runs only here: the Windows service host (WS-7) and the Linux one (WS-8), each of which
        // refuses (exit 2) when the service manager did not start it (INV-90, INV-100). Never the GUI executable.
        if (build == BuildKind::Gui) {
            return usage("--service is not available in the GUI executable");
        }
        return Decision{Action::Service, kExitOk, {}, {}};
    case cli::Command::SelfTestInit:
        if (build == BuildKind::Gui) {
            return Decision{Action::SelfTestInit, kExitOk, {}, {}};
        }
        return usage("--self-test-init is not available in the headless executable");
    case cli::Command::UninstallFromApps:
        // WS-16 (D63): the Apps & features entry's UninstallString names the installed console executable.
        if (!env.windows) {
            return usage("--uninstall-from-apps is only for Windows' Apps & features; use --uninstall");
        }
        if (build == BuildKind::Gui) {
            return usage(std::string("run this with the console executable: ") + consoleExe + " --uninstall-from-apps");
        }
        break;
    case cli::Command::Helper:
        // Linux, the console executable only (the polkit action's exec.path): runCommand runs it (WS-9b, §5.7). The GUI
        // executable refuses it below, naming the console executable.
        if (env.windows) {
            return usage("--helper is not available on Windows");
        }
        break;
    default:
        break;
    }
    // The GUI executable shows the managed state (read only); every other command, and every scratch config, is the
    // console executable's (WS-9a; WS-10 may wire the Windows GUI executable's elevated commands).
    if (build == BuildKind::Gui) {
        const bool readOnly = (p.command == cli::Command::Status || p.command == cli::Command::ShowConfig) && !p.config;
        if (!readOnly) {
            return usage(std::string("run this with the console executable: ") + consoleExe + " " +
                         text(qargs.join(QLatin1Char(' '))));
        }
    }
    return Decision{Action::Command, kExitOk, {}, p};
}

DispatchEnv currentEnv() {
    DispatchEnv e;
#ifdef _WIN32
    e.windows = true;
    e.stdinIsTty = _isatty(_fileno(stdin)) != 0;
    e.stdoutIsTty = _isatty(_fileno(stdout)) != 0;
#else
    e.stdinIsTty = ::isatty(0) != 0;
    e.stdoutIsTty = ::isatty(1) != 0;
    e.hasDisplay = !envValue("WAYLAND_DISPLAY").empty() || !envValue("DISPLAY").empty();
#endif
    e.term = envValue("TERM");
    return e;
}

std::string helpText(BuildKind build) {
#ifdef _WIN32
    const bool windows = true;
#else
    const bool windows = false;
#endif
    return text(cli::usageText(build == BuildKind::Gui, windows)) + "\n";
}

}  // namespace asb::app
