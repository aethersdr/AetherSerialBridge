#pragma once

// The service's final report and its exit paths (WS-7; S12, D59, INV-93). Portable.
//
// Every exit of the service process is Exit::now(killDeadline, code, &ServiceExit::reportStopped):
//  1. the kill deadline is tightened to now + 2 s FIRST (Exit.h),
//  2. then reportStopped(code) makes the pre-built STOPPED report (stoppedRow(code)) straight through the
//     final reporter: no mutex, no allocation, no log — SetServiceStatus may block, and then the kill deadline
//     ends the process with faultDie(code) and no report,
//  3. then TerminateProcess(code).
// Faults (Fault::die, the firewall watchdog's faultDie(7), the kill deadline itself) make no report at all.

#include "platform/win/ServiceStatus.h"

namespace asb::win::ServiceExit {

// The reporter the final report uses; set once, after the dispatcher connected and before any exit path can
// run. nullptr (the default) = no SCM connection: reportStopped does nothing.
void setFinalReporter(IServiceStatusReporter*);
IServiceStatusReporter* finalReporter();

// Exit::now's `report`: stoppedRow(code) handed to the final reporter. Lock-free and allocation-free.
void reportStopped(int code);

}  // namespace asb::win::ServiceExit
