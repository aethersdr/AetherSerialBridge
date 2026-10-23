#pragma once

// The bridge configuration model (master §4.2, WS-3): fields, defaults and validation messages, in a fixed
// order, with three deliberate refusals (shorthand bind addresses, RequestToSendXOnXOff, firewall
// keywords the Windows provider does not accept).

#include <QList>
#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>
#include <optional>

namespace asb {

// The enum values are fixed numbers, because a config may carry them as integers. A value outside the
// named ones is representable on purpose: it loads, and validate() reports it ("unknown parity.").
enum class Parity : int { None = 0, Odd = 1, Even = 2, Mark = 3, Space = 4 };
enum class StopBits : int { None = 0, One = 1, Two = 2, OnePointFive = 3 };
enum class Handshake : int { None = 0, XOnXOff = 1, RequestToSend = 2, RequestToSendXOnXOff = 3 };

// Name <-> value. `name` returns an empty string for a value outside the named ones.
QString enumName(Parity);
QString enumName(StopBits);
QString enumName(Handshake);
bool isDefined(Parity);
bool isDefined(StopBits);
bool isDefined(Handshake);
// Ordinal case-insensitive comparison, for enum names, the firewall keywords and the duplicate serial port
// check. ASCII letters compare without case; a non-ASCII character never equals an
// ASCII one (the Kelvin sign is not 'K', long s is not 'S', dotless i is not 'I', where Qt::CaseInsensitive
// folds the first two); other characters compare by their simple uppercase. Per UTF-16 unit, for the BMP: every caller compares ASCII names or strings the validators already cleared of surrogates.
bool equalsOrdinalIgnoreCase(const QString& a, const QString& b);

// Name lookup ignoring case as Enum.TryParse(ignoreCase: true) did (equalsOrdinalIgnoreCase).
std::optional<Parity> parityFromName(const QString&);
std::optional<StopBits> stopBitsFromName(const QString&);
std::optional<Handshake> handshakeFromName(const QString&);

struct PortMapping {
    bool enabled = true;
    // Both pass validation with ONE trailing '\n' ("KAT500\n", "COM3\n"): `$` in the patterns matches
    // before a final newline. Consumers must not assume either is newline-free: trim or reject it
    // where it matters (a firewall rule's DisplayName, a log line, a device open), never rely on the fence.
    QString name, comPort;
    int baudRate = 38400, dataBits = 8;
    Parity parity = Parity::None;
    StopBits stopBits = StopBits::One;
    Handshake handshake = Handshake::None;
    // Off by default: that is how the bridge was proven against real Elecraft gear, and some
    // amplifiers treat DTR/RTS as power or PTT lines.
    bool dtrEnable = false, rtsEnable = false;
    int tcpPort = 0;
    // Local address to listen on; 0.0.0.0 listens on every interface.
    QString bindAddress;
    // Remote addresses the Windows Firewall rule admits (see AllowedClients). Blank admits any
    // address. Linux records it but does not enforce it (D7). Never pass it to a firewall as is: use
    // AllowedClients::tokens(), the one tokeniser.
    QString allowedClients;

    // The name, or the serial port when the name is blank. Computed, never stored (a `displayName` in a
    // file is ignored on read, and the writer does not write one). It can end in
    // one '\n' (see `name`).
    QString displayName() const;

    bool operator==(const PortMapping&) const = default;
};

struct BridgeConfig {
    int version = 1;
    QList<PortMapping> ports;

    // Every problem found, port by port in a fixed order; empty means the config is usable.
    QStringList validate() const;

    bool operator==(const BridgeConfig&) const = default;
};

// An IP address as the validators (and so the engine and the firewall plan) understand it.
struct IpAddress {
    enum class Family { V4, V6 } family = Family::V4;
    std::array<quint8, 16> bytes{};   // V4 uses the first 4
    QString scopeId;                  // V6 only ("%3", "%eth0" without the '%')
    int size() const { return family == Family::V4 ? 4 : 16; }
    bool operator==(const IpAddress&) const = default;
};

// Parses a full address:
//  - contains ':' -> an IPv6 address (optionally with a %scope); brackets and ports are refused;
//  - otherwise exactly four dot-separated groups of 1-3 ASCII digits, each <= 255 as written in
//    decimal, then read with a leading 0 as octal
//    ("010" = 8; "08" is invalid). Shorthand forms ("10", "1.2.3", "127.1") and hex are refused.
// This is also the bind-address rule.
std::optional<IpAddress> parseIpAddress(const QString&);
// Canonical text: dotted decimal for V4; RFC 5952 for V6 (with %scope if any).
QString toString(const IpAddress&);
// The unspecified address ("every interface") by value, in either family: 0.0.0.0 however it is spelled
// ("0.00.000.0"), ::, 0:0:0:0:0:0:0:0 (any %scope), and the IPv4-mapped ::ffff:0.0.0.0. Never compare the text.
bool isUnspecified(const IpAddress&);
// A bind address that listens on every interface: it parses (parseIpAddress) and isUnspecified.
bool listensOnEveryInterface(const QString& bindAddress);

}  // namespace asb
