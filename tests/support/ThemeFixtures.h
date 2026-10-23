#pragma once

// Theme JSON fixtures for the P1/P2 tests (WS-2): variants of the bundled Default Dark, built in
// memory so their bytes exist in no file.

#include <functional>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace asb::test {

// The bytes of a bundled theme, from the GUI resources (":/themes/default-dark.json", ...).
inline QByteArray builtInThemeBytes(const QString& resource) {
    QFile f(resource);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return f.readAll();
}

// obj[path[0]][path[1]]... = value, creating the intermediate objects.
inline void setPath(QJsonObject& obj, const QStringList& path, const QJsonValue& value) {
    if (path.size() == 1) {
        obj.insert(path.first(), value);
        return;
    }
    QJsonObject inner = obj.value(path.first()).toObject();
    setPath(inner, path.mid(1), value);
    obj.insert(path.first(), inner);
}

// Default Dark renamed, with `edit` applied to the whole document (v2: the root scope's tokens
// are at scopes.root.tokens, child scopes at scopes.root.scopes, the palette at primitives).
inline QByteArray themeVariant(const QString& name,
                               const std::function<void(QJsonObject& doc)>& edit = {}) {
    QJsonObject doc =
        QJsonDocument::fromJson(builtInThemeBytes(QStringLiteral(":/themes/default-dark.json")))
            .object();
    if (doc.isEmpty()) {
        return {};
    }
    doc.insert(QStringLiteral("name"), name);
    if (edit) {
        edit(doc);
    }
    return QJsonDocument(doc).toJson(QJsonDocument::Compact);
}

inline QStringList rootToken(const QStringList& tokenPath) {
    return QStringList{QStringLiteral("scopes"), QStringLiteral("root"), QStringLiteral("tokens")} +
           tokenPath;
}

// The canon cyan of the "bytes only" theme: a value no bundled theme file has.
inline QString bytesOnlyCyan() { return QStringLiteral("#123456"); }

inline QByteArray bytesOnlyTheme(const QString& name) {
    return themeVariant(name, [](QJsonObject& doc) {
        setPath(doc, rootToken({QStringLiteral("color"), QStringLiteral("canon"), QStringLiteral("cyan")}),
                bytesOnlyCyan());
    });
}

}  // namespace asb::test
