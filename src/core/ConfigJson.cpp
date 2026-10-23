#include "core/ConfigJson.h"

#include <QCryptographicHash>
#include <QStringDecoder>

#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace asb {
namespace {

// ---------------------------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------------------------
void appendString(QByteArray& out, const QString& s)
{
    out += '"';
    for (qsizetype i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        const char16_t u = c.unicode();
        switch (u) {
        case u'"': out += "\\\""; continue;
        case u'\\': out += "\\\\"; continue;
        case u'\b': out += "\\b"; continue;
        case u'\f': out += "\\f"; continue;
        case u'\n': out += "\\n"; continue;
        case u'\r': out += "\\r"; continue;
        case u'\t': out += "\\t"; continue;
        default: break;
        }
        const bool pair = c.isHighSurrogate() && i + 1 < s.size() && s.at(i + 1).isLowSurrogate();
        if (pair) {
            out += QStringView(s).mid(i, 2).toUtf8();
            ++i;
        } else if (u < 0x20 || c.isSurrogate()) {
            // Control characters, and a lone surrogate (not encodable in UTF-8), as \u escapes so the
            // bytes round-trip exactly.
            out += QStringLiteral("\\u%1").arg(uint(u), 4, 16, QLatin1Char('0')).toLatin1();
        } else {
            out += QStringView(s).mid(i, 1).toUtf8();
        }
    }
    out += '"';
}

template <typename E>
void appendEnum(QByteArray& out, E v)
{
    const QString name = enumName(v);
    if (name.isEmpty()) out += QByteArray::number(int(v));   // what JsonStringEnumConverter writes for it
    else appendString(out, name);
}

// ---------------------------------------------------------------------------------------------
// Reader
//
// A small strict JSON parser (RFC 8259: no comments, no trailing commas, nesting at most 64) that keeps
// each number's literal text. Qt's parser cannot be used here: it stores 38400.0 and 3.84e4 as integers,
// and an Int32 field must refuse both.
//
// String contents are decoded byte-exactly: the only BOM handled is the document's (fromJson), and a
// U+FEFF inside a string, literal or escaped, stays in the value. The tokenizer refuses raw control characters and bad escapes anywhere, but invalid
// UTF-8 and lone surrogates only matter when a string is READ: such a value is refused where it is read
// (Reader), an unknown member's value is ignored, a property name holding invalid UTF-8 matches no field,
// and a property name holding a lone surrogate is refused in the objects the reader reads (the root and a port).
// ---------------------------------------------------------------------------------------------
struct Text {
    QString string;
    bool loneSurrogate = false;   // an escaped surrogate without its partner
    bool invalidUtf8 = false;     // undecodable bytes (never replaced silently: the value is unreadable)
};

struct Member;
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    QByteArray number;                                        // the literal text
    Text text;
    std::vector<Json> array;
    std::vector<Member> object;                               // in document order (duplicates kept; last wins)
};
struct Member {
    Text key;
    Json value;
};

class Parser {
public:
    explicit Parser(const QByteArray& b) : m_b(b) {}

    std::optional<Json> document(QString* error)
    {
        Json v;
        skipWs();
        if (!value(v, 0)) return failed(error);
        skipWs();
        if (m_i != m_b.size()) {
            m_err = QStringLiteral("unexpected data after the value");
            return failed(error);
        }
        return v;
    }

private:
    static constexpr int kMaxDepth = 64;

    std::optional<Json> failed(QString* error)
    {
        if (error) *error = QStringLiteral("not valid JSON (%1 at offset %2)").arg(m_err).arg(m_i);
        return std::nullopt;
    }
    bool fail(const char* what)
    {
        if (m_err.isEmpty()) m_err = QString::fromLatin1(what);
        return false;
    }
    char peek() const { return m_i < m_b.size() ? m_b[m_i] : '\0'; }
    bool eof() const { return m_i >= m_b.size(); }
    void skipWs()
    {
        while (!eof() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) ++m_i;
    }
    bool literal(const char* word)
    {
        const qsizetype n = qsizetype(qstrlen(word));
        if (m_b.mid(m_i, n) != word) return fail("invalid literal");
        m_i += n;
        return true;
    }

