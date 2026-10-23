#pragma once

// Running time: a monotonic clock that does not advance while the machine is suspended (N9-1). The
// deadline watchdogs (D56, D59) measure in it, so a resume neither kills a healthy engine nor shortens
// the 35 s firewall bound: no packet is forwarded while the machine sleeps.
//
//   Windows  QueryUnbiasedInterruptTime (100 ns units; excludes sleep and hibernation). NOT
//            std::chrono::steady_clock, which MSVC implements with QueryPerformanceCounter, and QPC
//            keeps counting across sleep.
//   Linux    CLOCK_MONOTONIC (excludes suspend; CLOCK_BOOTTIME would include it). This is also what
//            libstdc++'s steady_clock reads.
//   macOS    mach_absolute_time via CLOCK_UPTIME_RAW (excludes sleep).
//
// The seams (IDeadlineWatchdog, WS-5's Deadline) take std::chrono::steady_clock::time_point. The
// mapping, applied by every IDeadlineWatchdog implementation AT THE CALL:
//
//     running deadline = RunningTime::now() + (deadline − steady_clock::now())      (fromSteady)
//
// i.e. what a steady_clock deadline carries is its remaining budget at the moment it is handed over;
// from then on that budget elapses in running time. time_point::max() means "never" and maps to
// kNever; a deadline already in the past maps to an already-expired running deadline. Callers build
// deadlines as steady_clock::now() + budget immediately before the call (a suspend between the two
// would be counted against the budget, the safe direction). On Linux the mapping is the identity up to
// the skew between the two samples.
//
// The two samples are taken at different instants, so the order matters. Running time is sampled FIRST,
// then steady_clock: a pause (deschedule, preemption) between them makes steady_now larger than the
// instant the running sample was taken, which SHORTENS the remaining budget that is carried over, never
// lengthens it. The result is therefore never later than the requested deadline (up to clock
// granularity), and the pause costs the budget only in the safe direction. Any caller of the pure
// three-argument overload must sample in that order too.

#include <chrono>
#include <functional>

namespace asb::RunningTime {

using Duration = std::chrono::nanoseconds;          // since an arbitrary, fixed origin
inline constexpr Duration kNever = Duration::max();

// The current running time.
Duration now();

// The mapping above, with both "now" samples given (pure; saturates instead of overflowing).
// `runningNow` must have been sampled BEFORE `steadyNow` (see above).
Duration fromSteady(std::chrono::steady_clock::time_point deadline, std::chrono::steady_clock::time_point steadyNow,
                    Duration runningNow);

// The two clocks fromSteady samples; a test seam, so a pause can be injected between the two samples.
struct Clocks {
    std::function<Duration()> running;
    std::function<std::chrono::steady_clock::time_point()> steady;
};
Clocks realClocks();   // { RunningTime::now, steady_clock::now }

// The mapping above, sampling `clocks.running` and then `clocks.steady`.
Duration fromSteady(std::chrono::steady_clock::time_point deadline, const Clocks& clocks);
// The mapping above, sampling both real clocks now (running time first). The IDeadlineWatchdog
// implementations call this one.
Duration fromSteady(std::chrono::steady_clock::time_point deadline);

}  // namespace asb::RunningTime
