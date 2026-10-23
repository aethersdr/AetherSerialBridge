#include "core/FirewallPlan.h"

#include "core/AllowedClients.h"

#include <QRegularExpression>

namespace asb::FirewallPlan {
namespace {

bool ofGeneration(const QString& instanceId, const FirewallGeneration& g)
{
    const std::optional<RuleName> n = parseRuleName(instanceId);
    return n && n->generation == g.id;
}

}  // namespace

std::optional<Plan> build(const BridgeConfig& config, const FirewallGeneration& generation, const QString& program,
                          QString* error)
{
    if (!FirewallGeneration::isValidId(generation.id)) {
        if (error) *error = QStringLiteral("the firewall generation must be 16 lower-case hex digits");
        return std::nullopt;
    }
    const QStringList problems = config.validate();
    if (!problems.isEmpty()) {
        if (error) *error = problems.join(QLatin1Char(' '));
        return std::nullopt;
    }
    Plan plan;
    plan.generation = generation;
    int i = 0;
    for (const PortMapping& p : config.ports) {
        if (!p.enabled) continue;
        Rule r;
        r.index = i;
        r.instanceId = QString::fromLatin1(kNamePrefix) + QString::fromLatin1(generation.id) + QLatin1Char('-') +
                       QString::number(i);
        // The addresses come ONLY from AllowedClients::tokens(): the one reader of the list. A list it
        // cannot tokenise refuses the plan; it never becomes "any address".
        const std::optional<QStringList> remotes = AllowedClients::tokens(p.allowedClients);
        if (!remotes) {
            if (error) *error = QStringLiteral("%1: the allowed clients cannot be read").arg(p.displayName().trimmed());
            return std::nullopt;
        }
        // A name (or serial port) may carry one trailing newline (BridgeConfig.h): never into a rule.
        QString shown = p.displayName();
        if (shown.endsWith(QLatin1Char('\n'))) shown.chop(1);
        r.displayName = QStringLiteral("%1 - %2 (TCP %3)").arg(QString::fromLatin1(kGroup), shown).arg(p.tcpPort);
        r.group = QString::fromLatin1(kGroup);
        r.protocol = QStringLiteral("TCP");
        r.localPorts = {QString::number(p.tcpPort)};
        r.remoteAddresses = remotes->isEmpty() ? QStringList{QStringLiteral("Any")} : *remotes;
        r.program = program;
        plan.rules << r;
        ++i;
    }
    return plan;
}

std::optional<RuleName> parseRuleName(const QString& instanceId)
{
    static const QRegularExpression re(QStringLiteral(R"re(\AAetherSerialBridge-([0-9a-f]{16})-(0|[1-9][0-9]{0,2})\z)re"));
    const QRegularExpressionMatch m = re.match(instanceId);
    if (!m.hasMatch()) return std::nullopt;
    return RuleName{m.captured(1).toLatin1(), m.captured(2).toInt()};
}

QList<Step> syncSteps(const Plan& plan, const QStringList& existing)
{
    QList<Step> steps;
    for (int i = 0; i < plan.rules.size(); ++i) {
        const QString& id = plan.rules[i].instanceId;
        steps << Step{Step::Kind::CreateDisabled, id, i} << Step{Step::Kind::SetPortFilter, id, i}
              << Step{Step::Kind::SetAddressFilter, id, i} << Step{Step::Kind::SetApplicationFilter, id, i}
              << Step{Step::Kind::Enable, id, i};
    }
    // Only after every new rule is enabled: everything else in the group goes.
    steps += sweepSteps(plan.generation, existing);
    return steps;
}

QList<Step> rollbackSteps(const FirewallGeneration& g, const QStringList& existing)
{
    QList<Step> steps;
    for (const QString& id : existing)
        if (ofGeneration(id, g)) steps << Step{Step::Kind::Delete, id, -1};
    return steps;
}

QList<Step> sweepSteps(const FirewallGeneration& keep, const QStringList& existing)
{
    QList<Step> steps;
    for (const QString& id : existing)
        if (!ofGeneration(id, keep)) steps << Step{Step::Kind::Delete, id, -1};
    return steps;
}

QList<Step> removeAllSteps(const QStringList& existing)
{
    QList<Step> steps;
    for (const QString& id : existing) steps << Step{Step::Kind::Delete, id, -1};
    return steps;
}

}  // namespace asb::FirewallPlan
