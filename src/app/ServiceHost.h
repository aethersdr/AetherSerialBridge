#pragma once

// The service hosts of the headless executable. Linux: runLinuxService (WS-8, ServiceHost_linux.cpp), the
// body of `aether-serial-bridge --service` under systemd. Windows: WS-7.

namespace asb::app {

#ifdef __linux__
// Never returns in production: every end goes through Exit::now (or exits 2/5 before anything exists).
int runLinuxService(int argc, char** argv);
#endif

#ifdef ASB_TEST_SEAMS
// Test builds only (the systemd suite, until WS-9's CLI exists): `--test-op <operation> [arguments]` runs one
// Applier operation through OperationHost on the Linux Platform and prints the Result as one JSON line.
int runTestOperation(int argc, char** argv);
#endif

}  // namespace asb::app
