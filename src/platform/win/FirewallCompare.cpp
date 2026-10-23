#include "platform/win/FirewallCompare.h"

#include "core/FirewallKeywords.h"

#include <QRegularExpression>

#include <algorithm>

namespace asb::win::FirewallCompare {
namespace {

using Check = IFirewallMonitor::Check;

std::optional<QString> acceptedKeyword(const QString& t)
{
    for (std::string_view k : kAcceptedFirewallKeywords) {
        const QString kw = QString::fromLatin1(k.data(), qsizetype(k.size()));
        if (equalsOrdinalIgnoreCase(t, kw)) return kw;
    }
    return std::nullopt;
}

bool isWord(const QString& t)
{
    static const QRegularExpression re(QStringLiteral(R"(\A[A-Za-z][A-Za-z0-9]*\z)"));
    return re.match(t).hasMatch();
}

int maxBits(const IpAddress& a) { return a.family == IpAddress::Family::V4 ? 32 : 128; }

// The family's last address (all bits set): the upper end of a /0 written as a range.
IpAddress allOnes(IpAddress a)
{
    for (int i = 0; i < a.size(); ++i) a.bytes[size_t(i)] = 0xFF;
    return a;
}

IpAddress masked(IpAddress a, int bits)
{
    for (int i = 0; i < a.size(); ++i) {
        const int keep = std::clamp(bits - 8 * i, 0, 8);
        const quint8 m = keep == 0 ? 0 : quint8(0xFF << (8 - keep));
        a.bytes[size_t(i)] &= m;
    }
    return a;
}

// A dotted IPv4 mask -> its prefix length; nullopt unless contiguous.
std::optional<int> maskBits(const QString& s)
{
    const auto m = parseIpAddress(s);
    if (!m || m->family != IpAddress::Family::V4) return std::nullopt;
    quint32 v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | m->bytes[size_t(i)];
    int bits = 0;
    while (bits < 32 && (v & (0x80000000u >> bits))) ++bits;
    if (bits < 32 && (v << bits) != 0) return std::nullopt;   // a hole: not a mask
    return bits;
}

std::optional<int> prefixBits(const QString& s, const IpAddress& net)
{
    static const QRegularExpression digits(QStringLiteral(R"(\A[0-9]{1,3}\z)"));
    if (digits.match(s).hasMatch()) {
        const int n = s.toInt();
        if (n > maxBits(net)) return std::nullopt;
        return n;
    }
    if (net.family == IpAddress::Family::V4) return maskBits(s);
    return std::nullopt;
}

struct Parsed {
    enum class Kind { Keyword, Address, Prefix, Range } kind;
    QString keyword;
    IpAddress a, b;
    int bits = 0;
};

std::optional<Parsed> parse(const QString& raw, bool keywordsFromAcceptedListOnly)
{
    const QString t = raw.trimmed();
    if (t.isEmpty()) return std::nullopt;
    if (const auto kw = acceptedKeyword(t)) return Parsed{Parsed::Kind::Keyword, *kw, {}, {}, 0};
    const QStringList dash = t.split(QLatin1Char('-'));
    if (dash.size() == 2) {
        const auto a = parseIpAddress(dash[0]);
        const auto b = parseIpAddress(dash[1]);
        if (!a || !b || a->family != b->family) return std::nullopt;
        if (*a == *b) return Parsed{Parsed::Kind::Address, {}, *a, {}, 0};
        // The whole family as a range is the /0 prefix (the form writerValue writes for it; WS-13).
        if (*a == masked(*a, 0) && *b == allOnes(*b)) return Parsed{Parsed::Kind::Prefix, {}, *a, {}, 0};
        return Parsed{Parsed::Kind::Range, {}, *a, *b, 0};
    }
    if (dash.size() > 2) return std::nullopt;
    const QStringList slash = t.split(QLatin1Char('/'));
    if (slash.size() == 2) {
        const auto net = parseIpAddress(slash[0]);
        if (!net) return std::nullopt;
        const auto bits = prefixBits(slash[1], *net);
        if (!bits) return std::nullopt;
        if (*bits == maxBits(*net)) return Parsed{Parsed::Kind::Address, {}, *net, {}, 0};
        return Parsed{Parsed::Kind::Prefix, {}, masked(*net, *bits), {}, *bits};
    }
    if (slash.size() > 2) return std::nullopt;
    if (const auto a = parseIpAddress(t)) return Parsed{Parsed::Kind::Address, {}, *a, {}, 0};
    // A keyword the provider returned that we do not accept: comparable (it can never equal a planned value).
    if (!keywordsFromAcceptedListOnly && isWord(t)) return Parsed{Parsed::Kind::Keyword, t, {}, {}, 0};
    return std::nullopt;
}

QString keyOf(const Parsed& p)
{
    switch (p.kind) {
    case Parsed::Kind::Keyword: return QStringLiteral("K:") + p.keyword.toLower();
    case Parsed::Kind::Address: return QStringLiteral("A:") + toString(p.a);
    case Parsed::Kind::Prefix: return QStringLiteral("P:%1/%2").arg(toString(p.a)).arg(p.bits);
    case Parsed::Kind::Range: return QStringLiteral("R:%1-%2").arg(toString(p.a), toString(p.b));
    }
    return {};
}

const Value* prop(const Props& p, const char* name)
{
    const auto it = p.constFind(QString::fromLatin1(name));
    return it == p.constEnd() ? nullptr : &*it;
}

bool intIs(const Props& p, const char* name, qint64 want)
{
    const Value* v = prop(p, name);
    return v && v->kind == Value::Kind::Int && v->i == want;
}

QString str(const Props& p, const char* name)
{
    const Value* v = prop(p, name);
    return v && v->kind == Value::Kind::String ? v->s : QString();
}

// A list property: (values, isNull). A single string reads as a one-element list.
std::pair<QStringList, bool> list(const Props& p, const char* name)
{
    const Value* v = prop(p, name);
    if (!v || v->kind == Value::Kind::Null) return {{}, true};
    if (v->kind == Value::Kind::String) return {{v->s}, false};
    if (v->kind == Value::Kind::Strings) return {v->list, false};
    return {{QStringLiteral("\x01not-a-string")}, false};
}

bool isAny(const Props& p, const char* name)
{
    const auto [values, isNull] = list(p, name);
    if (isNull || values.isEmpty()) return true;
    return values.size() == 1 && equalsOrdinalIgnoreCase(values.front().trimmed(), QStringLiteral("Any"));
}

QString normalisedPath(QString s)
{
    s.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return s.toLower();
}

Verdict mismatch(const QString& why) { return {Check::Mismatch, why}; }

// The planned fields of one enabled rule (the plan rule `want`); empty = matches.
QString fieldProblem(const RuleRead& r, const FirewallPlan::Rule& want)
{
    const Props& rule = r.rule;
    if (!intIs(rule, "Direction", 1)) return QStringLiteral("Direction is not Inbound");
    if (!intIs(rule, "Action", 2)) return QStringLiteral("Action is not Allow");
    if (!intIs(rule, "Profiles", 0)) return QStringLiteral("Profiles is not Any");
    if (!intIs(rule, "EdgeTraversalPolicy", 0)) return QStringLiteral("EdgeTraversalPolicy is not Block");
    if (!intIs(rule, "PolicyStoreSourceType", 1)) return QStringLiteral("the rule does not come from the local store");

    const Props& port = r.port.front();
    const QString protocol = str(port, "Protocol");
    if (!equalsOrdinalIgnoreCase(protocol, QStringLiteral("TCP")) && protocol != QLatin1String("6"))
        return QStringLiteral("Protocol is not TCP");
    const auto [localPorts, lpNull] = list(port, "LocalPort");
    if (lpNull || localPorts != want.localPorts) return QStringLiteral("LocalPort differs");
    if (!isAny(port, "RemotePort")) return QStringLiteral("RemotePort is not Any");

    const Props& address = r.address.front();
    if (!isAny(address, "LocalAddress")) return QStringLiteral("LocalAddress is not Any");
    const auto [remote, raNull] = list(address, "RemoteAddress");
    const auto haveSet = addressSet(remote, raNull);
    const auto wantValues = writerValues(want.remoteAddresses);
    const auto wantSet = wantValues ? addressSet(*wantValues, false) : std::nullopt;
    if (!haveSet || !wantSet || *haveSet != *wantSet) return QStringLiteral("RemoteAddress differs");

    const QString app = str(r.application.front(), "AppPath");
    if (app.isEmpty() || normalisedPath(app) != normalisedPath(want.program)) return QStringLiteral("AppPath differs");

    if (!intIs(r.security.front(), "OverrideBlockRules", 0)) return QStringLiteral("OverrideBlockRules is not False");
    return {};
}

}  // namespace

std::optional<QString> writerValue(const QString& token)
{
    const auto p = parse(token, /*keywordsFromAcceptedListOnly=*/true);
    if (!p) return std::nullopt;
    switch (p->kind) {
    case Parsed::Kind::Keyword: return p->keyword;
    case Parsed::Kind::Address: return toString(p->a);
    case Parsed::Kind::Prefix:
        // A /0 is written as the family's whole range: the WMI firewall provider stores "0.0.0.0/0" as the single
        // address 0.0.0.0 (a release activation_matrix run on Windows 11 25H2 read back RemoteAddress=[0.0.0.0]), which
        // admits nobody and fails the service's own check (exit 7). The comparator reads the range back as the same /0.
        if (p->bits == 0) return QStringLiteral("%1-%2").arg(toString(p->a), toString(allOnes(p->a)));
        return QStringLiteral("%1/%2").arg(toString(p->a)).arg(p->bits);
    case Parsed::Kind::Range: return QStringLiteral("%1-%2").arg(toString(p->a), toString(p->b));
    }
    return std::nullopt;
}

std::optional<QStringList> writerValues(const QStringList& tokens)
{
    QStringList out;
    for (const QString& t : tokens) {
        const auto v = writerValue(t);
        if (!v) return std::nullopt;
        out << *v;
    }
    if (out.isEmpty()) return std::nullopt;   // a plan rule always has at least ["Any"]
    return out;
}

std::optional<QString> addressKey(const QString& value)
{
    const auto p = parse(value, /*keywordsFromAcceptedListOnly=*/false);
    if (!p) return std::nullopt;
    return keyOf(*p);
}

std::optional<QStringList> addressSet(const QStringList& values, bool isNull)
{
    const QStringList any{QStringLiteral("K:any")};
    if (isNull || values.isEmpty()) return any;
    QStringList keys;
    for (const QString& v : values) {
        const auto k = addressKey(v);
        if (!k) return std::nullopt;
        if (*k == any.front()) return any;   // Any admits everything, whatever else is listed
        if (!keys.contains(*k)) keys << *k;
    }
    keys.sort();
    return keys;
}

Verdict compare(const QList<RuleRead>& group, const FirewallPlan::Plan& plan)
{
    // A complete read: exactly one filter of each kind per rule (INV-117), disabled rules included.
    for (const RuleRead& r : group) {
        const QString id = str(r.rule, "InstanceID");
        if (r.port.size() != 1 || r.address.size() != 1 || r.application.size() != 1 || r.security.size() != 1)
            return {Check::Failed, QStringLiteral("rule '%1' does not have exactly one port, address, application and "
                                                  "security filter")
                                       .arg(id)};
    }
    QList<bool> seen(plan.rules.size(), false);
    for (const RuleRead& r : group) {
        const QString id = str(r.rule, "InstanceID");
        if (intIs(r.rule, "Enabled", 2)) continue;   // disabled: no exposure (the Applier's sweep deletes it)
        if (!intIs(r.rule, "Enabled", 1)) return mismatch(QStringLiteral("rule '%1' is not plainly enabled or disabled").arg(id));
        const auto name = FirewallPlan::parseRuleName(id);
        if (!name) return mismatch(QStringLiteral("enabled rule '%1' in the reserved group is not one of ours").arg(id));
        if (name->generation != plan.generation.id)
            return mismatch(QStringLiteral("enabled rule '%1' is of another generation").arg(id));
        const auto want = std::find_if(plan.rules.cbegin(), plan.rules.cend(),
                                       [&](const FirewallPlan::Rule& p) { return p.index == name->index; });
        if (want == plan.rules.cend()) return mismatch(QStringLiteral("enabled rule '%1' is not planned").arg(id));
        const qsizetype at = want - plan.rules.cbegin();
        if (seen[at]) return mismatch(QStringLiteral("more than one enabled rule has index %1").arg(name->index));
        seen[at] = true;
        if (want->instanceId != id) return mismatch(QStringLiteral("rule '%1' is not named as planned").arg(id));
        const QString problem = fieldProblem(r, *want);
        if (!problem.isEmpty()) return mismatch(QStringLiteral("rule '%1': %2").arg(id, problem));
    }
    for (qsizetype i = 0; i < plan.rules.size(); ++i)
        if (!seen[i]) return mismatch(QStringLiteral("planned rule '%1' is missing or disabled").arg(plan.rules[i].instanceId));
    return {Check::Match, {}};
}

}  // namespace asb::win::FirewallCompare
