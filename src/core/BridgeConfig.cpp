#include "core/BridgeConfig.h"

#include "core/AllowedClients.h"

#include <QHostAddress>
#include <QRegularExpression>

#include <algorithm>

namespace asb {
namespace {

// Blank: only characters of the Unicode White_Space set, which is what QChar::isSpace tests (Zs/Zl/Zp, U+0009–U+000D, U+0085, U+00A0).
bool isNullOrWhiteSpace(const QString& s)
{
    return std::all_of(s.cbegin(), s.cend(), [](QChar c) { return c.isSpace(); });
}

// The classes below admit BMP characters only: a character outside the BMP fails every one of them.
// PCRE2 matches whole code points, so surrogates are refused first.
bool hasSurrogate(const QString& s)
{
    return std::any_of(s.cbegin(), s.cend(), [](QChar c) { return c.isSurrogate(); });
}

// A word character is [\p{L}\p{Mn}\p{Nd}\p{Pc}] and a digit is \p{Nd} (Unicode, not ECMAScript). Spelled
// out rather than relying on PCRE2's \w, which adds \p{N} (No/Nl such as '²'). `$` matches at the end or
// before one final newline (PCRE2_DOLLAR_ENDONLY is not set).
const QRegularExpression& namePattern()
{
    static const QRegularExpression re(QStringLiteral(R"(^[\p{L}\p{Mn}\p{Nd}\p{Pc} .()\-]{0,40}$)"),
                                       QRegularExpression::UseUnicodePropertiesOption);
    return re;
}

const QRegularExpression& comPattern()
{
    static const QRegularExpression re(QStringLiteral(R"(^(COM\p{Nd}{1,3}|/dev/[\p{L}\p{Mn}\p{Nd}\p{Pc}./\-]+)$)"),
                                       QRegularExpression::UseUnicodePropertiesOption |
                                           QRegularExpression::CaseInsensitiveOption);
    return re;
}

bool matches(const QRegularExpression& re, const QString& s)
{
    return !hasSurrogate(s) && re.match(s).hasMatch();
}

std::optional<quint8> parseIpv4Group(QStringView g)
{
    // 1–3 digits, <= 255 as written in decimal, then read with a leading 0 selecting octal, where 8 and 9
    // are invalid.
    if (g.isEmpty() || g.size() > 3) return std::nullopt;
    int decimal = 0;
    for (QChar c : g) {
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) return std::nullopt;
        decimal = decimal * 10 + (c.unicode() - '0');
    }
    if (decimal > 255) return std::nullopt;
    if (g.size() > 1 && g.front() == QLatin1Char('0')) {
        int octal = 0;
        for (QChar c : g) {
            if (c > QLatin1Char('7')) return std::nullopt;
            octal = octal * 8 + (c.unicode() - '0');
        }
        return quint8(octal);
    }
    return quint8(decimal);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------------------------
namespace {
template <typename E, size_t N>
QString nameOf(E v, const std::array<const char*, N>& names)
{
    const int i = int(v);
    return (i >= 0 && size_t(i) < N) ? QString::fromLatin1(names[size_t(i)]) : QString();
}
template <typename E, size_t N>
std::optional<E> fromName(const QString& s, const std::array<const char*, N>& names)
{
    for (size_t i = 0; i < N; ++i)
        if (equalsOrdinalIgnoreCase(s, QString::fromLatin1(names[i]))) return E(int(i));
    return std::nullopt;
}
constexpr std::array<const char*, 5> kParity = {"None", "Odd", "Even", "Mark", "Space"};
constexpr std::array<const char*, 4> kStopBits = {"None", "One", "Two", "OnePointFive"};
constexpr std::array<const char*, 4> kHandshake = {"None", "XOnXOff", "RequestToSend", "RequestToSendXOnXOff"};
}  // namespace

bool equalsOrdinalIgnoreCase(const QString& a, const QString& b)
{
    if (a.size() != b.size()) return false;
    const auto asciiLower = [](char16_t c) { return c >= u'A' && c <= u'Z' ? char16_t(c + 32) : c; };
    for (qsizetype i = 0; i < a.size(); ++i) {
        const char16_t x = a.at(i).unicode(), y = b.at(i).unicode();
        if (x == y) continue;
        if ((x < 0x80) != (y < 0x80)) return false;   // never maps non-ASCII onto ASCII
        if (x < 0x80 ? asciiLower(x) != asciiLower(y) : QChar::toUpper(char32_t(x)) != QChar::toUpper(char32_t(y)))
            return false;
    }
    return true;
}

QString enumName(Parity v) { return nameOf(v, kParity); }
QString enumName(StopBits v) { return nameOf(v, kStopBits); }
QString enumName(Handshake v) { return nameOf(v, kHandshake); }
bool isDefined(Parity v) { return !enumName(v).isEmpty(); }
bool isDefined(StopBits v) { return !enumName(v).isEmpty(); }
bool isDefined(Handshake v) { return !enumName(v).isEmpty(); }
std::optional<Parity> parityFromName(const QString& s) { return fromName<Parity>(s, kParity); }
std::optional<StopBits> stopBitsFromName(const QString& s) { return fromName<StopBits>(s, kStopBits); }
std::optional<Handshake> handshakeFromName(const QString& s) { return fromName<Handshake>(s, kHandshake); }

// ---------------------------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------------------------
std::optional<IpAddress> parseIpAddress(const QString& s)
{
    IpAddress out;
    if (s.contains(QLatin1Char(':'))) {
        // QHostAddress trims white space and is lenient about some forms; refuse
        // anything but address characters and an optional %scope before handing it over.
        static const QRegularExpression v6(QStringLiteral(R"(^[0-9A-Fa-f:.]+(%[0-9A-Za-z_.\-]+)?\z)"));
        if (!v6.match(s).hasMatch()) return std::nullopt;
        QHostAddress a;
        if (!a.setAddress(s) || a.protocol() != QAbstractSocket::IPv6Protocol) return std::nullopt;
        const Q_IPV6ADDR raw = a.toIPv6Address();
        out.family = IpAddress::Family::V6;
        std::copy(std::begin(raw.c), std::end(raw.c), out.bytes.begin());
        out.scopeId = a.scopeId();
        return out;
    }
    const QList<QStringView> groups = QStringView(s).split(QLatin1Char('.'));
    if (groups.size() != 4) return std::nullopt;
    out.family = IpAddress::Family::V4;
    for (int i = 0; i < 4; ++i) {
        const auto g = parseIpv4Group(groups[i]);
        if (!g) return std::nullopt;
        out.bytes[size_t(i)] = *g;
    }
    return out;
}

bool isUnspecified(const IpAddress& a)
{
    if (a.family == IpAddress::Family::V4) {
        for (int i = 0; i < 4; ++i)
            if (a.bytes[size_t(i)] != 0) return false;
        return true;
    }
    // :: itself, or ::ffff:0.0.0.0 (an IPv4 wildcard on a dual-stack socket).
    for (int i = 0; i < 10; ++i)
        if (a.bytes[size_t(i)] != 0) return false;
    const bool mapped = a.bytes[10] == 0xff && a.bytes[11] == 0xff;
    if (!mapped && (a.bytes[10] != 0 || a.bytes[11] != 0)) return false;
    for (int i = 12; i < 16; ++i)
        if (a.bytes[size_t(i)] != 0) return false;
    return true;
}

bool listensOnEveryInterface(const QString& bindAddress)
{
    const std::optional<IpAddress> ip = parseIpAddress(bindAddress);
    return ip && isUnspecified(*ip);
}

QString toString(const IpAddress& a)
{
    if (a.family == IpAddress::Family::V4)
        return QStringLiteral("%1.%2.%3.%4").arg(a.bytes[0]).arg(a.bytes[1]).arg(a.bytes[2]).arg(a.bytes[3]);
    Q_IPV6ADDR raw;
    std::copy(a.bytes.begin(), a.bytes.end(), std::begin(raw.c));
    QHostAddress h(raw);
    if (!a.scopeId.isEmpty()) h.setScopeId(a.scopeId);
    return h.toString();
}

// ---------------------------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------------------------
QString PortMapping::displayName() const
{
    return isNullOrWhiteSpace(name) ? comPort : name;
}

QStringList BridgeConfig::validate() const
{
    QStringList errors;
    QList<std::pair<QString, QString>> comSeen;   // compared with equalsOrdinalIgnoreCase
    QList<std::pair<int, QString>> tcpSeen;

    for (const PortMapping& p : ports) {
        const QString shown = p.displayName();
        const QString who = isNullOrWhiteSpace(shown) ? QStringLiteral("(unnamed port)") : shown;
        const auto err = [&](const QString& text) { errors.append(who + QStringLiteral(": ") + text); };

        if (!matches(namePattern(), p.name))
            err(QStringLiteral("name may use letters, digits, spaces and . ( ) - only, up to 40 characters."));

        // One wording for both OSes' paths: COM names and /dev/serial/by-id paths.
        const auto com = std::find_if(comSeen.cbegin(), comSeen.cend(), [&](const auto& e) {
            return equalsOrdinalIgnoreCase(e.first, p.comPort);
        });
        if (isNullOrWhiteSpace(p.comPort) || !matches(comPattern(), p.comPort))
            err(QStringLiteral("'%1' is not a serial port name (expected COM1, COM2, ... or a /dev/serial/by-id/... "
                               "path).")
                    .arg(p.comPort));
        else if (com != comSeen.cend())
            err(QStringLiteral("%1 is already used by %2.").arg(p.comPort, com->second));
        else
            comSeen.append({p.comPort, who});

        const auto tcp = std::find_if(tcpSeen.cbegin(), tcpSeen.cend(), [&](const auto& e) { return e.first == p.tcpPort; });
        if (p.tcpPort < 1 || p.tcpPort > 65535)
            err(QStringLiteral("TCP port must be 1-65535."));
        else if (tcp != tcpSeen.cend())
            err(QStringLiteral("TCP port %1 is already used by %2.").arg(p.tcpPort).arg(tcp->second));
        else
            tcpSeen.append({p.tcpPort, who});

        // Shorthand ("10", "1.2.3") is refused (parseIpAddress).
        if (!parseIpAddress(p.bindAddress))
            err(QStringLiteral("bind address '%1' is not an IP address.").arg(p.bindAddress));
        if (p.baudRate < 50 || p.baudRate > 4'000'000)
            err(QStringLiteral("baud rate %1 is out of range.").arg(p.baudRate));
        if (p.dataBits < 5 || p.dataBits > 8)
            err(QStringLiteral("data bits must be 5-8."));
        if (p.stopBits == StopBits::None || !isDefined(p.stopBits))
            err(QStringLiteral("stop bits must be One, OnePointFive or Two."));
        if (!isDefined(p.parity))
            err(QStringLiteral("unknown parity."));
        if (!isDefined(p.handshake))
            err(QStringLiteral("unknown handshake."));
        // Qt has no combined RTS/CTS + XON/XOFF flow control.
        else if (p.handshake == Handshake::RequestToSendXOnXOff)
            err(QStringLiteral("handshake RequestToSendXOnXOff (RTS/CTS and XON/XOFF together) is not supported; "
                               "use RequestToSend or XOnXOff."));
        // Characters first (the quoting fence), then each token's meaning, so nothing the firewall
        // would reject reaches a firewall change. Firewall keywords are checked in AllowedClients.
        if (!AllowedClients::passesCharacterFence(p.allowedClients))
            err(QStringLiteral("allowed clients may only list IP addresses, ranges or LocalSubnet."));
        else
            for (const QString& e : AllowedClients::check(p.allowedClients)) err(e);
    }
    return errors;
}

}  // namespace asb
