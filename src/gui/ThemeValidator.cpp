#include "gui/ThemeValidator.h"

#include <cmath>

#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStringList>

// The rules, their order and their failure codes are those of tools/theme_validator_ref.py; keep
// the two in step (tst_ThemeFollower::oracleFixturesAgree fails when they drift). QJsonObject
// iterates keys in sorted order, as the oracle does, so both report the same first failure.

namespace asb::gui {

QString ThemeValidation::describe() const {
    if (ok) {
        return QStringLiteral("valid (%1 leaves, %2 gradients, %3 fonts)")
            .arg(leaves)
            .arg(gradients)
            .arg(fonts);
    }
    return QStringLiteral("%1 at %2").arg(code, path.isEmpty() ? QStringLiteral("<document>") : path);
}

namespace {

constexpr int kMaxDepth = 12;
constexpr int kMaxLeaves = 5000;
constexpr qsizetype kMaxStops = 32;
constexpr double kMaxAbsNumber = 10000.0;

struct Patterns {
    QRegularExpression key{QRegularExpression::anchoredPattern(QStringLiteral(R"([A-Za-z0-9._/-]{1,96})"))};
    QRegularExpression alias{QRegularExpression::anchoredPattern(QStringLiteral(R"(\{[A-Za-z0-9._-]{1,96}\})"))};
    QRegularExpression hex{QRegularExpression::anchoredPattern(
        QStringLiteral(R"(#(?:[0-9A-Fa-f]{3}|[0-9A-Fa-f]{6}|[0-9A-Fa-f]{8}))"))};
    QRegularExpression word{QRegularExpression::anchoredPattern(QStringLiteral(R"([a-z]+)"))};
    QRegularExpression family{QRegularExpression::anchoredPattern(QStringLiteral(R"([A-Za-z0-9 _-]{1,64})"))};
};

const Patterns& patterns() {
    static const Patterns p;
    return p;
}

bool full(const QRegularExpression& re, const QString& s) { return re.match(s).hasMatch(); }

bool finiteBounded(const QJsonValue& v) {
    if (!v.isDouble()) {
        return false;
    }
    const double d = v.toDouble();
    return std::isfinite(d) && std::fabs(d) <= kMaxAbsNumber;
}

bool isColor(const QString& s) {
    const Patterns& p = patterns();
    return full(p.alias, s) || full(p.hex, s) || (full(p.word, s) && QColor::isValidColorName(s));
}

QString join(const QString& path, const QString& key) {
    return path.isEmpty() ? key : path + QLatin1Char('.') + key;
}

struct Failure {
    QString code;
    QString path;
};

class Walker {
public:
    int leaves = 0;
    int gradients = 0;
    int fonts = 0;

    // Throws Failure.
    void document(const QJsonObject& doc) {
        static const QStringList kMetadata{QStringLiteral("name"), QStringLiteral("author"),
                                           QStringLiteral("version"), QStringLiteral("description"),
                                           QStringLiteral("schemaVersion")};
        static const QStringList kSections{QStringLiteral("tokens"), QStringLiteral("primitives"),
                                           QStringLiteral("scopes")};

        const QJsonValue sv = doc.value(QStringLiteral("schemaVersion"));
        if (!sv.isDouble() || (sv.toDouble() != 1.0 && sv.toDouble() != 2.0)) {
            throw Failure{QStringLiteral("schema"), QStringLiteral("schemaVersion")};
        }
        for (auto it = doc.begin(); it != doc.end(); ++it) {
            const QString key = it.key();
            const QJsonValue value = it.value();
            if (kMetadata.contains(key)) {
                continue;
            }
            if (key == QLatin1String("baseTheme")) {
                const QString base = value.toString();
                if (!value.isString() || (base != QLatin1String("Default Dark") &&
                                          base != QLatin1String("Default Light"))) {
                    throw Failure{QStringLiteral("base-theme"), key};
                }
            } else if (kSections.contains(key)) {
                if (!value.isObject()) {
                    throw Failure{QStringLiteral("section"), key};
                }
                group(value.toObject(), key, QStringList{key}, 1);
            } else {
                throw Failure{QStringLiteral("top-key"), key};
            }
        }
    }

private:
    // `segments`: every key from the section down, each split on '.', the way the loader
    // flattens nested groups into one dotted token name.
    void group(const QJsonObject& obj, const QString& path, const QStringList& segments, int depth) {
        if (depth > kMaxDepth) {
            throw Failure{QStringLiteral("depth"), path};
        }
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            const QString key = it.key();
            const QJsonValue value = it.value();
            const QString child = join(path, key);
            if (!full(patterns().key, key)) {
                throw Failure{QStringLiteral("key"), child};
            }
            const QStringList childSegments = segments + key.split(QLatin1Char('.'));
            if (value.isObject()) {
                const QJsonObject inner = value.toObject();
                // flattenTokens' order: a string "type" first, then a string "family".
                if (inner.value(QStringLiteral("type")).isString()) {
                    gradient(inner, child);
                } else if (inner.value(QStringLiteral("family")).isString()) {
                    font(inner, child);
                } else {
                    group(inner, child, childSegments, depth + 1);
                }
                continue;
            }
            if (value.isArray()) {
                throw Failure{QStringLiteral("array"), child};
            }
            if (value.isNull() || value.isUndefined()) {
                throw Failure{QStringLiteral("null"), child};
            }
            if (value.isBool()) {
                // allowed
            } else if (value.isDouble()) {
                if (!finiteBounded(value)) {
                    throw Failure{QStringLiteral("number"), child};
                }
            } else {
                const QString s = value.toString();
                const bool belowFont =
                    childSegments.mid(0, childSegments.size() - 1).contains(QLatin1String("font"));
                if (!(isColor(s) || (belowFont && full(patterns().family, s)))) {
                    throw Failure{QStringLiteral("string"), child};
                }
            }
            if (++leaves > kMaxLeaves) {
                throw Failure{QStringLiteral("leaves"), child};
            }
        }
    }

