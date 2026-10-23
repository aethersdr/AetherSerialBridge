#pragma once

// The headless executable's Host (WS-9): this OS's SystemHost plus what only this executable contains — the engine
// for `--run --config <scratch>`, the service hosts (WS-7 Windows, WS-8 Linux), the Windows port list
// (QSerialPortInfo, never opening a port) — and, in a test build, the guard probe and the systemd suite's seams.

#include "cli/SystemHost.h"

#include <memory>

namespace asb::app {

class HeadlessHost final : public cli::SystemHost {
public:
    HeadlessHost(int argc, char** argv);
    ~HeadlessHost() override;

    int runScratch(const QString& configFile, const BridgeConfig& loopbackConfig, cli::Console&) override;
#ifdef __linux__
    // Windows: main() runs the WS-7 service host directly (it never returns); the base class refuses here.
    int runService() override;
#endif
#ifdef _WIN32
    int listPorts(cli::Console&) override;
#endif
#ifdef ASB_TEST_SEAMS
    void guardProbe() override;
#ifdef __linux__
    void resultHook(const Result&, const QStringList& reports) override;

protected:
    void customise(posix::LinuxPlatform::Options&) override;
    IFirewall* wrapFirewall(IFirewall&) override;

private:
    std::unique_ptr<IFirewall> m_firewall;
#endif
#endif
};

}  // namespace asb::app
