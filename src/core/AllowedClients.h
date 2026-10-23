#pragma once

// A port's allow-list (INV-14), so a value the Windows firewall would
// reject never reaches a firewall change. Each comma-separated token must be an IPv4/IPv6 address, a CIDR
// block with a valid prefix, a same-family range a-b with a <= b, or an accepted RemoteAddress keyword
// (FirewallKeywords.h). Blank means any address.
//
// This file is the single owner of how the list is read: the character fence, the split on ',' and the
// trim. Firewall code (WS-5's plan, WS-7's MSFT_NetAddressFilter.RemoteAddress) MUST take its addresses
// from tokens() and must never pass the raw `allowedClients` string through, split it itself, or treat a
// value it could not tokenise as "any": the fence admits one trailing '\n' ("1.2.3.4\n"), and
// only tokens() hands out the trimmed tokens that were validated.

#include <QString>
#include <QStringList>

#include <optional>
#include <span>
#include <string_view>

namespace asb::AllowedClients {

// The ASCII quoting fence BridgeConfig::validate runs before check(): only 0-9 A-Z a-z . : / - , and
// space, at most 500 of them (plus one final '\n', which `$` matches before).
bool passesCharacterFence(const QString& clients);

// The addresses a firewall rule admits: the value split on ',' and each token trimmed, exactly as check()
// reads it. Fails closed: nullopt unless the value passes the fence and check() reports nothing, so an
// invalid list can never be mistaken for "any address". An empty list means any address (a blank value).
std::optional<QStringList> tokens(const QString& clients);

// One message per bad token (without the port name), in token order; empty when the list is usable.
QStringList check(const QString& clients);

// The same check against an explicit keyword policy: `accepted` are admitted, and `known` keywords
// that are not accepted get the "firewall does not accept" message instead of "not an IP address".
// check(clients) == check(clients, kAcceptedFirewallKeywords, kKnownFirewallKeywords).
QStringList check(const QString& clients, std::span<const std::string_view> accepted,
                  std::span<const std::string_view> known);

}  // namespace asb::AllowedClients
