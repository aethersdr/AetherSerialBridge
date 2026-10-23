#pragma once

// The Windows firewall comparator (WS-7; D53, INV-117) and the RemoteAddress normalisation both the writer and
// the comparator use. Pure and portable: the CIM adapter (CimFirewall.h) reads the reserved group into a
// GroupRead and this file decides.
//
// Classification of every rule in the reserved group "Aether Serial Bridge" (read from the ActiveStore):
//  - the read must be complete: the group query and every association query ended normally (the reader throws
//    otherwise), and every rule has exactly one port, address, application and security filter -> else Failed;
//  - Enabled == 2 (False): ignored here (no exposure; the Applier's sweep deletes it);
//  - every other rule is an exposure and must be one of ours, exactly as planned: InstanceID
//    `AetherSerialBridge-<16 hex>-<index>` of the token's generation, an index the plan contains, the only
//    enabled rule with that index, Direction Inbound (1), Action Allow (2), Profiles Any (0), EdgeTraversalPolicy
//    Block (0), PolicyStoreSourceType Local (1); port filter Protocol TCP, LocalPort = [port], RemotePort Any;
//    address filter LocalAddress Any (the writer leaves it unset), RemoteAddress equal to the plan's as a
//    normalised SET; application filter AppPath = the installed exe (case-insensitive, '\' separators); security
//    filter OverrideBlockRules False (the writer leaves it at its default) -> else Mismatch;
//  - every planned rule must be present and enabled -> else Mismatch.
// An empty group is a Match only for an empty plan, and only because the read completed (the reader never
// returns a partial group).
//
// Read-back facts (WS-7 probe, windows-latest 10.0.26100): unset filter properties read back NULL, never "Any";
// RemoteAddress ["Any"] reads back NULL; an IPv4 prefix a/n reads back as a/<dotted mask>; keywords read back in
// their canonical spelling; the provider reorders the list.

#include "core/BridgeConfig.h"
#include "core/FirewallPlan.h"
#include "core/Interfaces.h"

#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

namespace asb::win::FirewallCompare {

// ---- RemoteAddress values ----------------------------------------------------------------------------------
// The value the writer passes to MSFT_NetAddressFilter.RemoteAddress for one AllowedClients token: an accepted
// keyword in its canonical spelling (never translated to another keyword), an address in canonical form
// (parseIpAddress semantics: "010.0.0.1" is 8.0.0.1, as the engine reads it), a prefix with the network bits
// masked ("10.1.2.3/8" -> "10.0.0.0/8"), or a range "a-b" of canonical addresses. nullopt = not a usable token.
std::optional<QString> writerValue(const QString& token);
// The writer's list for a plan rule (tokens from AllowedClients::tokens(), or ["Any"]). nullopt if any token is
// unusable (the sync then fails: never "Any" by accident).
std::optional<QStringList> writerValues(const QStringList& tokens);

// A comparison key for one value as the plan has it or as the provider returns it: "K:<keyword>", "A:<addr>",
// "P:<net>/<len>" (a full-length prefix is the address), "R:<a>-<b>"; "K:Any" for Any. Accepts the provider's
// "a/255.255.255.0" mask form (contiguous masks only). nullopt = unparseable (-> Mismatch).
std::optional<QString> addressKey(const QString& value);
// The normalised SET of a RemoteAddress list; null/empty/"Any" anywhere -> {"K:Any"}. nullopt if any element is
// unparseable.
std::optional<QStringList> addressSet(const QStringList& values, bool isNull);

// ---- what the reader returns ------------------------------------------------------------------------------
// One CIM value as read: null (property unset), an integer (uint16/uint32/boolean as 0/1), a string, or a
// string array.
struct Value {
    enum class Kind { Null, Int, String, Strings } kind = Kind::Null;
    qint64 i = 0;
    QString s;
    QStringList list;
    static Value null() { return {}; }
    static Value integer(qint64 v) { Value x; x.kind = Kind::Int; x.i = v; return x; }
    static Value string(const QString& v) { Value x; x.kind = Kind::String; x.s = v; return x; }
    static Value strings(const QStringList& v) { Value x; x.kind = Kind::Strings; x.list = v; return x; }
    bool operator==(const Value&) const = default;
};
using Props = QMap<QString, Value>;

struct RuleRead {
    Props rule;
    // Every object each association query returned (exactly one each is expected).
    QList<Props> port, address, application, security;
};

struct Verdict {
    IFirewallMonitor::Check check = IFirewallMonitor::Check::Failed;
    QString why;   // the first reason (diagnostics, tests; the dying process never writes it anywhere)
};

// The plan carries the program the rules must name (the installed service exe).
Verdict compare(const QList<RuleRead>& group, const FirewallPlan::Plan& plan);

}  // namespace asb::win::FirewallCompare
