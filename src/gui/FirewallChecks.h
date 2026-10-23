#pragma once

// The window's own stale-rule checks (D53, S20; WS-10): each runs on a thread this object owns, with
// a deadline, and the window can wait for them before it starts an operation.
//
//  start(config, g, done)  runs `work` on a new thread with the deadline now + `budget`; `done` runs on the GUI thread
//                          (posted to this object's context), and never after this object is gone.
//  whenIdle(cb)            true when no check is running now; otherwise `cb` is posted to the GUI thread when the last
//                          running check ends (and never after this object is gone). The window defers every operation
//                          on it (submit), so the service is never started, stopped or reconfigured while the window's
//                          own firewall query is still inside WMI, and the GUI thread never waits for it (v1.1 INV-129).
//  waitIdle(max)           blocks the calling thread until no check is running, at most `max`: true when none is. Only
//                          the destructor uses it (never the GUI thread while the window is up).
//  ~FirewallChecks         waits for running checks (at most the budget and a margin), joins every finished thread;
//                          one still inside a native call past that is detached and posts nothing.
//
// `work` is the native comparator (Windows: a WbemSession of its own, every query bounded by the deadline, see
// WbemSession::setDeadline); tests give a function of their own.

#include "core/BridgeConfig.h"
#include "core/Interfaces.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace asb::gui {

class FirewallChecks {
public:
    using Verdict = std::pair<IFirewallMonitor::Check, QString>;
    using Work = std::function<Verdict(const BridgeConfig&, const FirewallGeneration&, Deadline)>;
    using Done = std::function<void(IFirewallMonitor::Check, const QString& why)>;

    FirewallChecks(Work work, std::chrono::milliseconds budget);
    ~FirewallChecks();
    FirewallChecks(const FirewallChecks&) = delete;
    FirewallChecks& operator=(const FirewallChecks&) = delete;

    bool start(const BridgeConfig&, const FirewallGeneration&, Done done);
    bool whenIdle(std::function<void()> cb);
    bool waitIdle(std::chrono::milliseconds max);
    int running() const;
    std::chrono::milliseconds budget() const { return m_budget; }

    // Test builds: a line when a check starts and ends ("start" / "end"), from the thread that saw it.
    std::function<void(const char*)> trace;

private:
    struct Shared {
        std::mutex m;
        std::condition_variable cv;
        int running = 0;
        bool alive = true;
        std::vector<std::function<void()>> whenIdle;   // posted to the GUI thread when `running` reaches 0
    };
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    void reap();

    Work m_work;
    std::chrono::milliseconds m_budget;
    std::shared_ptr<Shared> m_shared;
    QObject m_context;   // the GUI thread's receiver of every verdict; destroyed with this object
    std::vector<Worker> m_workers;
};

}  // namespace asb::gui
