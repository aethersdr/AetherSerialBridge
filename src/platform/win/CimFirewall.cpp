#include "platform/win/CimFirewall.h"

namespace asb::win {

namespace cim {

QString storeName(Store s)
{
    switch (s) {
    case Store::Persistent: return QStringLiteral("PersistentStore");
    case Store::Active: return QStringLiteral("ActiveStore");
    case Store::LocalGpo: return QStringLiteral("localhost");
    }
    return {};
}

ConnectServerCall connectServerCall()
{
    return ConnectServerCall{QStringLiteral("ROOT\\StandardCimv2"), kWbemConnectUseMaxWait};
}

CimError::CimError(quint32 code, const QString& text)
    : PlatformError(QStringLiteral("%1 (0x%2)").arg(text.isEmpty() ? QStringLiteral("WMI error") : text)
                        .arg(code, 8, 16, QLatin1Char('0'))
                        .toStdString()),
      hr(code)
{
}

}  // namespace cim

namespace {

using cim::Props;
using cim::PutFlag;
using cim::Store;
using cim::Value;

const QString kRuleClass = QStringLiteral("MSFT_NetFirewallRule");

QString assocQuery(const QString& relPath, const char* assocClass)
{
    return QStringLiteral("ASSOCIATORS OF {%1} WHERE AssocClass = %2").arg(relPath, QString::fromLatin1(assocClass));
}

// InstanceIDs are ours (AetherSerialBridge-<hex>-<n>) or read from the group; a backslash and a quote are escaped
// with a backslash anyway (WQL string literals).
QString wqlString(QString s)
{
    s.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    s.replace(QLatin1Char('\''), QStringLiteral("\\'"));
    return QLatin1Char('\'') + s + QLatin1Char('\'');
}

QString idOf(const cim::Instance& i)
{
    const auto it = i.props.constFind(QStringLiteral("InstanceID"));
    return it != i.props.constEnd() && it->kind == Value::Kind::String ? it->s : QString();
}

}  // namespace

QString groupQuery()
{
    return QStringLiteral("SELECT * FROM MSFT_NetFirewallRule WHERE RuleGroup = %1")
        .arg(wqlString(QString::fromLatin1(FirewallPlan::kGroup)));
}

// ---------------------------------------------------------------------------------------------------
// CimFirewall
// ---------------------------------------------------------------------------------------------------

CimFirewall::CimFirewall(ICimSession& session, QString installedExe, IClock& clock)
    : m_s(session), m_exe(std::move(installedExe)), m_clock(clock)
{
}

void CimFirewall::beforeNormalStep(Deadline deadline, const char* what)
{
    // INV-114: once the deadline has passed no further normal transaction step starts.
    if (m_clock.now() > deadline)
        throw PlatformError(std::string("the firewall deadline passed before ") + what);
}

void CimFirewall::requireConnected() const
{
    // WS-5 round 4: every operation connects exactly once, first; a call while unconnected is a step failure.
    if (!m_connected) throw PlatformError("the firewall provider is not connected");
}

void CimFirewall::connect(Deadline deadline)
{
    if (m_connected) return;
    beforeNormalStep(deadline, "connecting to the firewall provider");
    m_s.connect();
    m_connected = true;
}

void CimFirewall::disconnect()
{
    // At the end of an operation, on the worker that connected (OperationHost): the session's COM proxies are released
    // on their own thread, and the next operation connects again.
    m_connected = false;
    m_s.disconnect();
}

QStringList CimFirewall::groupIds()
{
    QStringList ids;
    for (const cim::Instance& r : m_s.query(groupQuery(), Store::Persistent)) ids << idOf(r);
    return ids;
}

QString CimFirewall::rulePath(const QString& instanceId)
{
    const QList<cim::Instance> found = m_s.query(
        QStringLiteral("SELECT * FROM MSFT_NetFirewallRule WHERE InstanceID = %1").arg(wqlString(instanceId)),
        Store::Persistent);
    if (found.size() != 1)
        throw PlatformError(QStringLiteral("firewall rule '%1' was not found exactly once (%2 found)")
                                .arg(instanceId)
                                .arg(found.size())
                                .toStdString());
    return found.front().relPath;
}

QString CimFirewall::filterPath(const QString& ruleRelPath, const char* assocClass, const char* what)
{
    const QList<cim::Instance> f = m_s.query(assocQuery(ruleRelPath, assocClass), Store::Persistent);
    if (f.size() != 1)
        throw PlatformError(QStringLiteral("the rule has %1 %2 filter(s), expected exactly one")
                                .arg(f.size())
                                .arg(QString::fromLatin1(what))
                                .toStdString());
    return f.front().relPath;
}

void CimFirewall::runSteps(const QList<FirewallPlan::Step>& steps, const FirewallPlan::Plan* plan, Deadline deadline,
                           bool cleanup)
{
    using K = FirewallPlan::Step::Kind;
    // A step's lookups (rulePath, filterPath; the session's own GetObject) block, and may return after the deadline:
    // the write that follows them is judged again first (review round 1). The rollback (cleanup) is exempt.
    const std::function<void()> onTime = [this, deadline, cleanup] {
        if (!cleanup) beforeNormalStep(deadline, "a firewall write whose lookup returned late");
    };
    for (const FirewallPlan::Step& s : steps) {
        if (!cleanup) beforeNormalStep(deadline, "the next firewall step");
        const FirewallPlan::Rule* rule = plan && s.rule >= 0 ? &plan->rules[s.rule] : nullptr;
        switch (s.kind) {
        case K::CreateDisabled: {
            Props p;
            p[QStringLiteral("InstanceID")] = Value::string(rule->instanceId);
            p[QStringLiteral("ElementName")] = Value::string(rule->displayName);
            p[QStringLiteral("RuleGroup")] = Value::string(rule->group);
            p[QStringLiteral("Direction")] = Value::integer(1);   // Inbound
            p[QStringLiteral("Action")] = Value::integer(2);      // Allow
            p[QStringLiteral("Enabled")] = Value::integer(2);     // False: no exposure until the filters are set
            p[QStringLiteral("Profiles")] = Value::integer(0);    // Any
            m_s.put(kRuleClass, QString(), p, PutFlag::CreateOnly, Store::Persistent, onTime);
            break;
        }
        case K::SetPortFilter: {
            Props p;
            p[QStringLiteral("Protocol")] = Value::string(rule->protocol);
            p[QStringLiteral("LocalPort")] = Value::strings(rule->localPorts);
            m_s.put(QString(), filterPath(rulePath(s.instanceId), kAssocPort, "port"), p, PutFlag::UpdateOnly,
                    Store::Persistent, onTime);
            break;
        }
        case K::SetAddressFilter: {
            const std::optional<QStringList> values = FirewallCompare::writerValues(rule->remoteAddresses);
            if (!values)
                throw PlatformError(QStringLiteral("%1: the allowed clients cannot be written to the firewall")
                                        .arg(rule->displayName)
                                        .toStdString());
            Props p;
            p[QStringLiteral("RemoteAddress")] = Value::strings(*values);
            m_s.put(QString(), filterPath(rulePath(s.instanceId), kAssocAddress, "address"), p, PutFlag::UpdateOnly,
                    Store::Persistent, onTime);
            break;
        }
        case K::SetApplicationFilter: {
            Props p;
            p[QStringLiteral("AppPath")] = Value::string(rule->program);
            m_s.put(QString(), filterPath(rulePath(s.instanceId), kAssocApplication, "application"), p,
                    PutFlag::UpdateOnly, Store::Persistent, onTime);
            break;
        }
        case K::Enable: {
            Props p;
            p[QStringLiteral("Enabled")] = Value::integer(1);
            m_s.put(QString(), rulePath(s.instanceId), p, PutFlag::UpdateOnly, Store::Persistent, onTime);
            break;
        }
        case K::Delete: {
            const QString path = rulePath(s.instanceId);
            onTime();   // the lookup may have returned late
            m_s.remove(path, Store::Persistent);
            break;
        }
        }
    }
}

void CimFirewall::sync(const BridgeConfig& config, const FirewallGeneration& g, Deadline deadline)
{
    requireConnected();
    QString why;
    const std::optional<FirewallPlan::Plan> plan = FirewallPlan::build(config, g, m_exe, &why);
    if (!plan) throw PlatformError(why.toStdString());
    beforeNormalStep(deadline, "reading the firewall group");
    const QStringList existing = groupIds();
    runSteps(FirewallPlan::syncSteps(*plan, existing), &*plan, deadline, false);
}

void CimFirewall::rollback(const FirewallGeneration& g, Deadline deadline)
{
    requireConnected();   // cleanup: not refused by the deadline, but never on an unconnected session
    runSteps(FirewallPlan::rollbackSteps(g, groupIds()), nullptr, deadline, true);
}

void CimFirewall::sweep(const FirewallGeneration& keep, Deadline deadline)
{
    requireConnected();
    beforeNormalStep(deadline, "reading the firewall group");
    runSteps(FirewallPlan::sweepSteps(keep, groupIds()), nullptr, deadline, false);
}

void CimFirewall::removeAll(Deadline deadline)
{
    requireConnected();
    beforeNormalStep(deadline, "reading the firewall group");
    runSteps(FirewallPlan::removeAllSteps(groupIds()), nullptr, deadline, false);
}

QList<FirewallCompare::RuleRead> CimFirewall::readGroup(ICimSession& s, Store store)
{
    QList<FirewallCompare::RuleRead> out;
    for (const cim::Instance& r : s.query(groupQuery(), store)) {
        FirewallCompare::RuleRead rr;
        rr.rule = r.props;
        auto read = [&](const char* assoc) {
            QList<Props> props;
            for (const cim::Instance& f : s.query(assocQuery(r.relPath, assoc), store)) props << f.props;
            return props;
        };
        rr.port = read(kAssocPort);
        rr.address = read(kAssocAddress);
        rr.application = read(kAssocApplication);
        rr.security = read(kAssocSecurity);
        out << rr;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------
// CimFirewallMonitor
// ---------------------------------------------------------------------------------------------------

CimFirewallMonitor::CimFirewallMonitor(ICimSession& session, QString installedExe)
    : m_s(session), m_exe(std::move(installedExe))
{
}

void CimFirewallMonitor::threadInit()
{
    try {
        m_s.connect();
        m_connected = true;
    } catch (...) {
        m_connected = false;   // every check then fails: exit 7 by the watchdog (fail closed)
    }
}

IFirewallMonitor::Check CimFirewallMonitor::checkOnce(const BridgeConfig& config, const FirewallGeneration& g)
{
    try {
        if (!m_connected) {
            m_s.connect();
            m_connected = true;
        }
        QString why;
        const std::optional<FirewallPlan::Plan> plan = FirewallPlan::build(config, g, m_exe, &why);
        if (!plan) {
            m_lastWhy = why;
            return Check::Failed;
        }
        const FirewallCompare::Verdict v = FirewallCompare::compare(CimFirewall::readGroup(m_s, Store::Active), *plan);
        m_lastWhy = v.why;
        return v.check;
    } catch (const std::exception& e) {
        m_lastWhy = QString::fromUtf8(e.what());
        return Check::Failed;
    } catch (...) {
        m_lastWhy = QStringLiteral("unknown failure");
        return Check::Failed;
    }
}

}  // namespace asb::win