    bool value(Json& v, int depth)
    {
        if (eof()) return fail("unexpected end of data");
        switch (peek()) {
        case '{': return object(v, depth + 1);
        case '[': return array(v, depth + 1);
        case '"': v.type = Json::Type::String; return string(v.text);
        case 't': v.type = Json::Type::Bool; v.boolean = true; return literal("true");
        case 'f': v.type = Json::Type::Bool; v.boolean = false; return literal("false");
        case 'n': v.type = Json::Type::Null; return literal("null");
        default: v.type = Json::Type::Number; return number(v.number);
        }
    }

    bool object(Json& v, int depth)
    {
        if (depth > kMaxDepth) return fail("nested too deeply");
        v.type = Json::Type::Object;
        ++m_i;   // {
        skipWs();
        if (peek() == '}') {
            ++m_i;
            return true;
        }
        for (;;) {
            skipWs();
            if (peek() != '"') return fail("expected a property name");
            Text key;
            if (!string(key)) return false;
            skipWs();
            if (peek() != ':') return fail("expected ':'");
            ++m_i;
            skipWs();
            Json member;
            if (!value(member, depth)) return false;
            v.object.push_back(Member{std::move(key), std::move(member)});
            skipWs();
            if (peek() == ',') {
                ++m_i;
                continue;
            }
            if (peek() == '}') {
                ++m_i;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool array(Json& v, int depth)
    {
        if (depth > kMaxDepth) return fail("nested too deeply");
        v.type = Json::Type::Array;
        ++m_i;   // [
        skipWs();
        if (peek() == ']') {
            ++m_i;
            return true;
        }
        for (;;) {
            skipWs();
            Json element;
            if (!value(element, depth)) return false;
            v.array.push_back(std::move(element));
            skipWs();
            if (peek() == ',') {
                ++m_i;
                continue;
            }
            if (peek() == ']') {
                ++m_i;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    static int hex(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool unicodeEscape(char16_t* out)
    {
        if (m_i + 4 > m_b.size()) return fail("truncated \\u escape");
        int x = 0;
        for (int k = 0; k < 4; ++k) {
            const int h = hex(m_b[m_i + k]);
            if (h < 0) return fail("invalid \\u escape");
            x = x * 16 + h;
        }
        m_i += 4;
        *out = char16_t(x);
        return true;
    }

    bool string(Text& t)
    {
        QString& out = t.string;
        ++m_i;   // opening quote
        QByteArray raw;   // UTF-8 run between escapes
        const auto flushRaw = [&]() -> bool {
            if (raw.isEmpty()) return true;
            // ConvertInitialBom: a run that starts with EF BB BF (the start of the string, or the bytes
            // after an escape) keeps its U+FEFF. Without it the decoder drops it, and "\xEF\xBB\xBF" alone
            // would read as "" (an unrestricted allow-list) and "\xEF\xBB\xBFallowedClients" as a known name.
            QStringDecoder utf8(QStringDecoder::Utf8,
                                QStringDecoder::Flag::Stateless | QStringDecoder::Flag::ConvertInitialBom);
            out += utf8(raw);
            if (utf8.hasError()) t.invalidUtf8 = true;
            raw.clear();
            return true;
        };
        for (;;) {
            if (eof()) return fail("unterminated string");
            const char c = m_b[m_i];
            if (c == '"') {
                ++m_i;
                return flushRaw();
            }
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in a string");
            if (c != '\\') {
                raw += c;
                ++m_i;
                continue;
            }
            if (!flushRaw()) return false;
            ++m_i;
            if (eof()) return fail("unterminated string");
            const char e = m_b[m_i++];
            switch (e) {
            case '"': out += QLatin1Char('"'); break;
            case '\\': out += QLatin1Char('\\'); break;
            case '/': out += QLatin1Char('/'); break;
            case 'b': out += QLatin1Char('\b'); break;
            case 'f': out += QLatin1Char('\f'); break;
            case 'n': out += QLatin1Char('\n'); break;
            case 'r': out += QLatin1Char('\r'); break;
            case 't': out += QLatin1Char('\t'); break;
            case 'u': {
                char16_t u = 0;
                if (!unicodeEscape(&u)) return false;
                if (QChar::isHighSurrogate(u) && m_b.mid(m_i, 2) == "\\u") {
                    const qsizetype save = m_i;
                    m_i += 2;
                    char16_t lo = 0;
                    if (!unicodeEscape(&lo)) return false;
                    if (QChar::isLowSurrogate(lo)) {
                        out += QChar(u);
                        out += QChar(lo);
                        break;
                    }
                    m_i = save;   // not a pair: the next escape is read on its own
                }
                if (QChar::isSurrogate(u)) t.loneSurrogate = true;
                out += QChar(u);
                break;
            }
            default: return fail("invalid escape");
            }
        }
    }

    bool number(QByteArray& out)
    {
        const qsizetype start = m_i;
        const auto digit = [&]() { return !eof() && peek() >= '0' && peek() <= '9'; };
        if (peek() == '-') ++m_i;
        if (!digit()) return fail("invalid value");
        if (peek() == '0') ++m_i;
        else
            while (digit()) ++m_i;
        if (peek() == '.') {
            ++m_i;
            if (!digit()) return fail("invalid number");
            while (digit()) ++m_i;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++m_i;
            if (peek() == '+' || peek() == '-') ++m_i;
            if (!digit()) return fail("invalid number");
            while (digit()) ++m_i;
        }
        out = m_b.mid(start, m_i - start);
        return true;
    }

    const QByteArray& m_b;
    qsizetype m_i = 0;
    QString m_err;
};

struct Reader {
    QString* error;
    bool fail(const QString& where, const QString& what)
    {
        if (error) *error = where + QStringLiteral(": ") + what;
        return false;
    }

    // An Int32 is read only from a number written as an integer: no fraction, no exponent, in range.
    static std::optional<int> int32(const Json& v)
    {
        if (v.type != Json::Type::Number) return std::nullopt;
        if (v.number.contains('.') || v.number.contains('e') || v.number.contains('E')) return std::nullopt;
        bool ok = false;
        const qlonglong x = v.number.toLongLong(&ok);
        if (!ok || x < std::numeric_limits<int>::min() || x > std::numeric_limits<int>::max()) return std::nullopt;
        return int(x);
    }

    bool readInt(const Json& v, const QString& where, int* out)
    {
        const auto x = int32(v);
        if (!x) return fail(where, QStringLiteral("expected an integer (Int32)"));
        *out = *x;
        return true;
    }

    bool readBool(const Json& v, const QString& where, bool* out)
    {
        if (v.type != Json::Type::Bool) return fail(where, QStringLiteral("expected true or false"));
        *out = v.boolean;
        return true;
    }

    // Why a string that is read cannot be used, or an empty string.
    static QString unreadable(const Text& t)
    {
        if (t.invalidUtf8) return QStringLiteral("invalid UTF-8");
        if (t.loneSurrogate) return QStringLiteral("invalid UTF-16 (a lone surrogate)");
        return QString();
    }

    // A string, or null (read as "").
    bool readString(const Json& v, const QString& where, QString* out)
    {
        if (v.type == Json::Type::Null) {
            *out = QString();
            return true;
        }
        if (v.type != Json::Type::String) return fail(where, QStringLiteral("expected a string"));
        if (const QString why = unreadable(v.text); !why.isEmpty()) return fail(where, why);
        *out = v.text.string;
        return true;
    }

    // A member of an object the reader reads (the root, a port): such a name is unescaped, which fails on a
    // lone surrogate; a name holding invalid UTF-8 is compared as bytes and matches nothing.
    // Returns false (refused) or true with *known = whether the name can match a field.
    bool readName(const Member& m, const QString& where, bool* known)
    {
        if (m.key.loneSurrogate) return fail(where, QStringLiteral("a property name holds invalid UTF-16 (a lone surrogate)"));
        *known = !m.key.invalidUtf8;
        return true;
    }

    // JsonStringEnumConverter (default options): a number token is taken as the Int32 value; a string
    // goes through Enum.TryParse(ignoreCase: true), which trims white space, reads a leading digit or
    // sign as an integer, and otherwise ORs a comma-separated list of names.
    template <typename E>
    bool readEnum(const Json& v, const QString& where, std::optional<E> (*fromName)(const QString&), E* out)
    {
        if (v.type == Json::Type::String && unreadable(v.text).isEmpty()) {
            if (const auto e = parseEnumString<E>(v.text.string, fromName)) {
                *out = *e;
                return true;
            }
            return fail(where, QStringLiteral("unknown name '%1'").arg(v.text.string));
        }
        if (const auto i = int32(v)) {
            *out = E(*i);   // any Int32; validate() reports undefined values
            return true;
        }
        return fail(where, QStringLiteral("expected a name or an integer"));
    }

    template <typename E>
    static std::optional<E> parseEnumString(const QString& raw, std::optional<E> (*fromName)(const QString&))
    {
        const QString s = raw.trimmed();
        if (s.isEmpty()) return std::nullopt;
        const QChar c = s.front();
        if (c.isDigit() || c == QLatin1Char('-') || c == QLatin1Char('+')) {
            bool ok = false;
            const int n = s.toInt(&ok, 10);
            return ok ? std::optional<E>(E(n)) : std::nullopt;
        }
        int value = 0;
        for (const QString& part : s.split(QLatin1Char(','))) {
            const auto e = fromName(part.trimmed());
            if (!e) return std::nullopt;
            value |= int(*e);
        }
        return E(value);
    }

    bool readPort(const Json& v, const QString& at, PortMapping* p)
    {
        if (v.type != Json::Type::Object) return fail(at, QStringLiteral("expected an object"));
        const auto field = [&](const char* name) { return at + QLatin1Char('.') + QLatin1String(name); };
        // Exact names only (no case folding).
        for (const Member& m : v.object) {
            bool known = false;
            if (!readName(m, at, &known)) return false;
            if (!known) continue;
            const QString& k = m.key.string;
            const Json& x = m.value;
            bool ok = true;
            if (k == QLatin1String("enabled")) ok = readBool(x, field("enabled"), &p->enabled);
            else if (k == QLatin1String("name")) ok = readString(x, field("name"), &p->name);
            else if (k == QLatin1String("comPort")) ok = readString(x, field("comPort"), &p->comPort);
            else if (k == QLatin1String("baudRate")) ok = readInt(x, field("baudRate"), &p->baudRate);
            else if (k == QLatin1String("dataBits")) ok = readInt(x, field("dataBits"), &p->dataBits);
            else if (k == QLatin1String("parity")) ok = readEnum<Parity>(x, field("parity"), parityFromName, &p->parity);
            else if (k == QLatin1String("stopBits"))
                ok = readEnum<StopBits>(x, field("stopBits"), stopBitsFromName, &p->stopBits);
            else if (k == QLatin1String("handshake"))
                ok = readEnum<Handshake>(x, field("handshake"), handshakeFromName, &p->handshake);
            else if (k == QLatin1String("dtrEnable")) ok = readBool(x, field("dtrEnable"), &p->dtrEnable);
            else if (k == QLatin1String("rtsEnable")) ok = readBool(x, field("rtsEnable"), &p->rtsEnable);
            else if (k == QLatin1String("tcpPort")) ok = readInt(x, field("tcpPort"), &p->tcpPort);
            else if (k == QLatin1String("bindAddress")) ok = readString(x, field("bindAddress"), &p->bindAddress);
            else if (k == QLatin1String("allowedClients"))
                ok = readString(x, field("allowedClients"), &p->allowedClients);
            // anything else (displayName included) is ignored
            if (!ok) return false;
        }
        return true;
    }
};

}  // namespace

QByteArray toJson(const BridgeConfig& c)
{
    QByteArray out;
    out += "{\n  \"version\": ";
    out += QByteArray::number(c.version);
    out += ",\n  \"ports\": [";
    for (qsizetype i = 0; i < c.ports.size(); ++i) {
        const PortMapping& p = c.ports[i];
        out += i == 0 ? "\n    {\n" : ",\n    {\n";
        const auto key = [&](const char* k) {
            out += "      \"";
            out += k;
            out += "\": ";
        };
        const auto boolean = [](bool b) { return b ? "true" : "false"; };
        key("enabled"); out += boolean(p.enabled); out += ",\n";
        key("name"); appendString(out, p.name); out += ",\n";
        key("comPort"); appendString(out, p.comPort); out += ",\n";
        key("baudRate"); out += QByteArray::number(p.baudRate); out += ",\n";
        key("dataBits"); out += QByteArray::number(p.dataBits); out += ",\n";
        key("parity"); appendEnum(out, p.parity); out += ",\n";
        key("stopBits"); appendEnum(out, p.stopBits); out += ",\n";
        key("handshake"); appendEnum(out, p.handshake); out += ",\n";
        key("dtrEnable"); out += boolean(p.dtrEnable); out += ",\n";
        key("rtsEnable"); out += boolean(p.rtsEnable); out += ",\n";
        key("tcpPort"); out += QByteArray::number(p.tcpPort); out += ",\n";
        key("bindAddress"); appendString(out, p.bindAddress); out += ",\n";
        key("allowedClients"); appendString(out, p.allowedClients); out += "\n";
        out += "    }";
    }
    out += c.ports.isEmpty() ? "]\n}" : "\n  ]\n}";
    return out;
}

std::optional<BridgeConfig> fromJson(const QByteArray& input, QString* error)
{
    QByteArray bytes = input;
    if (bytes.startsWith("\xEF\xBB\xBF")) bytes.remove(0, 3);   // a leading BOM is skipped

    QString why;
    const std::optional<Json> doc = Parser(bytes).document(&why);
    Reader r{error};
    if (!doc) {
        r.fail(QStringLiteral("config"), why);
        return std::nullopt;
    }
    // A file holding just `null` loads as an empty config.
    if (doc->type == Json::Type::Null) return BridgeConfig{};
    if (doc->type != Json::Type::Object) {
        r.fail(QStringLiteral("config"), QStringLiteral("expected a JSON object"));
        return std::nullopt;
    }

    BridgeConfig c;
    for (const Member& m : doc->object) {
        bool known = false;
        if (!r.readName(m, QStringLiteral("config"), &known)) return std::nullopt;
        if (!known) continue;
        const QString& key = m.key.string;
        const Json& value = m.value;
        if (key == QLatin1String("version")) {
            if (!r.readInt(value, QStringLiteral("version"), &c.version)) return std::nullopt;
        } else if (key == QLatin1String("ports")) {
            // "ports": null (and a null entry) is refused here.
            if (value.type != Json::Type::Array) {
                r.fail(QStringLiteral("ports"), QStringLiteral("expected an array"));
                return std::nullopt;
            }
            c.ports.clear();   // a duplicate "ports" replaces the earlier one (last wins)
            for (size_t i = 0; i < value.array.size(); ++i) {
                PortMapping p;
                if (!r.readPort(value.array[i], QStringLiteral("ports[%1]").arg(i), &p)) return std::nullopt;
                c.ports.append(p);
            }
        }
    }
    return c;
}

QString sha256Hex(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

}  // namespace asb
