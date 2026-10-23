#pragma once

// The TUI mode of both executables (master section 6, WS-12; dispatch WS-9 §4.10).
//
// Mode entry, in this order: the process-creation guard (INV-112: the elevated TUI runs the Applier; the guard is
// installed in every TUI so the order never depends on a later check), the test build's guard probe, the stop
// signals blocked before any thread exists (they are read on the event loop instead, so FTXUI's own handlers never
// end the process in the middle of an operation), then the terminal check (TERM=dumb or no terminal: the help),
// the QCoreApplication, elevation and the Platform.
//
// The loop (D16, INV-71): QCoreApplication owns it. FTXUI's App::Fullscreen() with an ftxui::Loop, and a 16 ms
// QTimer calls Loop::RunOnce() (non-blocking: pending input, then a redraw when something changed); a model
// change posts Event::Custom. Loop::HasQuitted() ends the Qt loop.

#include "cli/Commands.h"

#include <functional>

namespace asb::tui {

struct Options {
    // The Linux GUI executable: the TUI is read only there (it never runs the Applier).
    bool readOnlyExecutable = false;
    // The GUI executable's: this build's headless program beside it (the window's program, digest checked), else empty.
    QString headlessProgram;
    // Called when there is no terminal to run on (TERM=dumb, or stdin/stdout not a terminal): prints the help and
    // returns the exit code.
    std::function<int()> notATerminal;
};

// stdin and stdout are terminals, and $TERM is not "dumb".
bool terminalUsable();

// Runs the TUI until it quits; returns the exit code.
int run(cli::Host&, const Options& = {});

// Ends the process after run(): flushes stdio, then _Exit. Under the process guard no at-exit work may run that
// would create a process (LeakSanitizer's StopTheWorld does, in the sanitizer build), and the terminal was restored
// when run() returned.
[[noreturn]] void exitProcess(int code);

}  // namespace asb::tui
