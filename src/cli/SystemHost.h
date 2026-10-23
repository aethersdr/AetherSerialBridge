#pragma once

// The Host of this operating system (WS-9): LinuxHost or WindowsHost. The executables derive from it; the headless
// one adds the engine (--run, --service), which the GUI never contains (INV-1).

#ifdef _WIN32
#include "cli/WindowsHost.h"
namespace asb::cli {
using SystemHost = WindowsHost;
}
#else
#include "cli/LinuxHost.h"
namespace asb::cli {
using SystemHost = LinuxHost;
}
#endif
