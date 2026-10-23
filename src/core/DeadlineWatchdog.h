#pragma once

// IDeadlineWatchdog (Exit.h, WS-3 contract) for Windows and POSIX (WS-4; D56, D59, N9-1).
//
// One thread per instance, created in the constructor (at process start, never at arm time). It waits
// on a condition (POSIX: pthread_cond_timedwait on a CLOCK_MONOTONIC condition) or an auto-reset
// event (Windows: WaitForSingleObject, whose timeouts exclude low-power time on Windows 8+) that
// arm/extend/tighten/expireNow signal, re-reads the deadline after every wake and waits at most
// kMaxSlice per slice, so a deadline change takes effect at once. Deadlines are kept in running time
// (RunningTime::fromSteady at the call). On expiry it calls faultDie(code) and nothing else: no log,
// no status, no report, no wait on anything. The thread asks for THREAD_PRIORITY_TIME_CRITICAL on
// Windows (nothing on Linux); a failure is ignored, only the kill latency depends on it.

#include "core/Exit.h"
#include "core/RunningTime.h"

#include <chrono>
#include <functional>
#include <memory>

namespace asb {

class DeadlineWatchdog final : public IDeadlineWatchdog {
public:
    static constexpr std::chrono::seconds kMaxSlice{1};

    // Test seams. Every member left empty takes the production default.
    struct Hooks {
        std::function<RunningTime::Duration()> clock;   // RunningTime::now
        std::function<void(int)> expire;                 // faultDie
        std::function<bool()> raisePriority;             // the platform's priority request
    };

    DeadlineWatchdog();
    explicit DeadlineWatchdog(Hooks hooks);
    // Stops and joins the thread. Production never destroys a watchdog (the process ends first).
    ~DeadlineWatchdog() override;
    DeadlineWatchdog(const DeadlineWatchdog&) = delete;
    DeadlineWatchdog& operator=(const DeadlineWatchdog&) = delete;

    void arm(std::chrono::steady_clock::time_point deadline, int code) override;
    void extend(std::chrono::steady_clock::time_point deadline) override;
    void tighten(std::chrono::steady_clock::time_point deadline, int code) override;
    void expireNow() override;

    // For tests: the state, whether the priority request succeeded, and the number of wakes.
    DeadlineState state() const;
    bool priorityRaised() const;
    long long wakeups() const;

    struct Impl;

private:
    RunningTime::Duration toRunning(std::chrono::steady_clock::time_point deadline) const;
    template <typename F>
    void update(F&& f);

    std::unique_ptr<Impl> d;
};

// The platform's priority request for the calling thread (Windows THREAD_PRIORITY_TIME_CRITICAL;
// POSIX: none, returns true).
bool raiseWatchdogThreadPriority();

}  // namespace asb
