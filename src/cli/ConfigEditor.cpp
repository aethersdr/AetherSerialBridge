#include "cli/ConfigEditor.h"

#include <QRegularExpression>

namespace asb::cli::ConfigEditor {
namespace {

std::optional<qsizetype> find(const BridgeConfig& c, const QString& name)
{
    for (qsizetype i = 0; i < c.ports.size(); ++i)
        if (equalsOrdinalIgnoreCase(c.ports[i].name, name)) return i;
    return std::nullopt;
}

char parityLetter(Parity p)
{
    switch (p) {
    case Parity::Even: return 'E';
    case Parity::Odd: return 'O';
    case Parity::Mark: return 'M';
    case Parity::Space: return 'S';
    default: return 'N';
    }
}

QString stopBitsText(StopBits s)
{
    switch (s) {
    case StopBits::Two: return QStringLiteral("2");
    case StopBits::OnePointFive: return QStringLiteral("1.5");
    default: return QStringLiteral("1");
    }
}

[[noreturn]] void refuse(const QString& why)
{
    throw EditRefused(why.toStdString());
}

}  // namespace

Format parseFormat(const QString& text)
{
    static const QRegularExpression re(QStringLiteral("^([5-8])([NEOMS])(1|1\\.5|2)$"),
                                       QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(text);
    // The pattern is ASCII, but CaseInsensitiveOption is Unicode-aware: refuse anything non-ASCII outright.
    bool ascii = true;
    for (const QChar c : text)
        if (c.unicode() > 0x7f) ascii = false;
    if (!m.hasMatch() || !ascii)
        refuse(QStringLiteral("Format '%1' should look like 8N1 (data bits 5-8, parity N/E/O/M/S, stop bits 1, 1.5 or 2)")
                   .arg(text));
    Format f;
    f.dataBits = m.captured(1).toInt();
    switch (m.captured(2).toUpper().at(0).unicode()) {
    case 'E': f.parity = Parity::Even; break;
    case 'O': f.parity = Parity::Odd; break;
    case 'M': f.parity = Parity::Mark; break;
    case 'S': f.parity = Parity::Space; break;
    default: f.parity = Parity::None; break;
    }
    const QString stop = m.captured(3);
    f.stopBits = stop == QLatin1String("2") ? StopBits::Two
                 : stop == QLatin1String("1.5") ? StopBits::OnePointFive
                                                : StopBits::One;
    return f;
}

QString formatText(const PortMapping& p)
{
    return QString::number(p.dataBits) + QLatin1Char(parityLetter(p.parity)) + stopBitsText(p.stopBits);
}

void requireValid(const BridgeConfig& c)
{
    const QStringList errors = c.validate();
    if (!errors.isEmpty()) refuse(QStringLiteral("Not saved:\n  ") + errors.join(QStringLiteral("\n  ")));
}

void addPort(BridgeConfig& c, const AddPortArgs& a, QStringList* said)
{
    if (a.name.trimmed().isEmpty()) refuse(QStringLiteral("A port needs a name, e.g. KAT500"));
    if (find(c, a.name))
        refuse(QStringLiteral("A port named '%1' already exists; remove it first or pick another name").arg(a.name));
    const Format f = parseFormat(a.format);
    PortMapping p;
    p.name = a.name;
    p.comPort = a.device;
    p.tcpPort = a.tcpPort;
    p.baudRate = a.baud;
    p.dataBits = f.dataBits;
    p.parity = f.parity;
    p.stopBits = f.stopBits;
    p.bindAddress = a.bind.value_or(QString::fromLatin1(kAllInterfaces));
    p.allowedClients = a.allow;
    BridgeConfig next = c;
    next.ports.append(p);
    requireValid(next);
    c = next;
    if (!said) return;
    *said << QStringLiteral("Added %1: %2 at %3 %4 on %5:%6.")
                 .arg(p.name, p.comPort)
                 .arg(p.baudRate)
                 .arg(a.format.toUpper(), p.bindAddress)
                 .arg(p.tcpPort);
    // By value, not by whether --bind was given: an explicit 0.0.0.0, ::, or any other spelling of them listens
    // on every interface too (WS-12 round 1 class audit).
    if (listensOnEveryInterface(p.bindAddress))
        *said << QStringLiteral("Warning: listening on every interface (%1). Use --bind <LAN or VPN address> to limit it, "
                                "and firewall the port. Never forward it from the internet.")
                     .arg(p.bindAddress);
    *said << deviceNotes(p.comPort);
}

QStringList deviceNotes(const QString& device)
{
    static const QRegularExpression renumbered(QStringLiteral("^/dev/tty(USB|ACM)\\d+$"));
    QStringList notes;
    if (renumbered.match(device).hasMatch())
        notes << QStringLiteral("Note: %1 can change number after a reboot or replug. Prefer its /dev/serial/by-id/ name "
                                "(see --list-ports).")
                     .arg(device);
    return notes;
}

void removePort(BridgeConfig& c, const QString& name, QStringList* said)
{
    const std::optional<qsizetype> i = find(c, name);
    if (!i) refuse(QStringLiteral("No port named '%1'. See --show-config").arg(name));
    const QString shown = c.ports[*i].name;
    BridgeConfig next = c;
    next.ports.removeAt(*i);
    requireValid(next);
    c = next;
    if (said) *said << QStringLiteral("Removed %1.").arg(shown);
}

void setEnabled(BridgeConfig& c, const QString& name, bool enabled, QStringList* said)
{
    const std::optional<qsizetype> i = find(c, name);
    if (!i) refuse(QStringLiteral("No port named '%1'. See --show-config").arg(name));
    BridgeConfig next = c;
    next.ports[*i].enabled = enabled;
    requireValid(next);
    c = next;
    if (said)
        *said << QStringLiteral("%1 is now %2.").arg(c.ports[*i].name, enabled ? QStringLiteral("enabled") : QStringLiteral("disabled"));
}

QStringList describe(const BridgeConfig& c, const QString& program)
{
    QStringList lines;
    if (c.ports.isEmpty())
        lines << QStringLiteral("No ports yet. Add one with: %1 --add-port <name> <device> <tcp-port>").arg(program);
    for (const PortMapping& p : c.ports) {
        const QString allow = p.allowedClients.isEmpty() ? QString() : QStringLiteral("  allow=%1").arg(p.allowedClients);
        // "  on   <name, 16 wide> <device>  <baud> <format>  <bind>:<tcp>[  allow=...]"
        lines << QStringLiteral("  %1  %2 %3  %4 %5  %6:%7%8")
                     .arg(p.enabled ? QStringLiteral("on ") : QStringLiteral("off"), p.name.leftJustified(16), p.comPort)
                     .arg(p.baudRate)
                     .arg(formatText(p), p.bindAddress)
                     .arg(p.tcpPort)
                     .arg(allow);
    }
    for (const QString& e : c.validate()) lines << QStringLiteral("Problem: %1").arg(e);
    return lines;
}

}  // namespace asb::cli::ConfigEditor
