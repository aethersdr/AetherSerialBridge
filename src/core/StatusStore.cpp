#include "core/StatusStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QVariant>

#include <array>

namespace asb {
namespace {

constexpr std::array<const char*, 4> kStates = {"Stopped", "Listening", "Connected", "Error"};

QJsonValue optString(const std::optional<QString>& s)
{
    return s ? QJsonValue(*s) : QJsonValue(QJsonValue::Null);
}

QJsonValue time(const QDateTime& t)
{
    return t.isValid() ? QJsonValue(t.toString(Qt::ISODateWithMs)) : QJsonValue(QJsonValue::Null);
}

struct Reader {
    QString* error;
    bool ok = true;
    void fail(const QString& what)
    {
        if (ok && error) *error = what;
        ok = false;
    }
    QString string(const QJsonObject& o, const char* k)
    {
        const QJsonValue v = o.value(QLatin1String(k));
        if (v.isUndefined() || v.isNull()) return {};
        if (!v.isString()) fail(QStringLiteral("%1: expected a string").arg(QLatin1String(k)));
        return v.toString();
    }
    std::optional<QString> optString(const QJsonObject& o, const char* k)
    {
        const QJsonValue v = o.value(QLatin1String(k));
        if (v.isUndefined() || v.isNull()) return std::nullopt;
        if (!v.isString()) fail(QStringLiteral("%1: expected a string").arg(QLatin1String(k)));
        return v.toString();
    }
    qint64 integer(const QJsonObject& o, const char* k)
    {
        const QJsonValue v = o.value(QLatin1String(k));
        if (v.isUndefined() || v.isNull()) return 0;
        if (!v.isDouble() || v.toVariant().typeId() != QMetaType::LongLong) {
            fail(QStringLiteral("%1: expected an integer").arg(QLatin1String(k)));
            return 0;
        }
        return v.toInteger();
    }
    QDateTime when(const QJsonObject& o, const char* k)
    {
        const QString s = string(o, k);
        if (s.isEmpty()) return {};
        QDateTime t = QDateTime::fromString(s, Qt::ISODateWithMs);
        if (!t.isValid()) fail(QStringLiteral("%1: not an ISO 8601 time").arg(QLatin1String(k)));
        return t;
    }
};

}  // namespace

QString enumName(BridgeState s)
{
    const int i = int(s);
    return (i >= 0 && size_t(i) < kStates.size()) ? QString::fromLatin1(kStates[size_t(i)]) : QString();
}

std::optional<BridgeState> bridgeStateFromName(const QString& s)
{
    for (size_t i = 0; i < kStates.size(); ++i)
        if (s == QLatin1String(kStates[i])) return BridgeState(int(i));
    return std::nullopt;
}

namespace StatusStore {

QByteArray toJson(const StatusReport& r)
{
    QJsonArray ports;
    for (const PortStatus& p : r.ports) {
        ports.append(QJsonObject{
            {QStringLiteral("name"), p.name},
            {QStringLiteral("comPort"), p.comPort},
            {QStringLiteral("tcpPort"), p.tcpPort},
            {QStringLiteral("bindAddress"), p.bindAddress},
            {QStringLiteral("state"), enumName(p.state)},
            {QStringLiteral("client"), optString(p.client)},
            {QStringLiteral("bytesToSerial"), p.bytesToSerial},
            {QStringLiteral("bytesToClient"), p.bytesToClient},
            {QStringLiteral("lastError"), optString(p.lastError)},
            {QStringLiteral("since"), time(p.since)},
        });
    }
    const QJsonObject o{
        {QStringLiteral("updated"), time(r.updated)},
        {QStringLiteral("startedAt"), time(r.startedAt)},
        {QStringLiteral("pid"), r.pid},
        {QStringLiteral("instanceId"), QString::fromLatin1(r.instanceId)},
        {QStringLiteral("startToken"), QString::fromLatin1(r.startToken)},
        {QStringLiteral("configSha256"), r.configSha256},
        {QStringLiteral("configError"), optString(r.configError)},
        {QStringLiteral("ports"), ports},
    };
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

std::optional<StatusReport> fromJson(const QByteArray& bytes, QString* error)
{
    if (bytes.size() > kStatusMaxBytes) {
        if (error) *error = QStringLiteral("status.json is larger than %1 bytes").arg(kStatusMaxBytes);
        return std::nullopt;
    }
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QStringLiteral("status.json is not a JSON object");
        return std::nullopt;
    }
    Reader rd{error};
    const QJsonObject o = doc.object();
    StatusReport r;
    r.updated = rd.when(o, "updated");
    r.startedAt = rd.when(o, "startedAt");
    r.pid = rd.integer(o, "pid");
    r.instanceId = rd.string(o, "instanceId").toLatin1();
    r.startToken = rd.string(o, "startToken").toLatin1();
    r.configSha256 = rd.string(o, "configSha256");
    r.configError = rd.optString(o, "configError");
    const QJsonValue ports = o.value(QLatin1String("ports"));
    if (!ports.isUndefined() && !ports.isNull() && !ports.isArray()) rd.fail(QStringLiteral("ports: expected an array"));
    for (const QJsonValue& v : ports.toArray()) {
        if (!v.isObject()) {
            rd.fail(QStringLiteral("ports: expected objects"));
            break;
        }
        const QJsonObject po = v.toObject();
        PortStatus p;
        p.name = rd.string(po, "name");
        p.comPort = rd.string(po, "comPort");
        p.bindAddress = rd.string(po, "bindAddress");
        p.tcpPort = int(rd.integer(po, "tcpPort"));
        const auto state = bridgeStateFromName(rd.string(po, "state"));
        if (!state) rd.fail(QStringLiteral("state: unknown"));
        p.state = state.value_or(BridgeState::Stopped);
        p.client = rd.optString(po, "client");
        p.lastError = rd.optString(po, "lastError");
        p.bytesToSerial = rd.integer(po, "bytesToSerial");
        p.bytesToClient = rd.integer(po, "bytesToClient");
        p.since = rd.when(po, "since");
        r.ports.append(p);
    }
    if (!rd.ok) return std::nullopt;
    return r;
}

WriteResult write(FileIo& io, const DirHandle& dir, const StatusReport& r, const NameGen& gen)
{
    return atomicWrite(io, dir, QString::fromLatin1(kFileName), toJson(r), OwnerPolicy::Runtime, gen);
}

QByteArray newInstanceId()
{
    quint32 words[4];
    QRandomGenerator::system()->fillRange(words);
    QByteArray hex;
    for (quint32 w : words) hex += QByteArray::number(w, 16).rightJustified(8, '0');
    return hex;
}

}  // namespace StatusStore
}  // namespace asb
