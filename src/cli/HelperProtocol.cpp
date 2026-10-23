#include "cli/HelperProtocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>

#include <vector>

namespace asb::cli::helper {
namespace {

const char* const kOpNames[] = {"save-and-apply", "deactivate", "start", "stop", "repair", "discard-pending", "inspect",
                                "install", "uninstall", "discard-pending-if"};
constexpr Op kOps[] = {Op::SaveAndApply, Op::Deactivate, Op::Start, Op::Stop, Op::Repair, Op::DiscardPending, Op::Inspect,
                       Op::Install, Op::Uninstall, Op::DiscardPendingIf};

bool isSha256Hex(const QString& s)
{
    if (s.size() != 64) return false;
    for (const QChar c : s)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
    return true;
}

// Called only on bytes QJsonDocument accepted (a well-formed document): finds a member name given twice in one
// object, which QJsonDocument would silently resolve (the last one wins). Names are compared decoded.
bool duplicateMember(const QByteArray& b, QString* which)
{
    struct Frame {
        bool object;
        bool expectName;
        QSet<QString> names;
    };
    std::vector<Frame> stack;
    qsizetype i = 0;
    while (i < b.size()) {
        const char ch = b[i];
        if (ch == '"') {
            qsizetype j = i + 1;
            while (j < b.size() && b[j] != '"') j += b[j] == '\\' ? 2 : 1;
            if (!stack.empty() && stack.back().object && stack.back().expectName) {
                const QByteArray token = b.mid(i, j - i + 1);
                const QString name = QJsonDocument::fromJson("[" + token + "]").array().at(0).toString();
                if (stack.back().names.contains(name)) {
                    if (which) *which = name;
                    return true;
                }
                stack.back().names.insert(name);
                stack.back().expectName = false;
            }
            i = j + 1;
            continue;
        }
        if (ch == '{') stack.push_back({true, true, {}});
        else if (ch == '[') stack.push_back({false, false, {}});
        else if ((ch == '}' || ch == ']') && !stack.empty()) stack.pop_back();
        else if (ch == ',' && !stack.empty() && stack.back().object) stack.back().expectName = true;
        ++i;
    }
    return false;
}

std::optional<QJsonObject> oneObject(const QByteArray& bytes, QString* error)
{
    QJsonParseError pe{};
    const QJsonDocument d = QJsonDocument::fromJson(bytes, &pe);
    if (pe.error != QJsonParseError::NoError) {
        // GarbageAtEnd: a second value, or anything else after the first (D34: exactly one value).
        if (error) *error = QStringLiteral("not one JSON value (%1 at offset %2)").arg(pe.errorString()).arg(pe.offset);
        return std::nullopt;
    }
    if (!d.isObject()) {
        if (error) *error = QStringLiteral("not a JSON object");
        return std::nullopt;
    }
    QString dup;
    if (duplicateMember(bytes, &dup)) {
        if (error) *error = QStringLiteral("\"%1\" is given twice").arg(dup);
        return std::nullopt;
    }
    return d.object();
}

bool onlyMembers(const QJsonObject& o, std::initializer_list<const char*> allowed, QString* error)
{
    for (auto it = o.begin(); it != o.end(); ++it) {
        bool known = false;
        for (const char* a : allowed) known = known || it.key() == QLatin1String(a);
        if (!known) {
            if (error) *error = QStringLiteral("unknown member \"%1\"").arg(it.key());
            return false;
        }
    }
    return true;
}

bool fail(QString* error, const QString& why)
{
    if (error) *error = why;
    return false;
}

// configBytes, source and supersedePending into a SaveRequest.
bool saveRequest(const QJsonObject& o, SaveRequest* out, QString* error)
{
    const QJsonValue cb = o.value(QStringLiteral("configBytes"));
    if (!cb.isString()) return fail(error, QStringLiteral("configBytes must be a base64 string"));
    const QByteArray b64 = cb.toString().toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(b64, QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    // Exact: the bytes re-encode to the very text that was sent (padding, no whitespace, no stray bits).
    if (decoded.decodingStatus != QByteArray::Base64DecodingStatus::Ok || decoded.decoded.toBase64() != b64 ||
        cb.toString() != QString::fromLatin1(b64))
        return fail(error, QStringLiteral("configBytes is not exact base64"));
    out->config = decoded.decoded;

    const QJsonValue src = o.value(QStringLiteral("source"));
    if (!src.isObject()) return fail(error, QStringLiteral("source must be an object"));
    const QJsonObject s = src.toObject();
    if (!onlyMembers(s, {"kind", "sha256"}, error)) return false;
    const QString kind = s.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("pending")) out->source.kind = Source::Pending;
    else if (kind == QLatin1String("committed")) out->source.kind = Source::Committed;
    else if (kind == QLatin1String("new")) out->source.kind = Source::New;
    else return fail(error, QStringLiteral("source.kind must be pending, committed or new"));
    const QJsonValue sha = s.value(QStringLiteral("sha256"));
    if (sha.isUndefined()) {
        if (out->source.kind != Source::New) return fail(error, QStringLiteral("source.sha256 is required for %1").arg(kind));
    } else if (!sha.isString() || !isSha256Hex(sha.toString())) {
        return fail(error, QStringLiteral("source.sha256 must be 64 lower-case hex digits"));
    } else {
        out->source.sha256 = sha.toString().toLatin1();
    }

    const QJsonValue sp = o.value(QStringLiteral("supersedePending"));
    if (sp.isUndefined()) out->supersedePending = false;
    else if (sp.isBool()) out->supersedePending = sp.toBool();
    else return fail(error, QStringLiteral("supersedePending must be true or false"));
    return true;
}

QString outcomeWire(Outcome o) { return enumName(o); }

std::optional<Outcome> outcomeFrom(const QString& s)
{
    for (Outcome o : {Outcome::NothingChanged, Outcome::Uncommitted, Outcome::CommittedNotRunning, Outcome::StateUnknown,
                      Outcome::SavedNotApplied, Outcome::Ok})
        if (enumName(o) == s) return o;
    return std::nullopt;
}

std::optional<Persisted> persistedFrom(const QString& s)
{
    for (Persisted p : {Persisted::None, Persisted::Staged, Persisted::Committed})
        if (enumName(p) == s) return p;
    return std::nullopt;
}

bool optionalString(const QJsonObject& o, const char* name, QString* out, QString* error)
{
    const QJsonValue v = o.value(QLatin1String(name));
    if (v.isUndefined()) return true;
    if (!v.isString()) return fail(error, QStringLiteral("%1 must be a string").arg(QLatin1String(name)));
    *out = v.toString();
    return true;
}

bool okMember(const QJsonObject& o, bool* ok, QString* error)
{
    const QJsonValue v = o.value(QStringLiteral("ok"));
    if (!v.isBool()) return fail(error, QStringLiteral("ok must be true or false"));
    if (ok) *ok = v.toBool();
    return true;
}

}  // namespace

QString opName(Op op)
{
    for (size_t i = 0; i < std::size(kOps); ++i)
        if (kOps[i] == op) return QString::fromLatin1(kOpNames[i]);
    return {};
}

QString trustName(DirAction a)
{
    switch (a) {
    case DirAction::Create: return QStringLiteral("Create");
    case DirAction::Trust: return QStringLiteral("Trust");
    case DirAction::Recreate: return QStringLiteral("Recreate");
    case DirAction::Refuse: return QStringLiteral("Refuse");
    }
    return {};
}

QString installationName(InstallState s)
{
    switch (s) {
    case InstallState::NotInstalled: return QStringLiteral("NotInstalled");
    case InstallState::IncompleteInstall: return QStringLiteral("IncompleteInstall");
    case InstallState::Current: return QStringLiteral("Current");
    case InstallState::Foreign: return QStringLiteral("Foreign");
    }
    return {};
}

QString serviceName(ServiceQuery::State s)
{
    switch (s) {
    case ServiceQuery::Absent: return QStringLiteral("Absent");
    case ServiceQuery::Stopped: return QStringLiteral("Stopped");
    case ServiceQuery::StartPending: return QStringLiteral("StartPending");
    case ServiceQuery::Running: return QStringLiteral("Running");
    case ServiceQuery::StopPending: return QStringLiteral("StopPending");
    case ServiceQuery::Other: return QStringLiteral("Other");
    }
    return {};
}

bool succeeded(const Result& r)
{
    return r.outcome == Outcome::Ok || r.outcome == Outcome::SavedNotApplied;
}

QByteArray encodeRequest(const Request& r)
{
    QJsonObject o;
    o[QStringLiteral("op")] = opName(r.op);
    if (r.save && (r.op == Op::SaveAndApply || r.op == Op::Deactivate || r.op == Op::Install)) {
        o[QStringLiteral("configBytes")] = QString::fromLatin1(r.save->config.toBase64());
        QJsonObject s;
        s[QStringLiteral("kind")] = r.save->source.kind == Source::Pending     ? QStringLiteral("pending")
                                    : r.save->source.kind == Source::Committed ? QStringLiteral("committed")
                                                                               : QStringLiteral("new");
        if (!r.save->source.sha256.isEmpty()) s[QStringLiteral("sha256")] = QString::fromLatin1(r.save->source.sha256);
        o[QStringLiteral("source")] = s;
        o[QStringLiteral("supersedePending")] = r.save->supersedePending;
    }
    if (r.op == Op::Uninstall) o[QStringLiteral("removeSettings")] = r.removeSettings;
    if (r.op == Op::DiscardPendingIf) o[QStringLiteral("sha256")] = r.sha256;
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

std::optional<Request> parseRequest(const QByteArray& bytes, QString* error)
{
    const std::optional<QJsonObject> o = oneObject(bytes, error);
    if (!o) return std::nullopt;
    const QJsonValue opv = o->value(QStringLiteral("op"));
    if (!opv.isString()) {
        fail(error, QStringLiteral("op must be a string"));
        return std::nullopt;
    }
    Request r;
    bool known = false;
    for (size_t i = 0; i < std::size(kOps); ++i)
        if (opv.toString() == QLatin1String(kOpNames[i])) {
            r.op = kOps[i];
            known = true;
        }
    if (!known) {
        fail(error, QStringLiteral("unknown op \"%1\"").arg(opv.toString()));
        return std::nullopt;
    }
    switch (r.op) {
    case Op::SaveAndApply: {
        if (!onlyMembers(*o, {"op", "configBytes", "source", "supersedePending"}, error)) return std::nullopt;
        SaveRequest s;
        if (!saveRequest(*o, &s, error)) return std::nullopt;
        r.save = s;
        break;
    }
    case Op::Deactivate:
    case Op::Install:
        if (!onlyMembers(*o, {"op", "configBytes", "source", "supersedePending"}, error)) return std::nullopt;
        if (o->contains(QStringLiteral("configBytes"))) {
            SaveRequest s;
            if (!saveRequest(*o, &s, error)) return std::nullopt;
            r.save = s;
        } else if (o->contains(QStringLiteral("source")) || o->contains(QStringLiteral("supersedePending"))) {
            fail(error, QStringLiteral("source and supersedePending go with configBytes"));
            return std::nullopt;
        }
        break;
    case Op::Uninstall: {
        if (!onlyMembers(*o, {"op", "removeSettings"}, error)) return std::nullopt;
        const QJsonValue v = o->value(QStringLiteral("removeSettings"));
        if (!v.isBool()) {
            fail(error, QStringLiteral("removeSettings must be true or false"));
            return std::nullopt;
        }
        r.removeSettings = v.toBool();
        break;
    }
    case Op::DiscardPendingIf: {
        if (!onlyMembers(*o, {"op", "sha256"}, error)) return std::nullopt;
        const QJsonValue v = o->value(QStringLiteral("sha256"));
        if (!v.isString() || !isSha256Hex(v.toString())) {
            fail(error, QStringLiteral("sha256 must be 64 lower-case hex digits"));
            return std::nullopt;
        }
        r.sha256 = v.toString();
        break;
    }
    default:
        if (!onlyMembers(*o, {"op"}, error)) return std::nullopt;
        break;
    }
    return r;
}

QByteArray encodeResult(const Result& r)
{
    QJsonObject o;
    o[QStringLiteral("ok")] = succeeded(r);
    o[QStringLiteral("outcome")] = outcomeWire(r.outcome);
    o[QStringLiteral("persisted")] = enumName(r.persisted);
    if (r.persisted != Persisted::None && !r.persistedSha256.isEmpty())
        o[QStringLiteral("persistedSha256")] = QString::fromLatin1(r.persistedSha256);
    if (!r.step.isEmpty()) o[QStringLiteral("step")] = r.step;
    o[QStringLiteral("message")] = r.message;
    if (!r.postcondition.isEmpty()) o[QStringLiteral("postcondition")] = r.postcondition;
    o[QStringLiteral("fileChanges")] = QJsonArray::fromStringList(r.fileChanges);
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

QByteArray encodeInspect(const InspectReport& i, bool ok)
{
    QJsonObject o;
    o[QStringLiteral("ok")] = ok;
    o[QStringLiteral("trust")] = i.trust;
    o[QStringLiteral("reason")] = i.reason;
    o[QStringLiteral("installation")] = i.installation;
    o[QStringLiteral("service")] = i.service;
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

std::optional<Result> parseResultResponse(const QByteArray& bytes, bool* ok, QString* error)
{
    const std::optional<QJsonObject> o = oneObject(bytes, error);
    if (!o || !okMember(*o, ok, error)) return std::nullopt;
    Result r;
    const std::optional<Outcome> outcome = outcomeFrom(o->value(QStringLiteral("outcome")).toString());
    const std::optional<Persisted> persisted = persistedFrom(o->value(QStringLiteral("persisted")).toString());
    if (!outcome || !persisted) {
        fail(error, QStringLiteral("outcome or persisted is missing or unknown"));
        return std::nullopt;
    }
    r.outcome = *outcome;
    r.persisted = *persisted;
    QString sha;
    if (!optionalString(*o, "persistedSha256", &sha, error) || !optionalString(*o, "step", &r.step, error) ||
        !optionalString(*o, "postcondition", &r.postcondition, error))
        return std::nullopt;
    if (!sha.isEmpty() && !isSha256Hex(sha)) {
        fail(error, QStringLiteral("persistedSha256 is not a sha256"));
        return std::nullopt;
    }
    r.persistedSha256 = sha.toLatin1();
    const QJsonValue msg = o->value(QStringLiteral("message"));
    const QJsonValue changes = o->value(QStringLiteral("fileChanges"));
    if (!msg.isString() || !changes.isArray()) {
        fail(error, QStringLiteral("message or fileChanges is missing"));
        return std::nullopt;
    }
    r.message = msg.toString();
    for (const QJsonValue& v : changes.toArray()) {
        if (!v.isString()) {
            fail(error, QStringLiteral("fileChanges must hold strings"));
            return std::nullopt;
        }
        r.fileChanges << v.toString();
    }
    return r;
}

std::optional<InspectReport> parseInspectResponse(const QByteArray& bytes, bool* ok, QString* error)
{
    const std::optional<QJsonObject> o = oneObject(bytes, error);
    if (!o || !okMember(*o, ok, error)) return std::nullopt;
    InspectReport i;
    for (auto [name, field] : {std::pair{"trust", &i.trust}, std::pair{"reason", &i.reason},
                               std::pair{"installation", &i.installation}, std::pair{"service", &i.service}}) {
        const QJsonValue v = o->value(QLatin1String(name));
        if (!v.isString()) {
            fail(error, QStringLiteral("%1 is missing").arg(QLatin1String(name)));
            return std::nullopt;
        }
        *field = v.toString();
    }
    return i;
}

}  // namespace asb::cli::helper
