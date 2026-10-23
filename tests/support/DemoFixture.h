#pragma once

// The demo fixture of the documentation images (INV-135, v1.1 WS-19): docs/images/demo/, read by the docsShots hooks
// of tst_BridgeWindow and tst_Tui, which tools/docs_shots.py runs. Test code only.
//
// The hooks run only when ASB_DOCS_SHOTS_DIR is set. They read the fixture from the folder compiled in
// (ASB_DOCS_DEMO_DIR) and refuse to run unless ASB_DOCS_FIXTURE names that same folder and the settings are isolated
// (ASB_DOCS_SHOTS_HOME: HOME, XDG_CONFIG_HOME, APPDATA and LOCALAPPDATA inside it, and on Linux Qt's own
// GenericConfigLocation too), so nothing real can reach an image.
//
// Beside every image a hook writes <name>.json: {"image", "os", "state", "theme", "scale", "size", "texts"}, the texts
// being every text the image shows, which tools/check_docs_images.py holds to INV-135's rules.

#include "core/StatusStore.h"
#include "core/Interfaces.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QStringList>

#include <optional>

#ifndef ASB_DOCS_DEMO_DIR
#error "ASB_DOCS_DEMO_DIR must name docs/images/demo"
#endif

namespace asb::test::docs {

struct Environment {
    QStringList serialDevices, localAddresses;
    QString dataFolder, appImage;
};

// Why the hooks may not run here, or an empty string when they may.
inline QString refusal()
{
    const QString compiled = QFileInfo(QStringLiteral(ASB_DOCS_DEMO_DIR)).canonicalFilePath();
    const QString given = QFileInfo(qEnvironmentVariable("ASB_DOCS_FIXTURE")).canonicalFilePath();
    if (compiled.isEmpty() || given != compiled)
        return QStringLiteral("ASB_DOCS_FIXTURE is not the demo fixture %1 (INV-135)").arg(QStringLiteral(ASB_DOCS_DEMO_DIR));
    const QString home = QFileInfo(qEnvironmentVariable("ASB_DOCS_SHOTS_HOME")).canonicalFilePath();
    if (home.isEmpty()) return QStringLiteral("ASB_DOCS_SHOTS_HOME is not set (run tools/docs_shots.py)");
    const auto inside = [&](const QString& p) {
        if (p.isEmpty()) return false;
        // A folder Qt has not created yet has no canonical path: its parent's, plus the rest.
        QString c = QFileInfo(p).canonicalFilePath();
        if (c.isEmpty()) {
            QString rest, at = QDir::cleanPath(QFileInfo(p).absoluteFilePath());
            for (int up = 0; c.isEmpty() && up < 16; ++up) {
                rest = QLatin1Char('/') + QFileInfo(at).fileName() + rest;
                at = QFileInfo(at).absolutePath();
                c = QFileInfo(at).canonicalFilePath();
            }
            if (c.isEmpty()) return false;
            c += rest;
        }
        return c == home || c.startsWith(home + QLatin1Char('/'));
    };
    for (const char* k : {"HOME", "XDG_CONFIG_HOME", "APPDATA", "LOCALAPPDATA"})
        if (!inside(qEnvironmentVariable(k))) return QStringLiteral("%1 is not inside ASB_DOCS_SHOTS_HOME").arg(QLatin1String(k));
#ifndef _WIN32
    if (!inside(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)))
        return QStringLiteral("Qt's settings folder is not inside ASB_DOCS_SHOTS_HOME");
#endif
    return {};
}

inline QByteArray file(const QString& name)
{
    QFile f(QStringLiteral(ASB_DOCS_DEMO_DIR "/") + name);
    if (!f.open(QIODevice::ReadOnly)) qFatal("docs fixture: cannot read %s", qPrintable(f.fileName()));
    return f.readAll();
}

inline QJsonObject object(const QString& name)
{
    const QJsonDocument d = QJsonDocument::fromJson(file(name));
    if (!d.isObject()) qFatal("docs fixture: %s is not a JSON object", qPrintable(name));
    return d.object();
}

