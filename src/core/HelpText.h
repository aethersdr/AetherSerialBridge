#pragma once

// What the Allowed field means, in the words the window's port card and the TUI's help both show (owner request
// 2026-10-19: "some more help about what allowed means like perhaps an entry example"). One place, so the two UIs
// say the same thing and a test can hold the example to the validator (AllowedClients::check) and the keywords to
// the ones the firewall accepts (FirewallKeywords.h).
//
// The example uses documentation addresses (RFC 5737): the docs images' manifests list every text the window shows,
// and tools/check_docs_images.py refuses any other address in them.

#include <QString>

namespace asb::help {

// A list a user could type into Allowed as is.
inline constexpr char kAllowedExample[] = "192.0.2.10, 198.51.100.0/24";
// A keyword named on Windows only, where the firewall understands it (kAcceptedFirewallKeywords).
inline constexpr char kAllowedKeywordExample[] = "LocalSubnet";

// What happens to the list on this platform, as one or two plain sentences.
// Windows: FirewallPlan::build makes it the RemoteAddress of the port's firewall rule (blank: Any). Nothing checks
// that the firewall is on, and other allow rules add up, so it says what the rule admits, never that it is enforced.
// Linux: NullFirewall; the list is validated and saved but nothing enforces it (D7).
inline QString allowedEffect(bool windows)
{
    return windows ? QStringLiteral("While Windows Firewall is on, this port's rule lets in only these addresses.")
                   : QStringLiteral("Linux does not enforce this list: use the system firewall to restrict who can "
                                    "connect.");
}

// The example line under the field: what to type, and what blank means.
inline QString allowedExampleLine(bool windows)
{
    return windows ? QStringLiteral("e.g. %1 or %2 · blank = any address")
                         .arg(QString::fromLatin1(kAllowedExample), QString::fromLatin1(kAllowedKeywordExample))
                   : QStringLiteral("e.g. %1 · blank = any address").arg(QString::fromLatin1(kAllowedExample));
}

}  // namespace asb::help
