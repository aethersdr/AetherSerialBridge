#include "gui/FirewallChecks.h"

#include <QMetaObject>

namespace asb::gui {

FirewallChecks::FirewallChecks(Work work, std::chrono::milliseconds budget)
    : m_work(std::move(work)), m_budget(budget), m_shared(std::make_shared<Shared>())
{
}

FirewallChecks::~FirewallChecks()
{
    {
        std::lock_guard g(m_shared->m);
        m_shared->alive = false;   // from here no thread posts to m_context
    }
    waitIdle(m_budget + std::chrono::seconds(1));
    for (Worker& w : m_workers) {
        if (!w.thread.joinable()) continue;
        if (w.finished->load()) w.thread.join();
        else w.thread.detach();   // still inside a native call past its deadline: it posts nothing (alive is false)
    }
}

void FirewallChecks::reap()
{
    for (auto it = m_workers.begin(); it != m_workers.end();) {
        if (it->finished->load()) {
            if (it->thread.joinable()) it->thread.join();
            it = m_workers.erase(it);
        } else {
            ++it;
        }
    }
}

bool FirewallChecks::start(const BridgeConfig& config, const FirewallGeneration& g, Done done)
{
    if (!m_work) return false;
    reap();
    const Deadline deadline = std::chrono::steady_clock::now() + m_budget;
    {
        std::lock_guard lk(m_shared->m);
        ++m_shared->running;
    }
    auto finished = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<Shared> shared = m_shared;
    QObject* context = &m_context;
    Work work = m_work;
    std::function<void(const char*)> traceLine = trace;
    std::thread t([shared, context, work, traceLine, config, g, deadline, done = std::move(done), finished] {
        if (traceLine) traceLine("start");
        Verdict v{IFirewallMonitor::Check::Failed, QString()};
        try {
            v = work(config, g, deadline);
        } catch (const std::exception& e) {
            v = {IFirewallMonitor::Check::Failed, QString::fromLocal8Bit(e.what())};
        } catch (...) {
            v = {IFirewallMonitor::Check::Failed, QStringLiteral("unknown failure")};
        }
        if (traceLine) traceLine("end");
        {
            std::lock_guard lk(shared->m);
            --shared->running;
            // Posted under the lock: the owner clears `alive` under the same lock before its context goes, and a
            // posted event to an object that is then deleted is discarded by Qt.
            if (shared->alive) {
                const IFirewallMonitor::Check check = v.first;
                const QString why = v.second;
                QMetaObject::invokeMethod(context, [done, check, why] { done(check, why); }, Qt::QueuedConnection);
                // The last running check ended: the operations deferred on it start now, on the GUI thread (INV-129).
                if (shared->running == 0) {
                    for (std::function<void()>& cb : shared->whenIdle)
                        QMetaObject::invokeMethod(context, std::move(cb), Qt::QueuedConnection);
                    shared->whenIdle.clear();
                }
            }
            finished->store(true);   // nothing of the owner is touched after this: joinable at once
        }
        shared->cv.notify_all();
    });
    m_workers.push_back(Worker{std::move(t), finished});
    return true;
}

bool FirewallChecks::whenIdle(std::function<void()> cb)
{
    std::lock_guard lk(m_shared->m);
    if (m_shared->running == 0) return true;
    if (cb) m_shared->whenIdle.push_back(std::move(cb));
    return false;
}

bool FirewallChecks::waitIdle(std::chrono::milliseconds max)
{
    std::unique_lock lk(m_shared->m);
    return m_shared->cv.wait_for(lk, max, [&] { return m_shared->running == 0; });
}

int FirewallChecks::running() const
{
    std::lock_guard lk(m_shared->m);
    return m_shared->running;
}

}  // namespace asb::gui
