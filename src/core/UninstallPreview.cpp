#include "core/UninstallPreview.h"

namespace asb::core {
namespace {

QString portName(const PortMapping& p)
{
    return p.name.isEmpty() ? QStringLiteral("port %1").arg(p.tcpPort) : QStringLiteral("port %1 (%2)").arg(p.tcpPort).arg(p.name);
}

}  // namespace

UninstallPreview uninstallPreview(const BridgeConfig& committed, const std::optional<StatusReport>& status,
                                  const Installation& installation, const OpenedFolder& folder, const QString& settingsFolder)
{
    UninstallPreview p;
    p.settingsFolder = settingsFolder;
    p.settingsTrusted = folder.usable;
    p.installed = installation.state == InstallState::Current || installation.state == InstallState::IncompleteInstall;
    if (installation.otherBuild || installation.state == InstallState::IncompleteInstall) p.installedVersion = installation.installedVersion;
    if (!p.installed) return p;
    for (const PortMapping& port : committed.ports) {
        if (!port.enabled) continue;
        const QString name = portName(port);
        p.portsGoingDown << name;
        std::optional<QString> client;
        if (status)
            for (const PortStatus& s : status->ports)
                if (s.tcpPort == port.tcpPort && s.state == BridgeState::Connected && s.client && !s.client->isEmpty())
                    client = *s.client;
        if (client) {
            p.connectedClients << QStringLiteral("the client at %1 on %2").arg(*client, name);
            p.lines << QStringLiteral("Stopping %1: the client at %2 is connected and will be disconnected.").arg(name, *client);
        } else {
            p.lines << QStringLiteral("Stopping %1: no client is connected.").arg(name);
        }
    }
    return p;
}

}  // namespace asb::core
