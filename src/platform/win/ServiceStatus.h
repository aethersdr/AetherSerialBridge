#pragma once

// The Windows service status table (WS-7; master §4.5, INV-60). Portable: the SERVICE_STATUS fields and their
// Win32 values are spelled out here, and every report goes through IServiceStatusReporter (production:
// SetServiceStatus on the handle from RegisterServiceCtrlHandlerExW; tests: a recorder; test builds of the
// service: the ServiceStatusReporter seam that can block, D59 fixtures).
//
// One ServiceStatus object serialises the ORDERLY reports under its mutex, and every report sets all the
// native fields of its row:
//
//   State                     dwCurrentState      dwControlsAccepted              exit / specific        checkPoint        waitHint
//   StartPending              START_PENDING (2)   0                               0                      1, 2, ...         10,000
//   Running                   RUNNING (4)         STOP | SHUTDOWN | PRESHUTDOWN   0                      0                 0
//   StopPending               STOP_PENDING (3)    0                               0                      1, 2, ... (from 1) 25,000
//   Stopped(normal)           STOPPED (1)         0                               0                      0                 0
//   Stopped(untrusted)        STOPPED             0                               SERVICE_SPECIFIC / 3   0                 0
//   Stopped(lock busy/insec.) STOPPED             0                               SERVICE_SPECIFIC / 4   0                 0
//   Stopped(not activated)    STOPPED             0                               0 (NO_ERROR)           0                 0
//
// Not activated (exit 6: no valid start token) is reported as a NORMAL stop (v1.1 station fix). The install sets
// fFailureActionsOnNonCrashFailures, so a STOPPED report with a non-zero dwWin32ExitCode is a failure and runs the
// recovery actions (restart after 5 / 5 / 30 s). A deliberately stopped bridge (Stop revokes the token and keeps the
// start type) was restarted every 30 s for hours, each start exiting 6 again (7024 + 7031 every time). No restart
// can ever find a token (only Start or Save & apply writes one), so this start is not a failure; the process exit
// code stays 6 and its one line (notActivatedLine) says why. Untrusted (3), lock (4) and the faults stay failures.
//
// Transitions: StartPending -> Running; StartPending -> StopPending (a stop requested during start: Running is
// never reported after a stop request); Running -> StopPending; StartPending/StopPending -> Stopped. Nothing
// leaves Stopped. Faults have no transition: they terminate without a report (S4).
//
// The final STOPPED report is NOT made through this object: it is Exit::now's report (D59), which must take no
// mutex and allocate nothing, so it is the pre-built row `stoppedRow(code)` handed to the reporter directly
// (ServiceExit.h). This object only records that the process is past the point of orderly reports.

#include <QtGlobal>

#include <mutex>

namespace asb::win {

namespace svc {
// winsvc.h / winerror.h values.
inline constexpr quint32 kOwnProcess = 0x10;          // SERVICE_WIN32_OWN_PROCESS
inline constexpr quint32 kStopped = 1;                // SERVICE_STOPPED
inline constexpr quint32 kStartPending = 2;
inline constexpr quint32 kStopPending = 3;
inline constexpr quint32 kRunning = 4;
inline constexpr quint32 kAcceptStop = 0x1;
inline constexpr quint32 kAcceptShutdown = 0x4;
inline constexpr quint32 kAcceptPreshutdown = 0x100;
inline constexpr quint32 kControlStop = 0x1;          // SERVICE_CONTROL_STOP
inline constexpr quint32 kControlInterrogate = 0x4;
inline constexpr quint32 kControlShutdown = 0x5;
inline constexpr quint32 kControlPreshutdown = 0xF;
inline constexpr quint32 kErrorServiceSpecific = 1066; // ERROR_SERVICE_SPECIFIC_ERROR
inline constexpr quint32 kNoError = 0;
inline constexpr quint32 kErrorCallNotImplemented = 120;
inline constexpr quint32 kStartWaitHintMs = 10000;
inline constexpr quint32 kStopWaitHintMs = 25000;
// The process exit code of a start that found no valid start token (exitCode(RunnerStart::NotActivated)).
inline constexpr int kNotActivatedCode = 6;
}  // namespace svc

// SERVICE_STATUS, field for field.
struct NativeServiceStatus {
    quint32 serviceType = svc::kOwnProcess;
    quint32 currentState = svc::kStopped;
    quint32 controlsAccepted = 0;
    quint32 win32ExitCode = 0;
    quint32 serviceSpecificExitCode = 0;
    quint32 checkPoint = 0;
    quint32 waitHint = 0;
    bool operator==(const NativeServiceStatus&) const = default;
};

// SetServiceStatus. May block (it is an RPC to SCM); false when the call failed.
class IServiceStatusReporter {
public:
    virtual ~IServiceStatusReporter() = default;
    virtual bool report(const NativeServiceStatus&) = 0;
};

enum class ServiceState { StartPending, Running, StopPending, Stopped };

// The rows of the table. `code` selects the Stopped row: 0 normal, 3 untrusted, 4 lock busy/insecure,
// 6 not activated (NO_ERROR, like 0: not a failure); any other code is reported as SERVICE_SPECIFIC with that code
// (best effort: such a code only reaches SCM through a report that should not have been made, so it is never
// silent).
NativeServiceStatus startPendingRow(quint32 checkPoint);
NativeServiceStatus runningRow();
NativeServiceStatus stopPendingRow(quint32 checkPoint);
NativeServiceStatus stoppedRow(int code);

// Whether the table allows `from -> to` (a repeated StartPending/StopPending is a checkpoint, allowed).
bool transitionAllowed(ServiceState from, ServiceState to);

class ServiceStatus {
public:
    explicit ServiceStatus(IServiceStatusReporter& reporter);

    // StartPending with the next checkpoint (1, 2, ...). Refused (false, nothing reported) once a stop was
    // requested or the state moved on.
    bool startPending();
    // Running. Refused once a stop was requested, and from any state but StartPending.
    bool running();
    // A stop request: StopPending with checkpoint 1 on entry, 2, 3, ... on every later call. Allowed from
    // StartPending, Running and StopPending; refused after Stopped.
    bool stopPending();
    // Marks the object Stopped: the process is past orderly reports (Exit::now makes the final report itself).
    void markStopped();

    ServiceState state() const;
    bool stopRequested() const;

    // Test seam (ASB_TEST_SEAMS service builds): the mutex that serialises orderly reports, so a test can hold
    // it and prove that a fault never waits for it (INV-82).
    std::mutex& mutexForTest() { return m_mutex; }

private:
    bool move(ServiceState to, const NativeServiceStatus& row);

    IServiceStatusReporter& m_reporter;
    mutable std::mutex m_mutex;
    ServiceState m_state = ServiceState::StartPending;
    bool m_stopRequested = false;
    quint32 m_startCheckPoint = 0;
    quint32 m_stopCheckPoint = 0;
};

}  // namespace asb::win
