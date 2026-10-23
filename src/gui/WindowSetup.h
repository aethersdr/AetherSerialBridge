#pragma once

// The window's mode entry and actions, decided before the window exists (v1.1 WS-17 §3; D64, D65, INV-130). Portable
// (no Windows headers), so tst_GuiMain checks the decisions on every OS over a fake host.
//
//  enterWindowGuard   Windows reads the token's elevation FIRST (no Platform or Applier object exists yet), then:
//                     elevated -> the v1.0 job guard (ActiveProcessLimit 1, INV-112) before any Platform or Applier;
//                     standard user -> a job of its own with limit 2 and kill-on-job-close, which does not stop the
//                     `runas` relaunch of Restart as administrator (D65; the limit is settled by HT W-2 step 5).
//  windowActions      elevated -> the Applier in this process (InProcessActions over an OperationHost on the apply
//                     host's own Platform); standard user -> ReadOnlyActions, and the apply host's platform() is never
//                     called: a standard user's window builds no Applier object, ever.
//  parseIntent        --intent=<install|update|uninstall|none> as the command line parsed it (cli::Parsed::intent).

#include "cli/Commands.h"
#include "core/OperationHost.h"
#include "gui/Actions.h"

#include <QString>

#include <memory>

namespace asb::gui {

enum class WindowRole { Elevated, StandardUser };

// Windows only (the caller): installs the guard for the role read from `apply` (nullptr: a standard user).
WindowRole enterWindowGuard(cli::Host& view, cli::Host* apply);

struct WindowActions {
    std::unique_ptr<OperationHost> operations;   // the elevated window's Applier worker; nullptr otherwise
    std::unique_ptr<IActions> actions;
};
WindowActions windowActions(cli::Host* apply, WindowRole role, Installation::Flavor flavor);

// The read-only reason a standard user's window shows (WS-17 §1).
QString standardUserReason();

}  // namespace asb::gui
