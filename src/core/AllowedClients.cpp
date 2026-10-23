#include "core/AllowedClients.h"

#include "core/BridgeConfig.h"
#include "core/FirewallKeywords.h"

#include <QRegularExpression>

#include <algorithm>

namespace asb::AllowedClients {
namespace {

bool isNullOrWhiteSpace(const QString& s)
{
    return std::all_of(s.cbegin(), s.cend(), [](QChar c) { return c.isSpace(); });
}

// Keywords match with ordinal, ASCII-only case folding (equalsOrdinalIgnoreCase).
bool matchesKeyword(const QString& token, std::span<const std::string_view> set)
{
    return std::any_of(set.begin(), set.end(), [&](std::string_view k) {
        return equalsOrdinalIgnoreCase(token, QString::fromLatin1(k.data(), qsizetype(k.size())));
    });
}

// The one tokeniser (blank is no entries; else split on ',' and trim each). check() and tokens() both use it.
QStringList split(const QString& clients)
{
    QStringList out;
    if (isNullOrWhiteSpace(clients)) return out;
    for (const QString& raw : clients.split(QLatin1Char(','))) out.append(raw.trimmed());
    return out;
}

QString notAnAddress(const QString& t)
{
    return QStringLiteral("allowed client '%1' is not an IP address, CIDR block (10.0.0.0/8), address range "
                          "(a-b) or a Windows keyword such as LocalSubnet.")
        .arg(t);
}

int compare(const IpAddress& a, const IpAddress& b)
{
    for (int i = 0; i < a.size(); ++i)
        if (a.bytes[size_t(i)] != b.bytes[size_t(i)]) return a.bytes[size_t(i)] < b.bytes[size_t(i)] ? -1 : 1;
    return 0;
}

// One token, in this order: keyword, then a-b, then a/n, then a bare address.
std::optional<QString> checkToken(const QString& t, std::span<const std::string_view> accepted,
                                  std::span<const std::string_view> known)
{
    if (t.isEmpty())
        return QStringLiteral("allowed clients has an empty entry (two commas in a row, or a trailing comma).");
    if (matchesKeyword(t, accepted)) return std::nullopt;
    // INV-30: a known keyword the provider does not accept (FirewallKeywords.h).
    if (matchesKeyword(t, known))
        return QStringLiteral("allowed client '%1' is a keyword the Windows firewall does not accept; list the "
                              "addresses instead.")
            .arg(t);

    const QStringList dash = t.split(QLatin1Char('-'));
    if (dash.size() == 2) {
        const auto a = parseIpAddress(dash[0]);
        const auto b = parseIpAddress(dash[1]);
        if (!a || !b) return notAnAddress(t);
        if (a->family != b->family)
            return QStringLiteral("allowed client '%1' is not a valid range: both ends must be IPv4 or both IPv6.")
                .arg(t);
        if (compare(*a, *b) > 0)
            return QStringLiteral("allowed client '%1' is not a valid range: the first address is after the second.")
                .arg(t);
        return std::nullopt;
    }
    if (dash.size() > 2) return notAnAddress(t);

    const QStringList slash = t.split(QLatin1Char('/'));
    if (slash.size() == 2) {
        const auto net = parseIpAddress(slash[0]);
        if (!net) return notAnAddress(t);
        const int max = net->family == IpAddress::Family::V4 ? 32 : 128;
        // The prefix is 1-3 digits; the character fence before this check admits ASCII only. `$` also
        // matches before one final newline (PCRE2), and the number parse allows the trailing white space
        // that leaves.
        static const QRegularExpression digits(QStringLiteral("^[0-9]{1,3}$"));
        const QString& prefix = slash[1];
        if (!digits.match(prefix).hasMatch() || prefix.trimmed().toInt() > max)
            return QStringLiteral("allowed client '%1' has an invalid prefix: use /0 to /%2 %3 (subnet masks are not "
                                  "accepted).")
                .arg(t)
                .arg(max)
                .arg(max == 32 ? QStringLiteral("for IPv4") : QStringLiteral("for IPv6"));
        return std::nullopt;
    }
    if (slash.size() > 2) return notAnAddress(t);

    return parseIpAddress(t) ? std::nullopt : std::optional<QString>(notAnAddress(t));
}

}  // namespace

bool passesCharacterFence(const QString& clients)
{
    // ASCII on purpose. No surrogate can match an ASCII class; refusing them first
    // keeps PCRE2 from seeing invalid UTF-16.
    static const QRegularExpression re(QStringLiteral(R"(^[0-9A-Za-z.:/\-, ]{0,500}$)"));
    return std::none_of(clients.cbegin(), clients.cend(), [](QChar c) { return c.isSurrogate(); }) &&
           re.match(clients).hasMatch();
}

QStringList check(const QString& clients, std::span<const std::string_view> accepted,
                  std::span<const std::string_view> known)
{
    QStringList problems;
    for (const QString& token : split(clients)) {
        if (auto problem = checkToken(token, accepted, known)) problems.append(*problem);
    }
    return problems;
}

QStringList check(const QString& clients)
{
    return check(clients, kAcceptedFirewallKeywords, kKnownFirewallKeywords);
}

std::optional<QStringList> tokens(const QString& clients)
{
    if (!passesCharacterFence(clients) || !check(clients).isEmpty()) return std::nullopt;
    return split(clients);
}

}  // namespace asb::AllowedClients
