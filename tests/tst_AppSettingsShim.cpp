// The AetherSDR shims (master §3.2, WS-2): AppSettings over QSettings(Ini) at
// <AppConfigLocation>/gui.ini with ActiveTheme dropped (B5, INV-86), LogManager's lcGui, and the
// P1 host hook.

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "core/AppSettings.h"
#include "core/LogManager.h"
#include "core/ThemeManagerHost.h"

using AetherSDR::AppSettings;

class TstAppSettingsShim : public QObject {
    Q_OBJECT

    QTemporaryDir m_scratch;
    QString m_settingsDir;

    QByteArray fileBytes() const {
        QFile f(AppSettings::instance().filePath());
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

private slots:
    void initTestCase() {
        QVERIFY(m_scratch.isValid());
        m_settingsDir = m_scratch.filePath(QStringLiteral("settings"));
        qputenv("AETHER_SETTINGS_DIR", m_settingsDir.toUtf8());
    }

    // AETHER_SETTINGS_DIR is honoured in Qt test mode only. Outside it the file is
    // <AppConfigLocation>/gui.ini whatever the environment says (path only: nothing is written
    // to the real location).
    void filePathOutsideTestMode() {
        QStandardPaths::setTestModeEnabled(false);
        const QString path = AppSettings::resolveFilePath();
        QCOMPARE(path, QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                           QStringLiteral("/gui.ini"));
        QVERIFY(!path.startsWith(m_settingsDir));
    }

    void filePathInTestMode() {
        QStandardPaths::setTestModeEnabled(true);
        QCOMPARE(AppSettings::resolveFilePath(), QDir(m_settingsDir).filePath(QStringLiteral("gui.ini")));
        // The singleton is created here, in test mode, and keeps that file.
        QCOMPARE(AppSettings::instance().filePath(), QDir(m_settingsDir).filePath(QStringLiteral("gui.ini")));
    }

    // setValue changes memory, save() writes the file, load() drops memory and re-reads it.
    void saveAndLoadRoundTrip() {
        auto& s = AppSettings::instance();
        const QString geometry = QStringLiteral("-30000,20,1420,900");   // commas: stays one string
        s.setValue(QStringLiteral("CanonTestGeometry"), geometry);
        QCOMPARE(s.value(QStringLiteral("CanonTestGeometry")).toString(), geometry);
        QVERIFY(!QFile::exists(s.filePath()));   // memory only until save()

        s.save();
        QVERIFY(QFile::exists(s.filePath()));
        s.setValue(QStringLiteral("NotSaved"), 7);
        s.load();
        QVERIFY(!s.contains(QStringLiteral("NotSaved")));
        QCOMPARE(s.value(QStringLiteral("CanonTestGeometry")).toString(), geometry);
        QCOMPARE(s.value(QStringLiteral("Missing"), QStringLiteral("dflt")).toString(), QStringLiteral("dflt"));

        s.remove(QStringLiteral("CanonTestGeometry"));
        QVERIFY(!s.contains(QStringLiteral("CanonTestGeometry")));
        s.save();
        s.load();
        QVERIFY(!s.contains(QStringLiteral("CanonTestGeometry")));
    }

    // B5 / INV-86: no theme state is ever persisted through AppSettings.
    void activeThemeWritesIgnored() {
        auto& s = AppSettings::instance();
        const int before = s.ignoredActiveThemeWrites();
        s.setValue(QStringLiteral("ActiveTheme"), QStringLiteral("Default Light"));
        QCOMPARE(s.ignoredActiveThemeWrites(), before + 1);
        QVERIFY(!s.contains(QStringLiteral("ActiveTheme")));
        QCOMPARE(s.value(QStringLiteral("ActiveTheme"), QStringLiteral("dflt")).toString(),
                 QStringLiteral("dflt"));
        s.setValue(QStringLiteral("Other"), 1);
        s.save();
        QVERIFY(!fileBytes().contains("ActiveTheme"));
        QVERIFY(fileBytes().contains("Other"));

        // A hand-edited file is not read either.
        {
            QSettings ini(s.filePath(), QSettings::IniFormat);
            ini.setValue(QStringLiteral("ActiveTheme"), QStringLiteral("Hand Edited"));
            ini.sync();
        }
        QVERIFY(fileBytes().contains("ActiveTheme"));
        s.load();
        QVERIFY(!s.contains(QStringLiteral("ActiveTheme")));
        QCOMPARE(s.value(QStringLiteral("ActiveTheme"), QStringLiteral("dflt")).toString(),
                 QStringLiteral("dflt"));
        s.setValue(QStringLiteral("Other"), 2);   // the next real save drops the stray key
        s.save();
        QVERIFY(!fileBytes().contains("ActiveTheme"));
    }

    // save() writes only after a change: the vendored ThemeManager constructor's save() (via
    // setActiveTheme, whose ActiveTheme write is dropped) must not rewrite gui.ini at startup.
    void saveWithoutChangesWritesNothing() {
        auto& s = AppSettings::instance();
        s.setValue(QStringLiteral("Kept"), 1);
        s.save();
        QVERIFY(QFile::remove(s.filePath()));
        s.save();
        QVERIFY(!QFile::exists(s.filePath()));
        s.setValue(QStringLiteral("ActiveTheme"), QStringLiteral("Default Dark"));   // dropped
        s.save();
        QVERIFY(!QFile::exists(s.filePath()));
        s.remove(QStringLiteral("NeverSet"));
        s.save();
        QVERIFY(!QFile::exists(s.filePath()));
        s.setValue(QStringLiteral("Kept"), 2);
        s.save();
        QVERIFY(QFile::exists(s.filePath()));
        s.load();
        QCOMPARE(s.value(QStringLiteral("Kept")).toInt(), 2);
    }

    // The closure logs through the bridge's own category.
    void logCategoryIsTheBridges() {
        QCOMPARE(QByteArray(AetherSDR::lcGui().categoryName()), QByteArray("asb.gui"));
        QVERIFY(AetherSDR::lcGui().isWarningEnabled());
        QVERIFY(!AetherSDR::lcGui().isDebugEnabled());
    }

    // The bridge's P1 host: no user theme directory.
    void hostHasNoUserThemeDir() { QVERIFY(AetherSDR::aetherUserThemeDir().isEmpty()); }
};

QTEST_MAIN(TstAppSettingsShim)
#include "tst_AppSettingsShim.moc"
