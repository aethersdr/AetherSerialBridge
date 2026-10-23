#include "AppSettings.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace AetherSDR {

namespace {
const QString kActiveTheme = QStringLiteral("ActiveTheme");
}  // namespace

AppSettings& AppSettings::instance() {
    static AppSettings s_instance;
    return s_instance;
}

AppSettings::AppSettings() : m_filePath(resolveFilePath()) { load(); }

QString AppSettings::resolveFilePath() {
    if (QStandardPaths::isTestModeEnabled()) {
        const QString dir = qEnvironmentVariable("AETHER_SETTINGS_DIR");
        if (!dir.isEmpty()) {
            return QDir(dir).filePath(QStringLiteral("gui.ini"));
        }
    }
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
           + QStringLiteral("/gui.ini");
}

bool AppSettings::isIgnoredKey(const QString& key) { return key == kActiveTheme; }

void AppSettings::load() {
    m_values.clear();
    m_dirty = false;
    if (!QFileInfo::exists(m_filePath)) {
        return;
    }
    QSettings file(m_filePath, QSettings::IniFormat);
    const QStringList keys = file.allKeys();
    for (const QString& key : keys) {
        if (!isIgnoredKey(key)) {
            m_values.insert(key, file.value(key));
        }
    }
}

void AppSettings::save() {
    if (!m_dirty) {
        return;
    }
    QDir().mkpath(QFileInfo(m_filePath).absolutePath());
    QSettings file(m_filePath, QSettings::IniFormat);
    file.clear();
    for (auto it = m_values.cbegin(); it != m_values.cend(); ++it) {
        file.setValue(it.key(), it.value());
    }
    file.sync();
    m_dirty = false;
}

QVariant AppSettings::value(const QString& key, const QVariant& defaultValue) const {
    if (isIgnoredKey(key)) {
        return defaultValue;
    }
    return m_values.value(key, defaultValue);
}

void AppSettings::setValue(const QString& key, const QVariant& val) {
    if (isIgnoredKey(key)) {
        ++m_ignoredActiveThemeWrites;
        return;
    }
    m_values.insert(key, val);
    m_dirty = true;
}

void AppSettings::remove(const QString& key) {
    if (m_values.remove(key)) {   // QHash::remove returns bool
        m_dirty = true;
    }
}

bool AppSettings::contains(const QString& key) const {
    return !isIgnoredKey(key) && m_values.contains(key);
}

}  // namespace AetherSDR
