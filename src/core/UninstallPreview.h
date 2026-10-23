#pragma once

// What an uninstall will take down, said before it runs (WS-16, INV-125): the ports that stop, the clients connected
// to them, where the settings are and whether that folder is trusted, and an installed build newer than this one.
// Pure: the window (WS-17), --uninstall and --uninstall-from-apps word it from what a ReadOnly view already read.

#include "core/BridgeConfig.h"
#include "core/Interfaces.h"
#include "core/StatusStore.h"
#include "core/TrustedConfig.h"

#include <QString>
#include <QStringList>

#include <optional>

namespace asb::core {

struct UninstallPreview {
    QStringList portsGoingDown;      // "port 4001 (Amp)", for each enabled port of the committed config
    QStringList connectedClients;    // "the client at 192.0.2.10:50000 on port 4001 (Amp)" (the endpoint the bridge reports)
    QString settingsFolder;
    bool settingsTrusted = false;    // the folder exists and the trust check passed (only then can settings be removed)
    bool installed = false;          // a service is installed (Current or IncompleteInstall)
    std::optional<QString> installedVersion;   // a different installed build's version, when known (wording only)
    // One line per port, as --uninstall prints it before it runs:
    // "Stopping port 4001 (Amp): the client at 192.0.2.10:50000 is connected and will be disconnected."
    QStringList lines;
};

// `committed`: the committed config (empty when there is none); `status`: the last status report (nullopt when none);
// `folder`: the ReadOnly open of the data folder. The installed build's version comes from
// `installation.installedVersion` (its build stamp, WS-14), kept only when it is another build or an unfinished install.
UninstallPreview uninstallPreview(const BridgeConfig& committed, const std::optional<StatusReport>& status,
                                  const Installation& installation, const OpenedFolder& folder, const QString& settingsFolder);

}  // namespace asb::core
