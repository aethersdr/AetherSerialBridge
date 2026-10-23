#pragma once

// `aether-serial-bridge --helper` (master §5.7; D2, D34, D57; INV-50, 51, 89, 112, 118). Linux only. The Linux GUI
// runs unprivileged and asks for every change through `pkexec /usr/local/bin/aether-serial-bridge --helper`
// (the polkit action org.aethersdr.aetherserialbridge.helper, auth_admin); this is that helper.
//
// Order (each step only after the one before succeeded):
//  1. mode entry: the process-creation guard (seccomp, INV-112), then a test build's guard probe;
//  2. authorization: geteuid() == 0, nothing else. PKEXEC_UID is never authority; it only names the requesting user
//     in the audit line (INV-50);
//  3. the request: read with HelperInput's bounds (≤ 256 KiB cumulative, one 10 s deadline for its EOF), then exactly
//     one JSON value (HelperProtocol). Any failure up to here: exit 2, a line on stderr, nothing on stdout, no
//     QCoreApplication, no Platform, no lock, nothing changed (INV-89);
//  4. the audit line (stderr and syslog authpriv), then the QCoreApplication and the Platform;
//  5. the operation on an OperationHost worker; its single JSON response is written only after `finished` (D57,
//     INV-118), then the helper exits. An interim notification is a stderr line, never stdout. inspect is read only
//     (no lock, no operation).
//
// Exit codes (INV-124, the contract): only 0 a response with "ok":true; 1 a response with "ok":false; 2 refused (not
// root, the request too large, late, unreadable or not exactly one valid request, or install/uninstall outside a staged
// program; nothing on stdout); 5 the guard could not be installed. Never 126 or 127, which the client reads as
// pkexec's own (cancelled, not authorized), and never 3, 4 or 6, which are the staging script's own (nothing ran).
//
// v1.1 (WS-15, INV-123): install and uninstall run only when the running program is exactly
// /usr/local/lib/aether-serial-bridge/staging.<8 characters>/aether-serial-bridge (the window's staged copy, D60); the
// path is the Host's ownExecutable() made canonical. Anywhere else, the installed program included, they are refused
// (exit 2) after the parse and before any Platform. The audit line says "staged" for a staged run.

#include "cli/Commands.h"
#include "cli/HelperProtocol.h"

#include <chrono>

namespace asb::cli {

struct HelperOptions {
    int inputFd = 0;
    int outputFd = 1;
    std::chrono::milliseconds inputDeadline = helper::kInputDeadline;
    qsizetype maxRequest = helper::kMaxRequestBytes;
    bool syslog = true;   // the audit line also goes to syslog (authpriv); unit tests turn it off
};

int runHelper(Host&, Console&, const HelperOptions& = {});

}  // namespace asb::cli
