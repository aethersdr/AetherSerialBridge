#pragma once

// The window's OS reads that the core seams do not carry (WS-10). All read-only; none opens a device.
//
//  readAutoStart   the service's real start type (INV-23). Windows: QueryServiceConfigW (SERVICE_QUERY_CONFIG, no
//                  administrator needed) -> SERVICE_AUTO_START. Linux: the unit's UnitFileState over D-Bus ("enabled"
//                  and not masked). nullopt when it cannot be read; false when the service does not exist.
//  serialPorts     device names for the combo (INV-32: names only). Windows: HKLM\HARDWARE\DEVICEMAP\SERIALCOMM.
//                  Linux: /dev/serial/by-id links and the kernel tty names (WS-8
//                  SerialPortList).
//  loadedModules   the files of every module mapped into this process (the --self-test-init report, INV-88).
//  firewallCheckWork  Windows: the service's own comparator (D53, S20: the dying engine writes no reason, so whoever
//                  looks re-runs it) against the reserved group in the ActiveStore, for the committed config and the
//                  token's generation, with a WMI session of its own and every query bounded by the check's deadline. It
//                  runs on a FirewallChecks thread (FirewallChecks.h). Linux: none (no managed firewall).

#include "core/BridgeConfig.h"
#include "core/Interfaces.h"
#include "gui/FirewallChecks.h"

#include <QStringList>

#include <functional>
#include <optional>

namespace asb::gui {

std::optional<bool> readAutoStart();
QStringList serialPorts();
QStringList loadedModules();

// The comparator a FirewallChecks runs (on its own thread, by a deadline); empty when this OS has no firewall check.
FirewallChecks::Work firewallCheckWork(const QString& installedExe);

}  // namespace asb::gui
