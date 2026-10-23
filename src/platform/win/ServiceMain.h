#pragma once

// `--service` on Windows (WS-7; master §4.5, §4.6 "Engine lock + start token", D56, D59; INV-7, 22, 60, 81, 82,
// 93, 103, 113, 117, 119). Linked only into the headless executable.
//
// Threads: main (QCoreApplication, engine), dispatcher (StartServiceCtrlDispatcherW on a thread of its own; its
// ServiceMain registers the control handler and parks), side-channel worker (log + status I/O), the kill deadline
// and the firewall watchdog (both DeadlineWatchdogs created FIRST, at process start), and the firewall monitor
// (created by BridgeRunner before any listener).
//
// Start order: watchdogs -> fault handlers -> dispatcher connect (no SCM: exit 2, nothing reported) ->
// StartPending -> serviceLocations() (nullopt: exit 3, kWindowsDataFolderError) -> windowsPaths() (the lock
// directory unknown: exit 4) -> process-creation guard (exit 5) -> COM security -> BridgeRunner::start()
// (engine.lock busy/missing/insecure/wrong identity: 4; untrusted folder: 3; no valid start token: 6, said in one
// line and reported as a normal stop, NO_ERROR, so SCM runs no recovery action (v1.1 station fix); startup
// firewall check failed or overdue: faultDie(7), no report) -> Running -> the event loop. Nothing in the data
// folder is touched before the runner's Service-mode trust passed: bridge.log is opened on its first line and
// status.json written on the first publish (INV-22); orphaned status.json temps of a killed engine are removed
// before that first status write.
//
// Faults (std::terminate, an exception escaping an event handler or slot, qFatal, an unhandled SEH exception)
// end the process with faultDie(1): no report, no log, no lock release (INV-82). SCM then runs the recovery
// actions (INV-7).

namespace asb::win {

// Never returns: every path ends in Exit::now or faultDie.
[[noreturn]] void runService(int argc, char** argv);

}  // namespace asb::win
