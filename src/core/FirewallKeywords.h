#pragma once

// Windows Firewall RemoteAddress keywords an allow-list may use (INV-30, master §4.2).
//
// The known set is the twelve keywords Windows documents for a rule's remote address. The bridge admits only
// the ones the Windows CIM provider is known to accept, and never translates a spelling: a known keyword the
// provider refuses is a validation error before any firewall change, with its own message.
//
// PINNED by WS-7's `fw_keyword_matrix` (VERIFY-ON-WINDOWS): one rule per keyword is created through CIM and
// its stored RemoteAddress read back. On windows-latest (Windows Server 2025, build 10.0.26100) every one of the
// twelve known keywords is stored verbatim (case canonicalised: "localsubnet" -> "LocalSubnet"; "Any" reads back
// as an unset filter), and `PlayToTargets` — the spelling Microsoft documents for MSFT_NetAddressFilter — is
// REFUSED by the provider (WBEM_E_FAILED), as is anything else. So the accepted set is the known set. No spelling
// is ever translated. The qualification script (qualification/ws7-station.ps1, item KEYWORDS) runs the same
// matrix on the station PC's build; a build that refuses a keyword here is a pin change for that build (the
// station's result is recorded in the WS-7 doc). Matching is case-insensitive.

#include <array>
#include <string_view>

namespace asb {

// Every keyword the validator knows.
inline constexpr std::array<std::string_view, 12> kKnownFirewallKeywords = {
    "Any", "LocalSubnet", "LocalSubnet4", "LocalSubnet6", "DNS", "DHCP", "WINS", "DefaultGateway",
    "Internet", "Intranet", "IntranetRemoteAccess", "PlayToDevice",
};

// The keywords the bridge accepts (see above): every one the provider stores. Must stay a subset of
// kKnownFirewallKeywords.
inline constexpr std::array<std::string_view, 12> kAcceptedFirewallKeywords = {
    "Any", "LocalSubnet", "LocalSubnet4", "LocalSubnet6", "DNS", "DHCP", "WINS", "DefaultGateway",
    "Internet", "Intranet", "IntranetRemoteAccess", "PlayToDevice",
};

}  // namespace asb
