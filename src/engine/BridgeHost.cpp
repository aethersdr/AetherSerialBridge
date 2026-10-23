#include "engine/BridgeHost.h"

#include <QTimer>

namespace asb::engine {

BridgeHost::BridgeHost(SerialDeviceFactory factory, asb::ISideChannel& side, EngineTimings timings, QObject* parent)
    : QObject(parent), m_factory(std::move(factory)), m_side(side), m_timings(timings)
{
}

BridgeHost::~BridgeHost() = default;

void BridgeHost::start(const asb::BridgeConfig& config)
{
    if (m_started) return;
    m_started = true;
    for (const asb::PortMapping& p : config.ports) {
        if (!p.enabled) continue;
        m_bridges.push_back(std::make_unique<PortBridge>(p, m_factory, m_side, m_timings));
    }
    for (auto& b : m_bridges) b->start();
}

void BridgeHost::stop(std::function<void()> done)
{
    if (!m_stopped) {
        m_stopped = true;
        for (auto& b : m_bridges) b->stop();
    }
    // `done` runs from the event loop, once per call.
    if (done) QTimer::singleShot(0, this, std::move(done));
}

QList<asb::PortStatus> BridgeHost::snapshot() const
{
    QList<asb::PortStatus> out;
    out.reserve(qsizetype(m_bridges.size()));
    for (const auto& b : m_bridges) out.append(b->snapshot());
    return out;
}

}  // namespace asb::engine
