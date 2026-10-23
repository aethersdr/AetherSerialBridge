#pragma once

// Aether Serial Bridge shim for AetherSDR's AppSettings (master §3.2, WS-2).
//
// The vendored canon closure includes "AppSettings.h" / "core/AppSettings.h"; the include order
// (shims/aethersdr/src/core, shims/aethersdr/src, vendor/aethersdr/src) makes it find this file,
// never AetherSDR's own settings store (which is not vendored). Only the calls the closure makes
// exist here: instance/load/save/value/setValue (+ remove/contains).
//
// Storage: QSettings(IniFormat) at <AppConfigLocation>/gui.ini, the bridge's own GUI settings
// file (the identity, D20, is applied before the first instance() call). In Qt test mode only,
// AETHER_SETTINGS_DIR (set by the vendored TestSettingsProfile) replaces the directory.
//
// Values live in memory between load() and save(), as in AetherSDR: setValue() changes memory,
// save() writes the file, load() drops memory and re-reads the file. The constructor loads.
// save() writes only when something changed since the last load() or save(), so the vendored
// ThemeManager constructor's save() (through setActiveTheme) never rewrites gui.ini at startup.
//
// The key "ActiveTheme" does not exist here (B5, INV-86): writes to it are dropped (and counted,
// for tests), reads return the caller's default, and it is never written to the file. The
// vendored ThemeManager constructor's setActiveTheme("Default Dark") therefore stores nothing,
// and no theme state is ever persisted through AppSettings.

#include <QHash>
#include <QString>
#include <QVariant>

namespace AetherSDR {

class AppSettings {
public:
    static AppSettings& instance();

    // Drops the in-memory values and re-reads the file (a missing file is an empty store).
    void load();
    // Writes the in-memory values to the file, replacing its contents; a no-op when nothing
    // changed since the last load() or save().
    void save();

    QVariant value(const QString& key, const QVariant& defaultValue = {}) const;
    void setValue(const QString& key, const QVariant& val);
    void remove(const QString& key);
    bool contains(const QString& key) const;

    // The settings file, fixed when the singleton is created.
    QString filePath() const { return m_filePath; }
    // Writes to "ActiveTheme" that were dropped. Test spy: theme activation must not try to
    // persist anything through AppSettings (INV-86).
    int ignoredActiveThemeWrites() const { return m_ignoredActiveThemeWrites; }

    // The file the shim uses, resolved now: AETHER_SETTINGS_DIR/gui.ini in Qt test mode when that
    // variable is set, otherwise <AppConfigLocation>/gui.ini.
    static QString resolveFilePath();

    AppSettings(const AppSettings&) = delete;
    AppSettings& operator=(const AppSettings&) = delete;

private:
    AppSettings();

    static bool isIgnoredKey(const QString& key);

    QString m_filePath;
    QHash<QString, QVariant> m_values;
    bool m_dirty = false;
    int m_ignoredActiveThemeWrites = 0;
};

}  // namespace AetherSDR
