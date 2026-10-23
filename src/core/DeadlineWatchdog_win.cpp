// DeadlineWatchdog wait primitive, Windows: an auto-reset event waited on with a timeout. On Windows 8
// and later wait timeouts are measured in unbiased interrupt time (low-power time excluded), the same
// base as RunningTime::now (QueryUnbiasedInterruptTime), N9-1.

#include "core/DeadlineWatchdog.h"
#include "core/DeadlineWatchdog_p.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace asb {

bool raiseWatchdogThreadPriority()
{
    return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
}

namespace detail {

struct WatchdogWaiter::Native {
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE event = nullptr;
};

WatchdogWaiter::WatchdogWaiter() : n(std::make_unique<Native>())
{
    n->event = CreateEventW(nullptr, FALSE /* auto-reset */, FALSE, nullptr);
}

WatchdogWaiter::~WatchdogWaiter()
{
    if (n->event) CloseHandle(n->event);
}

void WatchdogWaiter::lock() { AcquireSRWLockExclusive(&n->lock); }
void WatchdogWaiter::unlock() { ReleaseSRWLockExclusive(&n->lock); }
void WatchdogWaiter::notify() { SetEvent(n->event); }   // latched: a wake before the wait is not lost

void WatchdogWaiter::wait(std::chrono::nanoseconds timeout)
{
    if (timeout <= std::chrono::nanoseconds::zero()) return;
    // Round up, so a wake never comes before the deadline is due.
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(timeout).count();
    ReleaseSRWLockExclusive(&n->lock);
    if (n->event) {
        WaitForSingleObject(n->event, DWORD(ms));
    } else {
        Sleep(DWORD(ms));   // no event (CreateEventW failed): plain slices, still bounded by 1 s
    }
    AcquireSRWLockExclusive(&n->lock);
}

}  // namespace detail
}  // namespace asb
