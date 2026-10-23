#pragma once

// The diagnostic run (`--run --config <scratch>`, master §4.10, D42; INV-1, INV-90, INV-101). Headless executable
// only: it is the second (and last) place that registers the serial device factory (INV-1; the first is --service).
// It takes neither engine.lock nor start.token, never touches a firewall, and runs a config the CLI already
// validated and made loopback-only (cli::ScratchRun::forceLoopback): so it can never stand in for the managed
// service nor benefit from its firewall rules. The status goes beside the scratch config, the log to
// the terminal.

#include "cli/Commands.h"
#include "core/IBridgeEngine.h"
#include "core/StatusStore.h"

#include <QHostAddress>
#include <QList>

#include <functional>
#include <memory>

namespace asb::engine {
class BridgeHost;
}

namespace asb::app {

class ScratchEngine {
public:
    explicit ScratchEngine(ISideChannel& side);
    ~ScratchEngine();

    void start(const BridgeConfig& loopbackConfig);
    void stop(std::function<void()> done);

    // What each enabled port's listener actually bound (QTcpServer::serverAddress / serverPort).
    struct Endpoint {
        QString name, device;
        bool listening = false;
        QHostAddress address;
        quint16 port = 0;
        QString error;
    };
    QList<Endpoint> endpoints() const;
    StatusReport report() const;

private:
    ISideChannel& m_side;
    std::unique_ptr<engine::BridgeHost> m_engine;
    QByteArray m_instanceId;
    QDateTime m_startedAt;
    QString m_configSha256;
};

// The whole run, until Ctrl+C (SIGINT/SIGTERM/SIGHUP; Windows console control events). Returns the exit code.
int runScratch(cli::Host& host, const QString& configFile, const BridgeConfig& loopbackConfig, cli::Console& console);

}  // namespace asb::app
