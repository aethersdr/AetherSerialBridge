// DeadlineWatchdog wait primitive, POSIX: a pthread condition on CLOCK_MONOTONIC (running time on
// Linux: it excludes suspend), so the wait and the deadline use the same base (N9-1).

#include "core/DeadlineWatchdog.h"
#include "core/DeadlineWatchdog_p.h"

#include <pthread.h>
#include <time.h>

namespace asb {

bool raiseWatchdogThreadPriority() { return true; }   // Linux: no priority request (master §4.6)

namespace detail {

struct WatchdogWaiter::Native {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

WatchdogWaiter::WatchdogWaiter() : n(std::make_unique<Native>())
{
    pthread_mutex_init(&n->mutex, nullptr);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#if !defined(__APPLE__)
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&n->cond, &attr);
    pthread_condattr_destroy(&attr);
}

WatchdogWaiter::~WatchdogWaiter()
{
    pthread_cond_destroy(&n->cond);
    pthread_mutex_destroy(&n->mutex);
}

void WatchdogWaiter::lock() { pthread_mutex_lock(&n->mutex); }
void WatchdogWaiter::unlock() { pthread_mutex_unlock(&n->mutex); }
void WatchdogWaiter::notify() { pthread_cond_signal(&n->cond); }

void WatchdogWaiter::wait(std::chrono::nanoseconds timeout)
{
    if (timeout <= std::chrono::nanoseconds::zero()) return;
    timespec ts{};
#if defined(__APPLE__)
    clock_gettime(CLOCK_REALTIME, &ts);   // macOS (later): no condattr clock; a slice is at most 1 s
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    ts.tv_sec += time_t(secs.count());
    ts.tv_nsec += long((timeout - secs).count());
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&n->cond, &n->mutex, &ts);
}

}  // namespace detail
}  // namespace asb
