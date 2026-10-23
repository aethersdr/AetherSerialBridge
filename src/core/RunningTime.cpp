#include "core/RunningTime.h"

#include <QtGlobal>
#include <QtNumeric>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <realtimeapiset.h>
#else
#  include <time.h>
#endif

#include <limits>

namespace asb::RunningTime {

Duration now()
{
#ifdef Q_OS_WIN
    ULONGLONG t = 0;   // 100 ns units
    QueryUnbiasedInterruptTime(&t);
    return Duration(qint64(t) * 100);
#else
#  if defined(Q_OS_MACOS)
    constexpr clockid_t clock = CLOCK_UPTIME_RAW;
#  else
    constexpr clockid_t clock = CLOCK_MONOTONIC;
#  endif
    timespec ts{};
    clock_gettime(clock, &ts);
    return std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
#endif
}

Duration fromSteady(std::chrono::steady_clock::time_point deadline, std::chrono::steady_clock::time_point steadyNow,
                    Duration runningNow)
{
    if (deadline == std::chrono::steady_clock::time_point::max()) return kNever;
    using Rep = Duration::rep;
    constexpr Rep kMax = std::numeric_limits<Rep>::max();
    constexpr Rep kMin = std::numeric_limits<Rep>::min();
    // remaining = deadline − steadyNow, in nanoseconds, saturating.
    const auto d = std::chrono::duration_cast<Duration>(deadline.time_since_epoch()).count();
    const auto s = std::chrono::duration_cast<Duration>(steadyNow.time_since_epoch()).count();
    Rep remaining;
    if (qSubOverflow(d, s, &remaining)) remaining = d > s ? kMax : kMin;
    Rep out;
    if (qAddOverflow(runningNow.count(), remaining, &out)) out = remaining > 0 ? kMax : kMin;
    // kNever is reserved for "never armed"; a finite deadline saturates just below it.
    if (out == kMax) out = kMax - 1;
    return Duration(out);
}

Clocks realClocks()
{
    return {[] { return now(); }, [] { return std::chrono::steady_clock::now(); }};
}

Duration fromSteady(std::chrono::steady_clock::time_point deadline, const Clocks& clocks)
{
    // Running time FIRST, then steady_clock. The result is runningNow + (deadline - steadyNow); if the
    // thread is paused between the samples, steadyNow is later than the instant runningNow describes,
    // so the pause SHORTENS the remaining budget and never extends the deadline. The other order would
    // add the pause to the result, letting a watchdog fire later than requested.
    const Duration runningNow = clocks.running();
    const auto steadyNow = clocks.steady();
    return fromSteady(deadline, steadyNow, runningNow);
}

Duration fromSteady(std::chrono::steady_clock::time_point deadline)
{
    return fromSteady(deadline, realClocks());
}

}  // namespace asb::RunningTime
