#pragma once

// Private: the platform wait primitive of DeadlineWatchdog (DeadlineWatchdog_posix.cpp / _win.cpp).

#include <chrono>
#include <memory>

namespace asb::detail {

class WatchdogWaiter {
public:
    WatchdogWaiter();
    ~WatchdogWaiter();
    WatchdogWaiter(const WatchdogWaiter&) = delete;
    WatchdogWaiter& operator=(const WatchdogWaiter&) = delete;

    void lock();
    void unlock();
    // Called with the lock held: waits at most `timeout` of running time, or until notify(); returns
    // with the lock held. Spurious wakes are allowed (the caller re-reads its state).
    void wait(std::chrono::nanoseconds timeout);
    // Wakes the waiter. Called with the lock held, so a wake is never lost.
    void notify();

    struct Native;

private:
    std::unique_ptr<Native> n;
};

}  // namespace asb::detail
