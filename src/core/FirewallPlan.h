#pragma once

// The Windows Firewall plan, pure (D51). One inbound allow rule per
// ENABLED port, all in the reserved group, each named `AetherSerialBridge-<generation>-<index>`; every
// value is a typed field that the CIM adapter (WS-7) passes as a typed property — no command or script
// text is ever built from configuration values (INV-12). The sync is add-then-swap (INV-11): each new rule
// is created disabled, its port/address/application filters set, then
// enabled; only after every new rule is enabled are the rules of other generations deleted. A failed
// sync rolls back only its own generation; the R9 sweep deletes every rule not of the current one.

#include "core/BridgeConfig.h"
#include "core/Interfaces.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace asb::FirewallPlan {

inline constexpr char kGroup[] = "Aether Serial Bridge";
inline constexpr char kNamePrefix[] = "AetherSerialBridge-";

struct Rule {
    int index = 0;                    // position among the enabled ports
    QString instanceId;               // AetherSerialBridge-<16 hex>-<index>
    QString displayName;              // "Aether Serial Bridge - <display name> (TCP <port>)", verbatim
                                      // (one trailing newline of the name removed)
    QString group;                    // kGroup
    enum class Direction { Inbound } direction = Direction::Inbound;
    enum class Action { Allow } action = Action::Allow;
    enum class Profiles { Any } profiles = Profiles::Any;
    QString protocol;                 // "TCP"
    QStringList localPorts;           // [the TCP port]
    QStringList remoteAddresses;      // AllowedClients::tokens(); ["Any"] only for a blank list
    QString program;                  // the installed service executable
    bool operator==(const Rule&) const = default;
};

struct Plan {
    FirewallGeneration generation;
    QList<Rule> rules;
};

// nullopt (with *error) unless `config` validates without errors and `generation` is 16 lower-case
// hex: validation is the first fence and the plan never sees an unvalidated value.
std::optional<Plan> build(const BridgeConfig& config, const FirewallGeneration& generation, const QString& program,
                          QString* error);

// A rule name of ours: the generation and the index. nullopt for anything else.
struct RuleName {
    QByteArray generation;
    int index = 0;
};
std::optional<RuleName> parseRuleName(const QString& instanceId);

// The order of CIM operations.
struct Step {
    enum class Kind { CreateDisabled, SetPortFilter, SetAddressFilter, SetApplicationFilter, Enable, Delete } kind;
    QString instanceId;
    int rule = -1;                    // index into Plan::rules for the create/filter/enable steps
    bool operator==(const Step&) const = default;
};
// `existing` = the InstanceIDs currently in the group.
QList<Step> syncSteps(const Plan&, const QStringList& existing);
QList<Step> rollbackSteps(const FirewallGeneration&, const QStringList& existing);
QList<Step> sweepSteps(const FirewallGeneration& keep, const QStringList& existing);
QList<Step> removeAllSteps(const QStringList& existing);

}  // namespace asb::FirewallPlan
