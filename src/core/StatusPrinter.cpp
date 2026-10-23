#include "core/StatusPrinter.h"

#include <algorithm>

namespace asb::StatusPrinter {

QString format(const StatusReport* report)
{
    if (!report) return QStringLiteral("No status reported yet.\n");

    QString o;
    o += QStringLiteral("Reported: %1 (pid %2)\n")
             .arg(report->updated.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
             .arg(report->pid);
    if (report->configError) o += QStringLiteral("Config error: %1\n").arg(*report->configError);

    // Windows COM names fit the classic 6 columns; Linux device paths widen it.
    qsizetype com = 6;
    for (const PortStatus& p : report->ports) com = std::max(com, p.comPort.size());

    for (const PortStatus& p : report->ports) {
        QString detail;
        if (p.state == BridgeState::Connected) detail = p.client.value_or(QString());
        else if (p.state == BridgeState::Error) detail = p.lastError.value_or(QString());
        // "  <name, 16 wide> <device, padded> <bind>:<tcp, 5 wide>  <state, 9 wide> in=.. out=.. <detail>"
        o += QStringLiteral("  %1 %2 %3:%4  %5 in=%6 out=%7 %8\n")
                 .arg(p.name.leftJustified(16), p.comPort.leftJustified(com), p.bindAddress,
                      QString::number(p.tcpPort).leftJustified(5), enumName(p.state).leftJustified(9),
                      QString::number(p.bytesToSerial), QString::number(p.bytesToClient), detail);
    }
    return o;
}

}  // namespace asb::StatusPrinter
