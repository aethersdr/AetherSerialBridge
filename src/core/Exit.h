#pragma once

// Process-exit primitives and the deadline watchdog contract (D28, D39, D56, D59; S4, S12, S20).
// WS-3 owns the contract; WS-4 implements it for Windows and POSIX (Exit_win.cpp / Exit_posix.cpp,
// DeadlineWatchdog_win.cpp / _posix.cpp); WS-5's runner and WS-7/WS-8's service hosts use it.
// There is no reporting watchdog (D59).

#include "core/RunningTime.h"

#include <chrono>
#include <optional>

namespace asb {

// A fault: TerminateProcess / _exit(code) at once. No I/O of any kind — no log, no status, no SCM
// report, no lock release, no allocation. SCM / systemd see an unexpected exit and run recovery.
[[noreturn]] void faultDie(int code);

// A deadline that terminates the process with faultDie(code) when it passes, and does nothing else.
// Each instance runs its own thread, created at process start (never at arm time), waiting on an
// event/condition that every call below signals; the thread re-reads the deadline after each wake and
// waits at most 1 s per slice. Two instances per service process (D59): the firewall watchdog (code 7,
// D56) and the kill deadline (orderly stop and every exit, D59); they are independent.
//
// Time base (N9-1, README builder nit): deadlines are steady_clock::time_point values; an
// implementation converts each to RUNNING time at the call with RunningTime::fromSteady (remaining
// budget at the call, then elapsing in running time: Windows QueryUnbiasedInterruptTime, Linux
// CLOCK_MONOTONIC — see RunningTime.h), and waits on the same base (Windows 8+ wait timeouts exclude
// low-power time; Linux pthread_cond_timedwait on a CLOCK_MONOTONIC condition). A suspend therefore
// neither fires nor shortens a deadline.
//
// State: a never-armed watchdog has deadline = +infinity and no code, and never fires.
//  arm(d, c)      never armed: activates it with (d, c). Already armed: the same as tighten(d, c), so a
//                 late arm can never move an armed deadline later.
//  extend(d)      armed: takes d only if it is later than the current deadline (code unchanged).
//                 Never armed: ignored.
//  tighten(d, c)  never armed: ACTIVATES it with (d, c). Armed: takes effect only if d is earlier, and
//                 then also takes c; a later d is ignored (deadline and code unchanged).
//  expireNow()    faultDie(current code) at once (kNeverArmedCode if it was never armed).
// DeadlineState below is the reference model of exactly these rules.
class IDeadlineWatchdog {
public:
    virtual ~IDeadlineWatchdog() = default;
    virtual void arm(std::chrono::steady_clock::time_point deadline, int code) = 0;
    virtual void extend(std::chrono::steady_clock::time_point deadline) = 0;
    virtual void tighten(std::chrono::steady_clock::time_point deadline, int code) = 0;
    virtual void expireNow() = 0;
};

// The exit code expireNow() uses on a watchdog nobody armed: the generic fault code (S4: exit 1).
inline constexpr int kNeverArmedCode = 1;

// Reference model of the IDeadlineWatchdog state rules, in running time. Pure, not thread-safe: an
// implementation holds one under its own lock (or as atomics) and signals its thread after each call.
struct DeadlineState {
    RunningTime::Duration deadline = RunningTime::kNever;
    std::optional<int> code;   // nullopt = never armed

    bool armed() const { return code.has_value(); }
    void arm(RunningTime::Duration d, int c)
    {
        if (!armed()) {
            deadline = d;
            code = c;
        } else {
            tighten(d, c);
        }
    }
    void extend(RunningTime::Duration d)
    {
        if (armed() && d > deadline) deadline = d;
    }
    void tighten(RunningTime::Duration d, int c)
    {
        if (!armed() || d < deadline) {
            deadline = d;
            code = c;
        }
    }
    // Whether the watchdog fires at running time `now`, and with which code.
    bool expired(RunningTime::Duration now) const { return armed() && now >= deadline; }
    int exitCode() const { return code.value_or(kNeverArmedCode); }
};

namespace Exit {

// How long Exit::now leaves the final report before the kill deadline ends the process.
inline constexpr std::chrono::seconds kReportBudget{2};

// The one exit primitive of a service process (S12, D59, INV-93):
//  1. killDeadline.tighten(steady_clock::now() + kReportBudget, code) FIRST;
//  2. then, if report != nullptr, report(code): the pre-built final report (Windows: SetServiceStatus
//     STOPPED). It may block; the kill deadline then ends the process with faultDie(code);
//  3. then TerminateProcess / _exit(code).
// No allocation, no mutex, no log. Linux --service passes report = nullptr (no SCM).
[[noreturn]] void now(IDeadlineWatchdog& killDeadline, int code, void (*report)(int code));

}  // namespace Exit
}  // namespace asb