    void gradient(const QJsonObject& obj, const QString& path) {
        static const QStringList kKeys{QStringLiteral("type"), QStringLiteral("angle"),
                                       QStringLiteral("stops"), QStringLiteral("center"),
                                       QStringLiteral("radius")};
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (!kKeys.contains(it.key())) {
                throw Failure{QStringLiteral("gradient-key"), join(path, it.key())};
            }
        }
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            const QString key = it.key();
            const QJsonValue value = it.value();
            const QString child = join(path, key);
            if (key == QLatin1String("type")) {
                const QString t = value.toString();
                if (t != QLatin1String("linear-gradient") && t != QLatin1String("radial-gradient")) {
                    throw Failure{QStringLiteral("gradient-type"), child};
                }
            } else if (key == QLatin1String("angle") || key == QLatin1String("radius")) {
                if (!finiteBounded(value)) {
                    throw Failure{QStringLiteral("gradient-number"), child};
                }
            } else if (key == QLatin1String("center")) {
                const QJsonArray c = value.toArray();
                if (!value.isArray() || c.size() != 2 || !finiteBounded(c.at(0)) ||
                    !finiteBounded(c.at(1))) {
                    throw Failure{QStringLiteral("gradient-center"), child};
                }
            } else if (key == QLatin1String("stops")) {
                if (!value.isArray() || value.toArray().size() > kMaxStops) {
                    throw Failure{QStringLiteral("gradient-stops"), child};
                }
                const QJsonArray stops = value.toArray();
                for (qsizetype i = 0; i < stops.size(); ++i) {
                    stop(stops.at(i), child + QStringLiteral("[%1]").arg(i));
                }
            }
        }
        ++gradients;
    }

    void stop(const QJsonValue& v, const QString& path) {
        if (!v.isObject()) {
            throw Failure{QStringLiteral("gradient-stops"), path};
        }
        const QJsonObject obj = v.toObject();
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (it.key() != QLatin1String("at") && it.key() != QLatin1String("color")) {
                throw Failure{QStringLiteral("stop-key"), join(path, it.key())};
            }
        }
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            const QString child = join(path, it.key());
            const QJsonValue value = it.value();
            if (it.key() == QLatin1String("at")) {
                if (!value.isDouble() || !(value.toDouble() >= 0.0 && value.toDouble() <= 1.0)) {
                    throw Failure{QStringLiteral("stop-at"), child};
                }
            } else if (!value.isString() || !isColor(value.toString())) {
                throw Failure{QStringLiteral("stop-color"), child};
            }
        }
    }

    void font(const QJsonObject& obj, const QString& path) {
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            const QString& k = it.key();
            if (k != QLatin1String("family") && k != QLatin1String("size") && k != QLatin1String("color")) {
                throw Failure{QStringLiteral("font-key"), join(path, k)};
            }
        }
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            const QString child = join(path, it.key());
            const QJsonValue value = it.value();
            if (it.key() == QLatin1String("family")) {
                if (!full(patterns().family, value.toString())) {
                    throw Failure{QStringLiteral("font-family"), child};
                }
            } else if (it.key() == QLatin1String("size")) {
                if (!value.isDouble() || !(value.toDouble() >= 0.0 && value.toDouble() <= 200.0)) {
                    throw Failure{QStringLiteral("font-size"), child};
                }
            } else if (!value.isString() || !isColor(value.toString())) {
                throw Failure{QStringLiteral("font-color"), child};
            }
        }
        ++fonts;
    }
};

}  // namespace

ThemeValidation validateTheme(const QByteArray& json) {
    ThemeValidation r;
    if (json.size() > kMaxThemeBytes) {
        r.code = QStringLiteral("size");
        return r;
    }
    // The parser ThemeManager's loader uses, on the same bytes: what is validated here is what
    // loads (no second parser whose reading of the document could differ).
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        r.code = QStringLiteral("document");
        return r;
    }
    Walker w;
    try {
        w.document(doc.object());
    } catch (const Failure& f) {
        r.code = f.code;
        r.path = f.path;
        return r;
    }
    r.ok = true;
    r.leaves = w.leaves;
    r.gradients = w.gradients;
    r.fonts = w.fonts;
    return r;
}

}  // namespace asb::gui
