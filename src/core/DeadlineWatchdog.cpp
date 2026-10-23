#include "core/DeadlineWatchdog.h"

#include "core/DeadlineWatchdog_p.h"

#include <algorithm>
#include <thread>

namespace asb {

struct DeadlineWatchdog::Impl {
    Hooks hooks;
    detail::WatchdogWaiter waiter;
    // Guarded by `waiter`'s lock.
    DeadlineState state;
    bool fired = false;
    bool stop = false;
    bool priority = false;
    long long wakeups = 0;
    std::thread thread;

    void run();
};

void DeadlineWatchdog::Impl::run()
{
    const bool raised = hooks.raisePriority();   // a failure is ignored (only latency depends on it)
    waiter.lock();
    priority = raised;
    while (!stop) {
        const RunningTime::Duration now = hooks.clock();
        if (!fired && state.expired(now)) {
            fired = true;
            const int code = state.exitCode();
            waiter.unlock();
            hooks.expire(code);   // production: faultDie, which does not return
            waiter.lock();
            continue;
        }
        RunningTime::Duration slice = kMaxSlice;
        if (!fired && state.armed() && state.deadline - now < slice) slice = state.deadline - now;
        waiter.wait(slice);
        ++wakeups;
    }
    waiter.unlock();
}

DeadlineWatchdog::DeadlineWatchdog() : DeadlineWatchdog(Hooks{}) {}

DeadlineWatchdog::DeadlineWatchdog(Hooks hooks) : d(std::make_unique<Impl>())
{
    if (!hooks.clock) hooks.clock = [] { return RunningTime::now(); };
    if (!hooks.expire) hooks.expire = [](int code) { faultDie(code); };
    if (!hooks.raisePriority) hooks.raisePriority = [] { return raiseWatchdogThreadPriority(); };
    d->hooks = std::move(hooks);
    d->thread = std::thread([impl = d.get()] { impl->run(); });
}

DeadlineWatchdog::~DeadlineWatchdog()
{
    d->waiter.lock();
    d->stop = true;
    d->waiter.notify();
    d->waiter.unlock();
    if (d->thread.joinable()) d->thread.join();
}

template <typename F>
void DeadlineWatchdog::update(F&& f)
{
    // The conversion to running time happens at the call (RunningTime.h): the remaining budget, then
    // elapsing in running time.
    d->waiter.lock();
    f(d->state);
    d->waiter.notify();
    d->waiter.unlock();
}

// Running time is sampled BEFORE steady_clock (RunningTime.h, WS-3 round 2): a pause between the two
// samples then shortens the carried-over budget, never extends it. The injected clock stands in for
// RunningTime::now in tests.
RunningTime::Duration DeadlineWatchdog::toRunning(std::chrono::steady_clock::time_point deadline) const
{
    return RunningTime::fromSteady(deadline, RunningTime::Clocks{d->hooks.clock, [] { return std::chrono::steady_clock::now(); }});
}

void DeadlineWatchdog::arm(std::chrono::steady_clock::time_point deadline, int code)
{
    const auto r = toRunning(deadline);
    update([&](DeadlineState& s) { s.arm(r, code); });
}

void DeadlineWatchdog::extend(std::chrono::steady_clock::time_point deadline)
{
    const auto r = toRunning(deadline);
    update([&](DeadlineState& s) { s.extend(r); });
}

void DeadlineWatchdog::tighten(std::chrono::steady_clock::time_point deadline, int code)
{
    const auto r = toRunning(deadline);
    update([&](DeadlineState& s) { s.tighten(r, code); });
}

void DeadlineWatchdog::expireNow()
{
    d->waiter.lock();
    const bool already = d->fired;
    d->fired = true;
    const int code = d->state.exitCode();
    d->waiter.notify();
    d->waiter.unlock();
    if (!already) d->hooks.expire(code);   // at once, on the calling thread
}

DeadlineState DeadlineWatchdog::state() const
{
    d->waiter.lock();
    const DeadlineState s = d->state;
    d->waiter.unlock();
    return s;
}

bool DeadlineWatchdog::priorityRaised() const
{
    d->waiter.lock();
    const bool p = d->priority;
    d->waiter.unlock();
    return p;
}

long long DeadlineWatchdog::wakeups() const
{
    d->waiter.lock();
    const long long w = d->wakeups;
    d->waiter.unlock();
    return w;
}

}  // namespace asb
