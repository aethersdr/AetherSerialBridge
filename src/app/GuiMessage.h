#pragma once

// The GUI executable's own messages (v1.1 WS-17 §5, owner debt): its usage errors, --help and --version. On Windows
// it is a WIN32-subsystem program with no console of its own, so a message goes, in this order, to
//   1. its standard handle, when it has one (a pipe or a file: a script, a test, `> out.txt`);
//   2. the console of the process that started it, when there is one (AttachConsole(ATTACH_PARENT_PROCESS), then
//      CONOUT$: typed in cmd or PowerShell);
//   3. a message box (double-clicked, or started by a shortcut with arguments).
// Test builds: ASB_TEST_MESSAGEBOX=record:<file> writes the box's text to <file> instead of showing it (and skips 1 and
// 2). Elsewhere (Linux) the message is printed to the standard stream as before. Exit codes never change.

#include <string>

namespace asb::app {

enum class MessageKind { Info, Error };

void showGuiMessage(const std::string& text, MessageKind kind);

}  // namespace asb::app