inline QByteArray config(bool windows) { return file(windows ? QStringLiteral("config-windows.json") : QStringLiteral("config-linux.json")); }

inline StatusReport status(bool windows)
{
    QString err;
    const auto r = StatusStore::fromJson(file(windows ? QStringLiteral("status-windows.json") : QStringLiteral("status-linux.json")), &err);
    if (!r) qFatal("docs fixture: status: %s", qPrintable(err));
    return *r;
}

// The fixture's ports and times over the identity the fake engine published (pid, start token, config digest), so the
// window and the TUI show the report as the running engine's.
inline StatusReport statusOver(const std::optional<StatusReport>& published, bool windows)
{
    StatusReport s = status(windows);
    // The fixture's times are kept as ages relative to its "updated", which becomes now (a report is shown only while
    // fresh), so the images show the same durations whenever they are made.
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const auto shift = [&](QDateTime& t) {
        if (t.isValid()) t = now.addMSecs(s.updated.msecsTo(t));
    };
    shift(s.startedAt);
    for (PortStatus& p : s.ports) shift(p.since);
    s.updated = now;
    if (published) {
        s.pid = published->pid;
        s.startToken = published->startToken;
        s.configSha256 = published->configSha256;
        s.instanceId = published->instanceId;
    }
    return s;
}

// "not-installed", "current", "other-build" onto an Installation (the flavor is the caller's).
inline void applyInstallation(Installation& i, const QString& which)
{
    const QJsonObject o = object(QStringLiteral("installation-%1.json").arg(which));
    const QString s = o.value(QStringLiteral("state")).toString();
    if (s == QLatin1String("NotInstalled")) i.state = InstallState::NotInstalled;
    else if (s == QLatin1String("Current")) i.state = InstallState::Current;
    else if (s == QLatin1String("IncompleteInstall")) i.state = InstallState::IncompleteInstall;
    else qFatal("docs fixture: installation state %s", qPrintable(s));
    i.otherBuild = o.value(QStringLiteral("otherBuild")).toBool();
    i.installedSha256 = o.value(QStringLiteral("installedSha256")).toString();
    const QJsonValue v = o.value(QStringLiteral("installedVersion"));
    if (v.isString()) i.installedVersion = v.toString();
    else i.installedVersion.reset();
    i.detail = o.value(QStringLiteral("detail")).toString();
}

inline Environment environment(bool windows)
{
    const QJsonObject o = object(windows ? QStringLiteral("environment-windows.json") : QStringLiteral("environment-linux.json"));
    Environment e;
    for (const QJsonValue& v : o.value(QStringLiteral("serialDevices")).toArray()) e.serialDevices << v.toString();
    for (const QJsonValue& v : o.value(QStringLiteral("localAddresses")).toArray()) e.localAddresses << v.toString();
    e.dataFolder = o.value(QStringLiteral("dataFolder")).toString();
    e.appImage = o.value(QStringLiteral("appImage")).toString();
    return e;
}

struct Shot {
    QString name, os, state, theme;
    double scale = 1;
    int width = 0, height = 0;
    QStringList texts;
};

// Writes <dir>/<name>.json; texts are de-duplicated, empty ones dropped, order kept.
inline bool writeManifest(const QString& dir, const Shot& s)
{
    QStringList texts;
    for (const QString& t : s.texts) {
        const QString x = t.trimmed();
        if (!x.isEmpty() && !texts.contains(x)) texts << x;
    }
    QJsonObject o{{QStringLiteral("image"), s.name + QStringLiteral(".png")},
                  {QStringLiteral("os"), s.os},
                  {QStringLiteral("state"), s.state},
                  {QStringLiteral("theme"), s.theme},
                  {QStringLiteral("scale"), s.scale},
                  {QStringLiteral("size"), QJsonArray{s.width, s.height}},
                  {QStringLiteral("texts"), QJsonArray::fromStringList(texts)}};
    QFile f(dir + QLatin1Char('/') + s.name + QStringLiteral(".json"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(QJsonDocument(o).toJson(QJsonDocument::Indented)) > 0;
}

}  // namespace asb::test::docs
