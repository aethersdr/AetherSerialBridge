#include "platform/posix/SystemdUnit.h"

#include "core/InstallationEvidence.h"

#include <stdexcept>

// ASB_REPO (owner/name) comes from the top-level CMakeLists.txt, which checks its shape.
#ifndef ASB_REPO
#error "ASB_REPO is not defined: it is set in the top-level CMakeLists.txt"
#endif

namespace asb::posix::SystemdUnit {

QByteArray render(const QString& executable)
{
    bool ok = executable.startsWith(QLatin1Char('/')) && !executable.contains(QLatin1Char('%'));
    for (const QChar c : executable)
        if (c.isSpace()) ok = false;
    if (!ok)
        throw std::invalid_argument(
            QStringLiteral("'%1' cannot be used in ExecStart (absolute path, no spaces or %).").arg(executable).toStdString());

    const QByteArray exe = executable.toUtf8();
    QByteArray u;
    u += InstallationEvidence::kQtUnitHeader;
    u += ". Re-run --install to regenerate it;\n"
         "# local changes belong in a drop-in (systemctl edit aether-serial-bridge).\n"
         "[Unit]\n"
         "Description=Aether Serial Bridge (serial ports on TCP for AetherSDR)\n"
         "Documentation=https://github.com/" ASB_REPO "\n"
         "After=network-online.target\n"
         "Wants=network-online.target\n"
         "\n"
         "[Service]\n"
         "Type=simple\n"
         "ExecStart=" + exe + " --service\n"
         "Restart=on-failure\n"
         "RestartSec=5\n"
         "# Exit 6 = not activated (stopped by Stop, or an apply did not finish): it stays\n"
         "# stopped until Start or Save & apply, so it is not a failure to restart.\n"
         "SuccessExitStatus=6\n"
         "\n"
         "# A throwaway system user that exists only while the service runs. It can\n"
         "# open serial ports through the dialout group and nothing else of note.\n"
         "DynamicUser=yes\n"
         "SupplementaryGroups=dialout\n"
         "\n"
         "# /etc/aether-serial-bridge (root-owned, read-only here) and\n"
         "# /run/aether-serial-bridge for status.json. The engine lock in\n"
         "# /var/lib/aether-serial-bridge is root-owned and only read (flock needs no write).\n"
         "ConfigurationDirectory=aether-serial-bridge\n"
         "RuntimeDirectory=aether-serial-bridge\n"
         "\n"
         "NoNewPrivileges=yes\n"
         "ProtectSystem=strict\n"
         "ProtectHome=yes\n"
         "PrivateTmp=yes\n"
         "\n"
         "# Device access is left open on purpose. DevicePolicy=closed with\n"
         "# DeviceAllow=char-ttyUSB/char-ttyACM/char-ttyS rw has not been proven against\n"
         "# real adapters yet, and a wrong policy fails as \"permission denied\" on the\n"
         "# port, which looks exactly like a missing dialout group. File permissions\n"
         "# (root:dialout 0660) still decide who opens a port.\n"
         "\n"
         "[Install]\n"
         "WantedBy=multi-user.target\n";
    return u;
}

}  // namespace asb::posix::SystemdUnit
