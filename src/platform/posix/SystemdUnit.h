#pragma once

// The systemd unit --install writes (D21, D37, master §4.8). Rendered from code so tests can check every line:
//  - the first line is the Qt header (InstallationEvidence::kQtUnitHeader), so detection can tell this
//    program's unit from any other;
//  - ExecStart runs `--service` (D37): the managed config runs only under the service manager;
//  - no CacheDirectory= (a native binary unpacks nothing);
//  - never StateDirectory=: with DynamicUser=yes it would move /var/lib/aether-serial-bridge under
//    /var/lib/private and chown it to the dynamic user, which breaks the root-owned engine lock (D54);
//  - SuccessExitStatus=6 (v1.1 station fix): a start without a valid start token (Stop revoked it and kept the
//    unit enabled) exits 6, which Restart=on-failure would otherwise restart every RestartSec forever.
// The unit lives in /usr/local/lib/systemd/system (D21); /etc/systemd/system/<unit> is where systemd keeps
// the mask link, the persistent fail-closed state (INV-48, INV-79).

#include <QByteArray>
#include <QString>

namespace asb::posix::SystemdUnit {

inline constexpr char kUnitName[] = "aether-serial-bridge.service";
inline constexpr char kUnitDirectory[] = "/usr/local/lib/systemd/system";
inline constexpr char kEtcDirectory[] = "/etc/systemd/system";
inline constexpr char kInstalledBinary[] = "/usr/local/bin/aether-serial-bridge";

// The unit text for `executable` (an absolute path with no white space and no '%'). Throws
// std::invalid_argument otherwise.
QByteArray render(const QString& executable = QString::fromLatin1(kInstalledBinary));

}  // namespace asb::posix::SystemdUnit
