#include "support/FakeCim.h"

#include <QRegularExpression>

namespace asb::test {

using win::cim::CimError;
using win::cim::Instance;
using win::cim::Props;
using win::cim::PutFlag;
using win::cim::Store;
using win::cim::Value;

namespace {

const char* flagName(PutFlag f)
{
    switch (f) {
    case PutFlag::CreateOnly: return "CREATE_ONLY";
    case PutFlag::UpdateOnly: return "UPDATE_ONLY";
    case PutFlag::CreateOrUpdate: return "CREATE_OR_UPDATE";
    }
    return "?";
}

QString show(const Value& v)
{
    switch (v.kind) {
    case Value::Kind::Null: return QStringLiteral("null");
    case Value::Kind::Int: return QString::number(v.i);
    case Value::Kind::String: return v.s;
    case Value::Kind::Strings: return QLatin1Char('[') + v.list.join(QLatin1Char(',')) + QLatin1Char(']');
    }
    return {};
}

const QMap<QString, QString>& filterClasses()
{
    static const QMap<QString, QString> m{
        {QStringLiteral("MSFT_NetFirewallRuleFilterByProtocolPort"), QStringLiteral("MSFT_NetProtocolPortFilter")},
        {QStringLiteral("MSFT_NetFirewallRuleFilterByAddress"), QStringLiteral("MSFT_NetAddressFilter")},
        {QStringLiteral("MSFT_NetFirewallRuleFilterByApplication"), QStringLiteral("MSFT_NetApplicationFilter")},
        {QStringLiteral("MSFT_NetFirewallRuleFilterBySecurity"), QStringLiteral("MSFT_NetNetworkLayerSecurityFilter")},
    };
    return m;
}

}  // namespace

QString FakeCim::relPathOf(const QString& cls, const QString& id)
{
    return QStringLiteral("%1.CreationClassName=\"MSFT|FW|FirewallRule|%2\"").arg(cls, id);
}

FakeCim::Rule FakeCim::ruleWithDefaults(const QString& id, const QString& group, qint64 enabled)
{
    Rule r;
    r.rule[QStringLiteral("InstanceID")] = Value::string(id);
    r.rule[QStringLiteral("ElementName")] = Value::string(id);
    r.rule[QStringLiteral("RuleGroup")] = Value::string(group);
    r.rule[QStringLiteral("Direction")] = Value::integer(1);
    r.rule[QStringLiteral("Action")] = Value::integer(2);
    r.rule[QStringLiteral("Enabled")] = Value::integer(enabled);
    r.rule[QStringLiteral("Profiles")] = Value::integer(0);
    r.rule[QStringLiteral("EdgeTraversalPolicy")] = Value::integer(0);
    r.rule[QStringLiteral("PolicyStoreSourceType")] = Value::integer(1);
    r.port[QStringLiteral("InstanceID")] = Value::string(id);
    r.port[QStringLiteral("Protocol")] = Value::string(QStringLiteral("Any"));
    r.address[QStringLiteral("InstanceID")] = Value::string(id);
    r.application[QStringLiteral("InstanceID")] = Value::string(id);
    r.security[QStringLiteral("InstanceID")] = Value::string(id);
    r.security[QStringLiteral("OverrideBlockRules")] = Value::integer(0);
    return r;
}

FakeCim::Rule FakeCim::asWritten(const QString& id, const QString& group, const QString& port, const QStringList& remote,
                                 const QString& appPath)
{
    Rule r = ruleWithDefaults(id, group, 1);
    r.port[QStringLiteral("Protocol")] = Value::string(QStringLiteral("TCP"));
    r.port[QStringLiteral("LocalPort")] = Value::strings({port});
    if (!(remote.size() == 1 && remote.front() == QLatin1String("Any")))
        r.address[QStringLiteral("RemoteAddress")] = Value::strings(remote);
    r.application[QStringLiteral("AppPath")] = Value::string(appPath);
    return r;
}

void FakeCim::call(const QString& line)
{
    {
        std::lock_guard g(m);
        log << line;
    }
    if (hook) hook(line);
}

void FakeCim::connect()
{
    call(QStringLiteral("connect"));
    std::lock_guard g(m);
    ++connects;
}

FakeCim::Rule* FakeCim::findByRelPath(const QString& relPath, QString* kind, Store store)
{
    static const QRegularExpression re(QStringLiteral(R"re(\A(\w+)\.CreationClassName="MSFT\|FW\|FirewallRule\|(.*)"\z)re"));
    const auto mm = re.match(relPath);
    if (!mm.hasMatch()) return nullptr;
    *kind = mm.captured(1);
    const QString id = mm.captured(2);
    if (store != Store::LocalGpo) {
        auto it = persistent.find(id);
        if (it != persistent.end()) return &*it;
    }
    if (store != Store::Persistent) {
        auto it = gpoRules.find(id);
        if (it != gpoRules.end()) return &*it;
    }
    return nullptr;
}

QList<Instance> FakeCim::query(const QString& wql, Store store)
{
    call(QStringLiteral("query %1 %2").arg(win::cim::storeName(store), wql));
    std::lock_guard g(m);
    QList<std::pair<QString, Rule*>> scope;
    if (store != Store::LocalGpo)
        for (auto it = persistent.begin(); it != persistent.end(); ++it) scope << std::pair{it.key(), &*it};
    if (store != Store::Persistent)
        for (auto it = gpoRules.begin(); it != gpoRules.end(); ++it) scope << std::pair{it.key(), &*it};

    QList<Instance> out;
    static const QRegularExpression byGroup(QStringLiteral(R"re(\ASELECT \* FROM MSFT_NetFirewallRule WHERE RuleGroup = '(.*)'\z)re"));
    static const QRegularExpression byId(QStringLiteral(R"re(\ASELECT \* FROM MSFT_NetFirewallRule WHERE InstanceID = '(.*)'\z)re"));
    static const QRegularExpression assoc(QStringLiteral(R"re(\AASSOCIATORS OF \{(.*)\} WHERE AssocClass = (\w+)\z)re"));
    if (const auto mm = byGroup.match(wql); mm.hasMatch()) {
        for (auto& [id, r] : scope)
            if (r->rule.value(QStringLiteral("RuleGroup")).s == mm.captured(1))
                out << Instance{relPathOf(QStringLiteral("MSFT_NetFirewallRule"), id), r->rule};
        return out;
    }
    if (const auto mm = byId.match(wql); mm.hasMatch()) {
        for (auto& [id, r] : scope)
            if (id == mm.captured(1)) out << Instance{relPathOf(QStringLiteral("MSFT_NetFirewallRule"), id), r->rule};
        return out;
    }
    if (const auto mm = assoc.match(wql); mm.hasMatch()) {
        QString kind;
        Rule* r = findByRelPath(mm.captured(1), &kind, store);
        if (!r || kind != QLatin1String("MSFT_NetFirewallRule")) throw CimError(win::cim::kWbemNotFound, QStringLiteral("Not found"));
        const QString cls = filterClasses().value(mm.captured(2));
        const QString id = r->rule.value(QStringLiteral("InstanceID")).s;
        if (cls == QLatin1String("MSFT_NetProtocolPortFilter")) out << Instance{relPathOf(cls, id), r->port};
        else if (cls == QLatin1String("MSFT_NetAddressFilter")) out << Instance{relPathOf(cls, id), r->address};
        else if (cls == QLatin1String("MSFT_NetApplicationFilter")) out << Instance{relPathOf(cls, id), r->application};
        else if (cls == QLatin1String("MSFT_NetNetworkLayerSecurityFilter")) out << Instance{relPathOf(cls, id), r->security};
        else throw CimError(0x80041017, QStringLiteral("Invalid query"));
        return out;
    }
    throw CimError(0x80041017, QStringLiteral("Invalid query"));
}

void FakeCim::put(const QString& className, const QString& relPath, const Props& props, PutFlag flag, Store store,
                  const std::function<void()>& beforeWrite)
{
    call(QStringLiteral("get %1 %2").arg(win::cim::storeName(store), relPath.isEmpty() ? className : relPath));
    if (beforeWrite) beforeWrite();
    QString line = QStringLiteral("put %1 %2 %3").arg(win::cim::storeName(store), QString::fromLatin1(flagName(flag)),
                                                       relPath.isEmpty() ? className : relPath);
    for (auto it = props.cbegin(); it != props.cend(); ++it) line += QStringLiteral(" %1=%2").arg(it.key(), show(*it));
    call(line);
    std::lock_guard g(m);
    QMap<QString, Rule>& target = store == Store::LocalGpo ? gpoRules : persistent;
    auto overlay = [](Props& dst, const Props& src) {
        for (auto it = src.cbegin(); it != src.cend(); ++it) dst[it.key()] = *it;
    };
    if (relPath.isEmpty()) {
        const QString id = props.value(QStringLiteral("InstanceID")).s;
        if (target.contains(id)) {
            if (flag == PutFlag::CreateOnly) throw CimError(win::cim::kWbemAlreadyExists, QStringLiteral("Object or property already exists"));
            overlay(target[id].rule, props);
            return;
        }
        if (flag == PutFlag::UpdateOnly) throw CimError(win::cim::kWbemNotFound, QStringLiteral("Not found"));
        Rule r = ruleWithDefaults(id, QString(), 1);
        if (store == Store::LocalGpo) r.rule[QStringLiteral("PolicyStoreSourceType")] = Value::integer(2);
        overlay(r.rule, props);
        target.insert(id, r);
        return;
    }
    QString kind;
    Rule* r = findByRelPath(relPath, &kind, store);
    if (!r) {
        static const QRegularExpression re(QStringLiteral(R"re(\AMSFT_NetFirewallRule\.CreationClassName="MSFT\|FW\|FirewallRule\|(.*)"\z)re"));
        const auto mm = re.match(relPath);
        if (flag == PutFlag::CreateOrUpdate && mm.hasMatch()) {   // resurrects a deleted rule
            Rule nr = ruleWithDefaults(mm.captured(1), QString(), 1);
            overlay(nr.rule, props);
            target.insert(mm.captured(1), nr);
            return;
        }
        throw CimError(win::cim::kWbemNotFound, QStringLiteral("Not found"));
    }
    if (flag == PutFlag::CreateOnly) throw CimError(win::cim::kWbemAlreadyExists, QStringLiteral("Object or property already exists"));
    if (kind == QLatin1String("MSFT_NetFirewallRule")) overlay(r->rule, props);
    else if (kind == QLatin1String("MSFT_NetProtocolPortFilter")) overlay(r->port, props);
    else if (kind == QLatin1String("MSFT_NetAddressFilter")) overlay(r->address, props);
    else if (kind == QLatin1String("MSFT_NetApplicationFilter")) overlay(r->application, props);
    else overlay(r->security, props);
}

void FakeCim::remove(const QString& relPath, Store store)
{
    call(QStringLiteral("remove %1 %2").arg(win::cim::storeName(store), relPath));
    std::lock_guard g(m);
    QString kind;
    Rule* r = findByRelPath(relPath, &kind, store);
    if (!r || kind != QLatin1String("MSFT_NetFirewallRule")) throw CimError(win::cim::kWbemNotFound, QStringLiteral("Not found"));
    const QString id = r->rule.value(QStringLiteral("InstanceID")).s;
    (store == Store::LocalGpo ? gpoRules : persistent).remove(id);
}

QStringList FakeCim::ids() const
{
    std::lock_guard g(m);
    return persistent.keys();
}

QStringList FakeCim::calls(const QString& prefix) const
{
    std::lock_guard g(m);
    QStringList out;
    for (const QString& l : log)
        if (l.startsWith(prefix)) out << l;
    return out;
}

}  // namespace asb::test
