#pragma once

// Mode dispatch (master §4.10, WS-9). Pure: decide() looks at nothing but its arguments, so the display/TTY rules
// are testable without a terminal or a display. Nothing here touches Qt's application objects: dispatch runs after
// the identity (D20) and BEFORE any Q*Application exists, and a QApplication is constructed only when the decision
// is Action::Gui. The command line itself is cli::parse (CommandLine.h); a command is run by cli::runCommand.

#include "cli/CommandLine.h"

#include <string>
#include <vector>

namespace asb::app {

enum class BuildKind { Headless, Gui };

// The terminal interface (WS-12). No arguments on a terminal open it; --tui forces it, and the TUI itself prints the
// help (exit 2) when there is no terminal after all (TERM=dumb, or stdin/stdout not a terminal; section 6.3).
inline constexpr bool kTuiAvailable = true;

struct DispatchEnv {
    bool stdinIsTty = false;
    bool stdoutIsTty = false;
    bool hasDisplay = false;  // $WAYLAND_DISPLAY or $DISPLAY (Linux only; ignored on Windows)
    bool windows = false;
    std::string term;  // $TERM
};

enum class Action {
    Help,            // print usage, exit with Decision::exitCode (0 asked for, 2 nothing to do)
    Version,
    Gui,             // construct QApplication
    Tui,             // WS-12 (tui::run)
    Service,         // the managed config runs only here (WS-7 Windows, WS-8 Linux); headless only
    Command,         // cli::runCommand(Decision::parsed, ...)
    SelfTestInit,    // the GUI executable's hidden --self-test-init (WS-10 for WS-13)
    NotImplemented,  // a mode a later workstream implements (exit 1)
    UsageError,      // message names the problem; exit code 2
};

struct Decision {
    Action action = Action::UsageError;
    int exitCode = 2;
    std::string message;  // what to tell the user (stderr), empty when none
    cli::Parsed parsed;   // Action::Command
};

inline constexpr int kExitOk = 0;
inline constexpr int kExitFailed = 1;
inline constexpr int kExitUsage = 2;

Decision decide(BuildKind build, const DispatchEnv& env, const std::vector<std::string>& args);

// Reads the real process environment (isatty, $TERM, $WAYLAND_DISPLAY/$DISPLAY).
DispatchEnv currentEnv();

// Usage text for --help and for "nothing to do".
std::string helpText(BuildKind build);

}  // namespace asb::app
