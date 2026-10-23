#pragma once

// IBridgeEngine (WS-3 seam) over one PortBridge per enabled mapping: one failing port
// never stops the others (INV-5). Lives on the main thread.

#include "core/IBridgeEngine.h"
#include "engine/PortBridge.h"

#include <QObject>

#include <memory>
#include <vector>

namespace asb::engine {

class BridgeHost final : public QObject, public asb::IBridgeEngine {
    Q_OBJECT

public:
    // `side` must outlive the host. An empty factory is legal: every client is then refused with
    // "no serial device in this mode" (INV-1: only the service code paths register one).
    explicit BridgeHost(SerialDeviceFactory factory, asb::ISideChannel& side, EngineTimings timings = {},
                        QObject* parent = nullptr);
    ~BridgeHost() override;

    void start(const asb::BridgeConfig&) override;
    void stop(std::function<void()> done) override;
    // The enabled ports, in config order.
    QList<asb::PortStatus> snapshot() const override;

    const std::vector<std::unique_ptr<PortBridge>>& bridges() const { return m_bridges; }

private:
    SerialDeviceFactory m_factory;
    asb::ISideChannel& m_side;
    EngineTimings m_timings;
    std::vector<std::unique_ptr<PortBridge>> m_bridges;
    bool m_started = false;
    bool m_stopped = false;
};

}  // namespace asb::engine
