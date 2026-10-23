#pragma once

// Test builds only (ASB_TEST_SEAMS): guard_installed_before_first_object (INV-112). With ASB_TEST_GUARD_PROBE set,
// the CLI's mode entry calls probe() right after the guard's call site and before the first Applier or Platform
// object: it tries to create a process (Linux fork, Windows CreateProcessW) and says on stderr whether that worked:
//   "ASB-GUARD-PROBE denied: <error>"   or   "ASB-GUARD-PROBE created a process"
// ASB_TEST_GUARD_PROBE=exit then ends the process (0 denied, 1 created) so the test changes nothing. A mutant that
// moves the guard below the first object makes the probe succeed.
//
// The only process creation compiled into the product's executables, and only into a test build: the
// process_creation_audit allow-lists this object by name, and the release configuration has no such file.

namespace asb::app {

void guardProbe();

}  // namespace asb::app
