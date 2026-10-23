// Theme following (master §5.6, WS-11): INV-31, 36, 53, 85, 86, 95, and INV-52's follower-layer
// gate (replaceAfterValidationHasNoEffect).
//
// Everything runs through a real ThemeFollower against real files: an AetherSDR-shaped settings
// database (app_settings WITHOUT ROWID, WAL) and a real theme folder in a scratch directory. The
// follower's seams are its paths, the SQLite open call (which records flags and URI, and can route
// through the stalling wrapper VFS in support/SlowVfs.c) and two test hooks. The one ThemeManager
// singleton of this process is what gets themed.
//
// Timing (D-WS11-3). A loaded host (a CI runner, a build next to a burner) can stretch any wait.
// So: a test that is not about the 750 ms UI deadline gives the follower a deadline it cannot miss
// (kTestUiDeadlineMs, through Options::uiDeadlineMs) and asserts the outcome; a test that is about
// the deadline runs the real 750 ms and holds the read on a gate (asb_slowvfs_arm_gated), so the
// read cannot answer first whatever the host is doing, and it asserts the order of events, never
// how long they took. Real time appears only as a lower bound (a timer cannot fire early) or as
// the generous limit (kWaitMs) a failing run gives up at.

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include <QColor>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QtTest>

#include <sqlite3.h>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "gui/ThemeDiscovery.h"
#include "gui/ThemeFollower.h"
#include "gui/ThemeNameReader.h"
#include "gui/ThemeValidator.h"
#include "support/SlowVfs.h"

using AetherSDR::AppSettings;
using AetherSDR::ThemeManager;
using asb::gui::ThemeDiscovery;
using asb::gui::ThemeFollower;

namespace {

const QString kDark = QStringLiteral("Default Dark");
const QString kLight = QStringLiteral("Default Light");

bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QByteArray sha256(const QString& path) {
    return QCryptographicHash::hash(readFile(path), QCryptographicHash::Sha256).toHex();
}

// A small valid theme (schema 1: tokens land in the root scope as "color.accent").
QByteArray userTheme(const QString& name, const QString& accent) {
    QJsonObject color{{QStringLiteral("accent"), accent}};
    QJsonObject doc{{QStringLiteral("schemaVersion"), 1},
                    {QStringLiteral("name"), name},
                    {QStringLiteral("tokens"), QJsonObject{{QStringLiteral("color"), color}}}};
    return QJsonDocument(doc).toJson(QJsonDocument::Indented);
}

bool exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        qWarning("sqlite: %s: %s", sql, err ? err : "?");
        sqlite3_free(err);
    }
    return rc == SQLITE_OK;
}

// AetherSDR's settings database, the way SettingsDatabase creates it (WAL, app_settings WITHOUT
// ROWID), holding `active` (or no ActiveTheme row when empty). Closed afterwards, so the WAL is
// checkpointed into the main file.
bool makeDb(const QString& path, const QString& active) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.toUtf8().constData(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
        SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    bool ok = exec(db, "PRAGMA journal_mode=WAL;") &&
              exec(db, "CREATE TABLE IF NOT EXISTS app_settings (key TEXT PRIMARY KEY, value TEXT NOT NULL) "
                       "WITHOUT ROWID;");
    if (ok && !active.isEmpty()) {
        const QByteArray sql = "INSERT INTO app_settings (key, value) VALUES ('ActiveTheme', '" + active.toUtf8() +
                               "') ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
        ok = exec(db, sql.constData());
    }
    ok = exec(db, "INSERT INTO app_settings (key, value) VALUES ('SomethingElse', 'x') ON CONFLICT(key) DO NOTHING;") &&
         ok;
    sqlite3_close(db);
    return ok;
}

// The open seam: records every call; optionally routes through the stalling VFS.
// Failure-time limit for "this must happen": never the thing under test, only how long a broken run
// waits before it says so.
constexpr int kWaitMs = 10000;
// The UI deadline for tests that are not about it (the product's is 750 ms).
constexpr int kTestUiDeadlineMs = 120000;

struct OpenRecorder {
    std::mutex mutex;
    QList<int> flags;
    QStringList uris;
    std::atomic<bool> slow{false};
};

// `uiDeadlineMs` 0 is the product's 750 ms; the default is a deadline no host load can reach.
ThemeFollower::Options optionsFor(const QString& dir, const std::shared_ptr<OpenRecorder>& rec,
                                  int uiDeadlineMs = kTestUiDeadlineMs) {
    ThemeFollower::Options o;
    o.uiDeadlineMs = uiDeadlineMs;
    o.databasePath = dir + QStringLiteral("/AetherSDR.db");
    o.themesDir = dir + QStringLiteral("/themes");
    o.openDatabase = [rec](const char* uri, sqlite3** db, int flags) {
        {
            std::lock_guard<std::mutex> lock(rec->mutex);
            rec->flags << flags;
            rec->uris << QString::fromUtf8(uri);
        }
        return sqlite3_open_v2(uri, db, flags, rec->slow ? ASB_SLOW_VFS_NAME : nullptr);
    };
    return o;
}

struct Applied {
    QString name;
    QString reason;
    bool ok = false;
};

// Waits for the next applied() on `spy` (which may already hold it).
Applied nextApplied(QSignalSpy& spy, int index, int timeoutMs = kWaitMs) {
    QElapsedTimer t;
    t.start();
    while (spy.count() <= index && t.elapsed() < timeoutMs) {
        spy.wait(50);
    }
    Applied a;
    if (spy.count() > index) {
        a.name = spy.at(index).at(0).toString();
        a.reason = spy.at(index).at(1).toString();
        a.ok = true;
    }
    return a;
}

#ifdef Q_OS_LINUX
int threadsNamed(const QByteArray& name) {
    int n = 0;
    const QStringList tasks = QDir(QStringLiteral("/proc/self/task")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& t : tasks) {
        if (readFile(QStringLiteral("/proc/self/task/%1/comm").arg(t)).trimmed() == name) {
            ++n;
        }
    }
    return n;
}
#endif

}  // namespace

#ifdef Q_OS_WIN
// The administrative share is not reachable. On a developer machine that is an environment
// fact and the test skips; in CI (GitHub sets CI=true) a skip would let the only real-UNC
// proof go missing without anyone noticing, so there it is a failure.
static void skipOrFailUnreachableShare(const QString &path) {
    const QString what = QStringLiteral("the administrative share is not reachable here: ") + path;
    if (qEnvironmentVariableIsSet("CI")) {
        QFAIL(qPrintable(what + QStringLiteral(" (CI is set: the UNC tests must run, not skip)")));
    }
    QSKIP(qPrintable(what));
}
#endif

class TstThemeFollower : public QObject {
    Q_OBJECT

    QTemporaryDir m_scratch;
    int m_caseNo = 0;

    // A fresh AetherSDR config directory: <dir>/AetherSDR.db and <dir>/themes/.
    QString newAetherDir(const QString& tag = {}) {
        const QString dir = m_scratch.filePath(QStringLiteral("case%1%2").arg(++m_caseNo).arg(tag));
        if (!QDir().mkpath(dir + QStringLiteral("/themes"))) {
            return {};
        }
        return dir;
    }

    // Puts the process on `name` first (through the validated route), so a switch is visible.
    void activateBuiltIn(const QString& name) {
        ThemeFollower f;
        f.start(name == kLight ? ThemeFollower::Choice::Light : ThemeFollower::Choice::Dark);
        QCOMPARE(ThemeManager::instance().activeTheme(), name);
    }

private slots:
    void initTestCase() {
        QVERIFY(m_scratch.isValid());
        QStandardPaths::setTestModeEnabled(true);
        qputenv("AETHER_SETTINGS_DIR", m_scratch.filePath(QStringLiteral("settings")).toUtf8());
        QCOMPARE(asb_slowvfs_register(), SQLITE_OK);
        AppSettings::instance();
        ThemeManager::instance();
    }

    void cleanup() {
        asb_slowvfs_arm(0, 0);  // also opens the gate, so no read is left held
        QVERIFY2(ThemeFollower::waitUntilIdle(20000), "a theme read was still outstanding after the test");
    }

    // ---------------------------------------------------------------------------------------
    // INV-36: the validator (oracle agreement, built-ins)
    // ---------------------------------------------------------------------------------------

    void oracleFixturesAgree_data() {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QJsonObject>("expected");
        const QString src = QStringLiteral(ASB_SOURCE_DIR);
        const QJsonObject manifest =
            QJsonDocument::fromJson(readFile(src + QStringLiteral("/tests/data/themes/manifest.json"))).object();
        const QJsonArray fixtures = manifest.value(QStringLiteral("fixtures")).toArray();
        QVERIFY(fixtures.size() >= 40);
        for (const QJsonValue& v : fixtures) {
            const QJsonObject e = v.toObject();
            const QString file = e.value(QStringLiteral("file")).toString();
            QTest::newRow(qPrintable(file)) << src + QLatin1Char('/') + file << e;
        }
    }

    // The Qt validator gives exactly the oracle's result on every fixture: PASS with the same
    // counts, or FAIL with the same rule at the same path.
    void oracleFixturesAgree() {
        QFETCH(QString, file);
        QFETCH(QJsonObject, expected);
        QByteArray bytes = readFile(file);
        QVERIFY2(!bytes.isEmpty(), qPrintable(file));
        const asb::gui::ThemeValidation v = asb::gui::validateTheme(bytes);
        const bool pass = expected.value(QStringLiteral("result")).toString() == QLatin1String("PASS");
        QVERIFY2(v.ok == pass, qPrintable(QStringLiteral("Qt validator: %1").arg(v.describe())));
        if (pass) {
            QCOMPARE(v.leaves, expected.value(QStringLiteral("leaves")).toInt());
            QCOMPARE(v.gradients, expected.value(QStringLiteral("gradients")).toInt());
            QCOMPARE(v.fonts, expected.value(QStringLiteral("fonts")).toInt());
        } else {
            QCOMPARE(v.code, expected.value(QStringLiteral("code")).toString());
            QCOMPARE(v.path, expected.value(QStringLiteral("path")).toString());
        }
    }

    // The two named fixtures of the WS-11 mutation table are in the manifest with the expected
    // verdicts (so "skip top-level tokens" and "accept unknown gradient key" cannot pass).
    void mutationFixturesPresent() {
        const QJsonObject manifest = QJsonDocument::fromJson(readFile(QStringLiteral(ASB_SOURCE_DIR) +
                                                                      QStringLiteral("/tests/data/themes/manifest.json")))
                                         .object();
        QMap<QString, QJsonObject> byFile;
        for (const QJsonValue& v : manifest.value(QStringLiteral("fixtures")).toArray()) {
            byFile.insert(v.toObject().value(QStringLiteral("file")).toString(), v.toObject());
        }
        const QJsonObject inj = byFile.value(QStringLiteral("tests/data/themes/v1_top_level_injection.json"));
        QCOMPARE(inj.value(QStringLiteral("result")).toString(), QStringLiteral("FAIL"));
        QCOMPARE(inj.value(QStringLiteral("path")).toString(), QStringLiteral("tokens.color.accent"));
        const QJsonObject gek = byFile.value(QStringLiteral("tests/data/themes/gradient_extra_key.json"));
        QCOMPARE(gek.value(QStringLiteral("code")).toString(), QStringLiteral("gradient-key"));
        const QJsonObject saveAs = byFile.value(QStringLiteral("tests/data/themes/pass_saveas_v2_basetheme.json"));
        QCOMPARE(saveAs.value(QStringLiteral("result")).toString(), QStringLiteral("PASS"));
    }

    // The bundled themes pass with the counts the spec measured (B5: 223 leaves / 10 gradients,
    // 224 leaves), read from the compiled-in resources the follower activates.
    void builtInsPassWithMeasuredCounts() {
        const auto dark = asb::gui::validateTheme(readFile(QStringLiteral(":/themes/default-dark.json")));
        QVERIFY2(dark.ok, qPrintable(dark.describe()));
        QCOMPARE(dark.leaves, 223);
        QCOMPARE(dark.gradients, 10);
        const auto light = asb::gui::validateTheme(readFile(QStringLiteral(":/themes/default-light.json")));
        QVERIFY2(light.ok, qPrintable(light.describe()));
        QCOMPARE(light.leaves, 224);
        QCOMPARE(light.gradients, 10);
    }

    // The oracle's colour words are exactly the names QColor::isValidColorName accepts.
    void colorNamesMatchOracle() {
        const QString python = qEnvironmentVariable("ASB_PYTHON");
        QVERIFY2(!python.isEmpty(), "ASB_PYTHON is not set: the oracle cannot run (a failure, not a skip)");
        QProcess p;
        p.start(python, {QStringLiteral(ASB_SOURCE_DIR) + QStringLiteral("/tools/theme_validator_ref.py"),
                         QStringLiteral("--color-names")});
        QVERIFY(p.waitForFinished(30000));
        QCOMPARE(p.exitCode(), 0);
        QSet<QString> oracle;
        for (const QByteArray& line : p.readAllStandardOutput().split('\n')) {
            if (!line.trimmed().isEmpty()) {
                oracle.insert(QString::fromLatin1(line.trimmed()));
            }
        }
        QSet<QString> qt;
        for (const QString& n : QColor::colorNames()) {
            qt.insert(n.toLower());
        }
        QCOMPARE(oracle, qt);
        for (const QString& n : oracle) {
            QVERIFY2(QColor::isValidColorName(n), qPrintable(n));
        }
    }

    // AetherSDR's own Save As output (schema 2, baseTheme) passes; the committed fixture is what
    // the vendored saveCurrentThemeAs writes today.
    void saveAsFixtureIsAetherSdrOutput() {
        const QString gen = qEnvironmentVariable("ASB_SAVEAS_GEN");
        QVERIFY2(!gen.isEmpty(), "ASB_SAVEAS_GEN is not set");
        const QString home = m_scratch.filePath(QStringLiteral("saveas-home"));
        QVERIFY(QDir().mkpath(home));
        const QString out = m_scratch.filePath(QStringLiteral("saveas-out.json"));
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("HOME"), home);
        env.remove(QStringLiteral("XDG_CONFIG_HOME"));
        env.insert(QStringLiteral("AETHER_SETTINGS_DIR"), m_scratch.filePath(QStringLiteral("saveas-settings")));
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        p.setProcessEnvironment(env);
        p.start(gen, {out});
        QVERIFY(p.waitForFinished(60000));
        QVERIFY2(p.exitCode() == 0, p.readAllStandardError().constData());
        const QByteArray produced = readFile(out);
        QByteArray committed = readFile(QStringLiteral(ASB_SOURCE_DIR) +
                                        QStringLiteral("/tests/data/themes/pass_saveas_v2_basetheme.json"));
        committed.replace("\r\n", "\n");  // a Windows checkout may convert line ends
        QCOMPARE(produced, committed);
        const QJsonObject doc = QJsonDocument::fromJson(produced).object();
        QCOMPARE(doc.value(QStringLiteral("schemaVersion")).toInt(), 2);
        QCOMPARE(doc.value(QStringLiteral("baseTheme")).toString(), kLight);
        const auto v = asb::gui::validateTheme(produced);
        QVERIFY2(v.ok, qPrintable(v.describe()));
    }

    // ---------------------------------------------------------------------------------------
    // Following
    // ---------------------------------------------------------------------------------------

    void followsUserTheme() {
        const QString dir = newAetherDir();
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), QStringLiteral("ASB User")));
        QVERIFY(writeFile(dir + QStringLiteral("/themes/asb-user.json"),
                          userTheme(QStringLiteral("ASB User"), QStringLiteral("#0a0b0c"))));
        auto rec = std::make_shared<OpenRecorder>();
        ThemeFollower f(optionsFor(dir, rec));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        const Applied a = nextApplied(spy, 0);
        QVERIFY(a.ok);
        QCOMPARE(a.name, QStringLiteral("ASB User"));
        QVERIFY2(a.reason.contains(QLatin1String("Following AetherSDR")), qPrintable(a.reason));
        QCOMPARE(ThemeManager::instance().activeTheme(), QStringLiteral("ASB User"));
        QCOMPARE(ThemeManager::instance().value(QStringLiteral("color.accent")), QStringLiteral("#0a0b0c"));
    }

    void invalidUserThemeFallsBackToDefaultDark() {
        activateBuiltIn(kLight);
        const QString dir = newAetherDir();
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), QStringLiteral("ASB Bad")));
        QVERIFY(writeFile(dir + QStringLiteral("/themes/bad.json"),
                          userTheme(QStringLiteral("ASB Bad"), QStringLiteral("red; image: url(//h/s)"))));
        ThemeFollower f(optionsFor(dir, std::make_shared<OpenRecorder>()));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        const Applied a = nextApplied(spy, 0);
        QCOMPARE(a.name, kDark);
        QVERIFY2(a.reason.contains(QLatin1String("failed validation")), qPrintable(a.reason));
        QCOMPARE(ThemeManager::instance().activeTheme(), kDark);
    }

    void noDatabaseOrNoValueMeansDefaultDark() {
        activateBuiltIn(kLight);
        const QString dir = newAetherDir();  // no AetherSDR.db at all
        {
            ThemeFollower f(optionsFor(dir, std::make_shared<OpenRecorder>()));
            QSignalSpy spy(&f, &ThemeFollower::applied);
            f.start(ThemeFollower::Choice::Follow);
            const Applied a = nextApplied(spy, 0);
            QCOMPARE(a.name, kDark);
            QVERIFY2(a.reason.contains(QLatin1String("Could not read")), qPrintable(a.reason));
        }
        QVERIFY(!QFile::exists(dir + QStringLiteral("/AetherSDR.db")));  // mode=ro created nothing
        activateBuiltIn(kLight);
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), QString()));
        ThemeFollower f(optionsFor(dir, std::make_shared<OpenRecorder>()));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        const Applied a = nextApplied(spy, 0);
        QCOMPARE(a.name, kDark);
        QVERIFY2(a.reason.contains(QLatin1String("no theme recorded")), qPrintable(a.reason));
    }

    // The choice lives in the bridge's gui.ini under ThemeChoice; no theme state does.
    void choiceStoredInGuiIni() {
        ThemeFollower f(optionsFor(newAetherDir(), std::make_shared<OpenRecorder>()));
        f.start(ThemeFollower::Choice::Light);
        QCOMPARE(ThemeFollower::savedChoice(), ThemeFollower::Choice::Light);
        f.start(ThemeFollower::Choice::Dark);
        QCOMPARE(ThemeFollower::savedChoice(), ThemeFollower::Choice::Dark);
        const QByteArray ini = readFile(AppSettings::instance().filePath());
        QVERIFY2(ini.contains("ThemeChoice=Dark"), ini.constData());
        QVERIFY(!ini.contains("ActiveTheme"));
        QVERIFY(!AppSettings::instance().contains(QStringLiteral("ActiveTheme")));
    }

    // ---------------------------------------------------------------------------------------
    // INV-86: one activation route
    // ---------------------------------------------------------------------------------------

    // Built-ins (chosen, and followed) are activated by activateValidatedTheme with the resource
    // bytes, never by setActiveTheme: the AppSettings spy sees no ActiveTheme write. Every step
    // switches to the OTHER built-in, because setActiveTheme returns early (before its write)
    // when the name is already active.
    void builtInsThroughSameRoute() {
        auto& tm = ThemeManager::instance();
        const int writesBefore = AppSettings::instance().ignoredActiveThemeWrites();
        activateBuiltIn(kDark);

        const QString dir = newAetherDir();
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), kLight));
        ThemeFollower f(optionsFor(dir, std::make_shared<OpenRecorder>()));
        QSignalSpy spy(&f, &ThemeFollower::applied);

        f.start(ThemeFollower::Choice::Light);
        QCOMPARE(tm.activeTheme(), kLight);
        f.start(ThemeFollower::Choice::Dark);
        QCOMPARE(tm.activeTheme(), kDark);
        f.start(ThemeFollower::Choice::Follow);  // AetherSDR says Default Light
        const Applied a = nextApplied(spy, 2);
        QCOMPARE(a.name, kLight);
        QCOMPARE(tm.activeTheme(), kLight);
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), kDark));
        f.refresh();
        QCOMPARE(nextApplied(spy, 3).name, kDark);
        QCOMPARE(tm.activeTheme(), kDark);

        QCOMPARE(spy.count(), 4);
        QCOMPARE(AppSettings::instance().ignoredActiveThemeWrites(), writesBefore);
        QVERIFY(!AppSettings::instance().contains(QStringLiteral("ActiveTheme")));
    }

    // ---------------------------------------------------------------------------------------
    // INV-52 (follower layer): the bytes validated are the bytes activated
    // ---------------------------------------------------------------------------------------

    // The follower validates the file; the file is then replaced on disk (atomically, by rename,
    // with a valid theme of the same name and a different accent) before activation. The
    // follower still activates exactly the validated bytes.
    void replaceAfterValidationHasNoEffect() {
        const QString dir = newAetherDir();
        const QString file = dir + QStringLiteral("/themes/replace.json");
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), QStringLiteral("ASB Replace")));
        QVERIFY(writeFile(file, userTheme(QStringLiteral("ASB Replace"), QStringLiteral("#111111"))));
        auto opts = optionsFor(dir, std::make_shared<OpenRecorder>());
        int replaced = 0;
        opts.beforeActivate = [&](const QString& path) {
            QCOMPARE(QFileInfo(path).fileName(), QStringLiteral("replace.json"));
            QSaveFile s(file);
            QVERIFY(s.open(QIODevice::WriteOnly));
            s.write(userTheme(QStringLiteral("ASB Replace"), QStringLiteral("#222222")));
            QVERIFY(s.commit());
            ++replaced;
        };
        ThemeFollower f(opts);
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        const Applied a = nextApplied(spy, 0);
        QCOMPARE(replaced, 1);
        QVERIFY(readFile(file).contains("#222222"));
        QCOMPARE(a.name, QStringLiteral("ASB Replace"));
        QCOMPARE(ThemeManager::instance().activeTheme(), QStringLiteral("ASB Replace"));
        QCOMPARE(ThemeManager::instance().value(QStringLiteral("color.accent")), QStringLiteral("#111111"));
    }

    // ---------------------------------------------------------------------------------------
    // INV-31: read-only
    // ---------------------------------------------------------------------------------------

    void openFlagsReadOnly() {
        const QString dir = newAetherDir();
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), kLight));
        auto rec = std::make_shared<OpenRecorder>();
        ThemeFollower f(optionsFor(dir, rec));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        QCOMPARE(nextApplied(spy, 0).name, kLight);
        std::lock_guard<std::mutex> lock(rec->mutex);
        QCOMPARE(rec->flags.size(), 1);
        QCOMPARE(rec->flags.first(), SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX);
        QCOMPARE(rec->uris.first(), asb::gui::themeDbUri(dir + QStringLiteral("/AetherSDR.db")));
        QVERIFY(rec->uris.first().startsWith(QLatin1String("file:")));
        QVERIFY(rec->uris.first().endsWith(QLatin1String("?mode=ro")));
    }

    // INV-31 on the theme files: the open asks for read access only (O_RDONLY / GENERIC_READ). A
    // theme file this process may not write is still followed; an open asking for write access
    // would fail on it and the theme would not be found.
    void themeFileOpenedReadOnly() {
#ifdef Q_OS_UNIX
        if (::geteuid() == 0) {
            QSKIP("root is not bound by file permissions; this check needs an unprivileged user");
        }
#endif
        activateBuiltIn(kLight);
        const QString dir = newAetherDir();
        const QString file = dir + QStringLiteral("/themes/read-only.json");
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), QStringLiteral("ASB ReadOnly")));
        QVERIFY(writeFile(file, userTheme(QStringLiteral("ASB ReadOnly"), QStringLiteral("#0d0d0d"))));
        const auto restore = qScopeGuard([file]() {
            QFile::setPermissions(file, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser |
                                            QFileDevice::WriteUser);
        });
        QVERIFY(QFile::setPermissions(file, QFileDevice::ReadOwner | QFileDevice::ReadUser | QFileDevice::ReadGroup |
                                                QFileDevice::ReadOther));
        {
            QFile probe(file);
            QVERIFY2(!probe.open(QIODevice::ReadWrite), "the theme file is still writable; the check would be vacuous");
        }
        const QByteArray before = sha256(file);

        ThemeFollower f(optionsFor(dir, std::make_shared<OpenRecorder>()));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        const Applied a = nextApplied(spy, 0);
        QVERIFY2(a.name == QStringLiteral("ASB ReadOnly"), qPrintable(a.reason));
        QCOMPARE(sha256(file), before);
    }

    // With a writer holding a WAL-only commit open: the main database and the -wal file are
    // byte-identical after the follower's read (the -shm reader slots may change: R7).
    void mainDbAndWalBytesUnchanged() {
        const QString dir = newAetherDir();
        const QString dbPath = dir + QStringLiteral("/AetherSDR.db");
        QVERIFY(makeDb(dbPath, kDark));
        sqlite3* writer = nullptr;
        QCOMPARE(sqlite3_open_v2(dbPath.toUtf8().constData(), &writer, SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
        QVERIFY(exec(writer, "PRAGMA wal_autocheckpoint=0;"));
        QVERIFY(exec(writer, "UPDATE app_settings SET value='Default Light' WHERE key='ActiveTheme';"));
        const QString wal = dbPath + QStringLiteral("-wal");
        QVERIFY(QFileInfo(wal).size() > 0);
        const QByteArray dbBefore = sha256(dbPath);
        const QByteArray walBefore = sha256(wal);

        ThemeFollower f(optionsFor(dir, std::make_shared<OpenRecorder>()));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        QCOMPARE(nextApplied(spy, 0).name, kLight);
        QCOMPARE(sha256(dbPath), dbBefore);
        QCOMPARE(sha256(wal), walBefore);
        sqlite3_close(writer);
    }

    // ---------------------------------------------------------------------------------------
    // INV-53: the name read
    // ---------------------------------------------------------------------------------------

    // A value committed only to the WAL (writer still open, autocheckpoint off) is seen.
    void walOnlyValueSeen() {
        activateBuiltIn(kDark);
        const QString dir = newAetherDir();
        const QString dbPath = dir + QStringLiteral("/AetherSDR.db");
        QVERIFY(makeDb(dbPath, kDark));  // checkpointed: the main file says Default Dark
        sqlite3* writer = nullptr;
        QCOMPARE(sqlite3_open_v2(dbPath.toUtf8().constData(), &writer, SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
        QVERIFY(exec(writer, "PRAGMA wal_autocheckpoint=0;"));
        QVERIFY(exec(writer, "UPDATE app_settings SET value='Default Light' WHERE key='ActiveTheme';"));
        // The new value exists only in the WAL: a reader that ignores it would see Default Dark.
        QVERIFY(!readFile(dbPath).contains("Default Light"));
        QVERIFY(readFile(dbPath + QStringLiteral("-wal")).contains("Default Light"));

        ThemeFollower f(optionsFor(dir, std::make_shared<OpenRecorder>()));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        QCOMPARE(nextApplied(spy, 0).name, kLight);
        QCOMPARE(ThemeManager::instance().activeTheme(), kLight);
        sqlite3_close(writer);
    }

    // The wrapper VFS holds one xRead on a gate. The UI applies Default Dark at 750 ms, while the
    // read is still held (so nothing can have been delivered yet); when the gate opens the read
    // returns "Default Light", and that result is discarded. Nothing here is a bound on latency:
    // the held read cannot answer first, so the only way the fallback arrives is the timer.
    void slowVfsTimesOutAndLateResultDiscarded() {
        activateBuiltIn(kDark);
        const QString dir = newAetherDir();
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), kLight));
        auto rec = std::make_shared<OpenRecorder>();
        rec->slow = true;
        ThemeFollower f(optionsFor(dir, rec, 0));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        const int stallsBefore = asb_slowvfs_stalls_done();
        asb_slowvfs_arm_gated(1);
        auto release = qScopeGuard([] { asb_slowvfs_release(); });
        QElapsedTimer t;
        t.start();
        f.start(ThemeFollower::Choice::Follow);
        const Applied a = nextApplied(spy, 0);
        const qint64 at = t.elapsed();
        QVERIFY2(a.ok, "nothing was applied: the fallback timer never fired");
        QCOMPARE(a.name, kDark);
        QVERIFY2(a.reason.contains(QLatin1String("did not answer within 750 ms")), qPrintable(a.reason));
        // Lower bound only: the timer cannot fire early.
        QVERIFY2(at >= 700, qPrintable(QString::number(at)));
        // The fallback came while the read was still held: no result had reached the follower.
        QCOMPARE(ThemeFollower::workersRunning(), 1);
        QCOMPARE(f.discardedLateResults(), 0);
        QCOMPARE(ThemeManager::instance().activeTheme(), kDark);

        asb_slowvfs_release();
        QVERIFY(ThemeFollower::waitUntilIdle(kWaitMs));
        QCOMPARE(asb_slowvfs_stalls_done(), stallsBefore + 1);  // the read did stall
        QCOMPARE(spy.count(), 1);
        QCOMPARE(f.discardedLateResults(), 1);
        QCOMPARE(ThemeManager::instance().activeTheme(), kDark);
    }

    // The deadline is checked when a result is accepted, not only by the deadline timer. The UI
    // thread is busy past the deadline while the read completes; the completion's queued event
    // is then delivered before the timer's (sendPostedEvents runs posted events, never timers).
    // The result is discarded and the fallback applied once; the late value is not last good.
    void resultPastDeadlineDiscardedBeforeTimer() {
        activateBuiltIn(kDark);
        const QString dir = newAetherDir();
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), kLight));
        auto rec = std::make_shared<OpenRecorder>();
        ThemeFollower f(optionsFor(dir, rec, 0));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        const int discardedBefore = f.discardedLateResults();
        f.start(ThemeFollower::Choice::Follow);
        // Measured from after start() returned, so from after the request's deadline was set.
        QElapsedTimer t;
        t.start();
        // Busy: no event processing until the deadline has passed and the worker has posted.
        while ((t.elapsed() < ThemeFollower::kUiDeadlineMs + 150 || ThemeFollower::workersRunning() != 0) &&
               t.elapsed() < 10000) {
            QThread::msleep(10);
        }
        QCOMPARE(ThemeFollower::workersRunning(), 0);
        QCOMPARE(spy.count(), 0);

        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);  // the completion only
        QCOMPARE(spy.count(), 1);
        const Applied a = nextApplied(spy, 0);
        QVERIFY2(a.name == kDark, qPrintable(a.name + QStringLiteral(": ") + a.reason));
        QVERIFY2(a.reason.contains(QLatin1String("did not answer within 750 ms")), qPrintable(a.reason));
        QCOMPARE(f.discardedLateResults() - discardedBefore, 1);
        QCOMPARE(ThemeManager::instance().activeTheme(), kDark);

        // Now the timer runs: nothing is pending any more, so no second fallback.
        QTest::qWait(200);
        QCOMPARE(spy.count(), 1);

        // The discarded value did not become last good: the next timeout falls back to Default
        // Dark, not to "keeping Default Light".
        rec->slow = true;
        asb_slowvfs_arm_gated(1);
        auto release = qScopeGuard([] { asb_slowvfs_release(); });
        f.refresh();
        const Applied next = nextApplied(spy, 1);
        QVERIFY(next.ok);
        QCOMPARE(next.name, kDark);
        QVERIFY2(next.reason.contains(QLatin1String("using Default Dark")), qPrintable(next.reason));
    }

    // A path with characters that mean something in a URI ('#', '?', '%', ' ', '&', '=') is
    // percent-encoded, not concatenated: the database is found and read.
    void uriEncodesSpecialCharacters() {
        activateBuiltIn(kDark);
#ifdef Q_OS_WIN
        const QString tag = QStringLiteral(" a#b %41 &c=d");
#else
        const QString tag = QStringLiteral(" a#b %41 ?x=1&c=d");
#endif
        const QString dir = newAetherDir(tag);
        QVERIFY(!dir.isEmpty());
        const QString dbPath = dir + QStringLiteral("/AetherSDR.db");
        QVERIFY(makeDb(dbPath, kLight));
        auto rec = std::make_shared<OpenRecorder>();
        ThemeFollower f(optionsFor(dir, rec));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        const Applied a = nextApplied(spy, 0);
        QVERIFY2(a.name == kLight, qPrintable(a.reason));
        std::lock_guard<std::mutex> lock(rec->mutex);
        const QString uri = rec->uris.first();
        QCOMPARE(uri, asb::gui::themeDbUri(dbPath));
        QVERIFY(uri.contains(QLatin1String("%23")));
        QVERIFY(uri.contains(QLatin1String("%2541")));
        QVERIFY(uri.contains(QLatin1String("%20")));
        QVERIFY(!uri.contains(QLatin1Char('#')));
        QCOMPARE(uri.count(QLatin1Char('?')), 1);
    }

    // ---------------------------------------------------------------------------------------
    // INV-53: the database URI for drive, POSIX and UNC paths
    // ---------------------------------------------------------------------------------------

    void uriBuilderShapes_data() {
        QTest::addColumn<QString>("slashPath");
        QTest::addColumn<QString>("uri");
        QTest::addColumn<QString>("parsedName");
        const QString e = QString(QChar(0x00e9));
        QTest::newRow("posix") << QStringLiteral("/tmp/asb/AetherSDR.db")
                               << QStringLiteral("file:///tmp/asb/AetherSDR.db?mode=ro")
                               << QStringLiteral("/tmp/asb/AetherSDR.db");
        QTest::newRow("posix special") << QStringLiteral("/tmp/a b/#x/%41/?q=1&r/AetherSDR.db")
                                       << QStringLiteral("file:///tmp/a%20b/%23x/%2541/%3Fq=1&r/AetherSDR.db?mode=ro")
                                       << QStringLiteral("/tmp/a b/#x/%41/?q=1&r/AetherSDR.db");
        QTest::newRow("drive") << QStringLiteral("C:/Users/me/AppData/Roaming/AetherSDR/AetherSDR.db")
                               << QStringLiteral("file:///C:/Users/me/AppData/Roaming/AetherSDR/AetherSDR.db?mode=ro")
                               << QStringLiteral("/C:/Users/me/AppData/Roaming/AetherSDR/AetherSDR.db");
        QTest::newRow("drive special") << QStringLiteral("C:/Users/a b/#x/%41/AetherSDR.db")
                                       << QStringLiteral("file:///C:/Users/a%20b/%23x/%2541/AetherSDR.db?mode=ro")
                                       << QStringLiteral("/C:/Users/a b/#x/%41/AetherSDR.db");
        QTest::newRow("unc") << QStringLiteral("//server/share/AetherSDR/AetherSDR.db")
                             << QStringLiteral("file:////server/share/AetherSDR/AetherSDR.db?mode=ro")
                             << QStringLiteral("//server/share/AetherSDR/AetherSDR.db");
        QTest::newRow("unc special") << QStringLiteral("//my server/sh#are/a b ") + e +
                                            QStringLiteral("/%41/?q=1&r/AetherSDR.db")
                                     << QStringLiteral("file:////my%20server/sh%23are/a%20b%20%C3%A9/%2541/%3Fq=1&r/"
                                                       "AetherSDR.db?mode=ro")
                                     << QStringLiteral("//my server/sh#are/a b ") + e +
                                            QStringLiteral("/%41/?q=1&r/AetherSDR.db");
        QTest::newRow("unc long-path prefix") << QStringLiteral("//?/UNC/server/share/AetherSDR.db")
                                              << QStringLiteral("file:////%3F/UNC/server/share/AetherSDR.db?mode=ro")
                                              << QStringLiteral("//?/UNC/server/share/AetherSDR.db");
    }

    // The URI builder's exact output, and what SQLite's own URI parser makes of it: the file name
    // SQLite hands to the VFS (xFullPathname's input) is the path we started from. A URI with an
    // authority ("file://server/share/...") never gets that far: SQLite refuses it, because it
    // is built without SQLITE_ALLOW_URI_AUTHORITY, and the open returns SQLITE_ERROR.
    void uriBuilderShapes() {
        QFETCH(QString, slashPath);
        QFETCH(QString, uri);
        QFETCH(QString, parsedName);
        QCOMPARE(asb::gui::themeDbUriFromSlashPath(slashPath), uri);
#ifdef Q_OS_WIN
        // Qt's separator conversion turns the long-path spelling "//?/UNC/server/…" into
        // "//server/…" on Windows (checked below); every other row is unchanged by it.
        if (!slashPath.startsWith(QLatin1String("//?/")))
#endif
            QCOMPARE(asb::gui::themeDbUri(slashPath), uri);

        asb_slowvfs_set_record_only(1);
        auto restore = qScopeGuard([] { asb_slowvfs_set_record_only(0); });
        asb_slowvfs_clear_pathname();
        sqlite3* db = nullptr;
        const QByteArray u = uri.toUtf8();
        const int rc = sqlite3_open_v2(u.constData(), &db, asb::gui::themeDbOpenFlags(), ASB_SLOW_VFS_NAME);
        const QString err = db ? QString::fromUtf8(sqlite3_errmsg(db)) : QString();
        sqlite3_close(db);
        QVERIFY2(rc == SQLITE_CANTOPEN, qPrintable(QStringLiteral("rc %1: %2").arg(rc).arg(err)));
        std::array<char, 4096> name{};
        QVERIFY2(asb_slowvfs_last_pathname(name.data(), static_cast<int>(name.size())) != 0,
                 "SQLite never reached the VFS (the URI was rejected)");
        QCOMPARE(QString::fromUtf8(name.data()), parsedName);
    }

    // The shape the old builder produced for a UNC path has an authority; SQLite rejects it. This
    // pins the premise of the fix on SQLite's own parser, not on a reading of the source.
    void uriWithAuthorityIsRefusedBySqlite() {
        asb_slowvfs_set_record_only(1);
        auto restore = qScopeGuard([] { asb_slowvfs_set_record_only(0); });
        asb_slowvfs_clear_pathname();
        sqlite3* db = nullptr;
        const QByteArray u = QUrl::fromLocalFile(QStringLiteral("//server/share/AetherSDR.db"))
                                 .toString(QUrl::FullyEncoded)
                                 .append(QStringLiteral("?mode=ro"))
                                 .toUtf8();
        QCOMPARE(QString::fromUtf8(u), QStringLiteral("file://server/share/AetherSDR.db?mode=ro"));
        const int rc = sqlite3_open_v2(u.constData(), &db, asb::gui::themeDbOpenFlags(), ASB_SLOW_VFS_NAME);
        const QString err = db ? QString::fromUtf8(sqlite3_errmsg(db)) : QString();
        sqlite3_close(db);
        QCOMPARE(rc, static_cast<int>(SQLITE_ERROR));
        QVERIFY2(err.contains(QLatin1String("invalid uri authority: server")), qPrintable(err));
        char name[16];
        QCOMPARE(asb_slowvfs_last_pathname(name, static_cast<int>(sizeof(name))), 0);
    }

#ifdef Q_OS_WIN
    // The backslash spellings of the same paths reach the same URI as their slash forms.
    void uriBuilderBackslashPaths() {
        QCOMPARE(asb::gui::themeDbUri(QStringLiteral("\\\\server\\share\\AetherSDR\\AetherSDR.db")),
                 QStringLiteral("file:////server/share/AetherSDR/AetherSDR.db?mode=ro"));
        QCOMPARE(asb::gui::themeDbUri(QStringLiteral("\\\\my server\\sh#are\\a b\\%41\\AetherSDR.db")),
                 QStringLiteral("file:////my%20server/sh%23are/a%20b/%2541/AetherSDR.db?mode=ro"));
        QCOMPARE(asb::gui::themeDbUri(QStringLiteral("C:\\Users\\me\\AppData\\Roaming\\AetherSDR\\AetherSDR.db")),
                 QStringLiteral("file:///C:/Users/me/AppData/Roaming/AetherSDR/AetherSDR.db?mode=ro"));
        QCOMPARE(asb::gui::themeDbUri(QStringLiteral("C:\\Users\\a b\\#x\\%41\\AetherSDR.db")),
                 QStringLiteral("file:///C:/Users/a%20b/%23x/%2541/AetherSDR.db?mode=ro"));
    }

    // The long-path spelling of a UNC path reaches the plain UNC URI (QDir::fromNativeSeparators
    // drops the "//?/UNC/" prefix on Windows; the observed behaviour is pinned here).
    void uriBuilderLongPathPrefix() {
        QCOMPARE(asb::gui::themeDbUri(QStringLiteral("//?/UNC/server/share/AetherSDR.db")),
                 QStringLiteral("file:////server/share/AetherSDR.db?mode=ro"));
    }

    // A real open through a UNC path: the local administrative share (\\localhost\C$), which a
    // CI runner's admin account can reach without any setup.
    void uncAdminShareOpens_data() {
        QTest::addColumn<int>("shape");
        QTest::newRow("backslash UNC") << 0;
        QTest::newRow("slash UNC") << 1;
        QTest::newRow("long-path UNC") << 2;
    }

    void uncAdminShareOpens() {
        QFETCH(int, shape);
        const QString dir = newAetherDir();
        QVERIFY(!dir.isEmpty());
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), kLight));
        // dir is "<drive>:/..."; the same folder through the drive's administrative share.
        QVERIFY2(dir.size() > 3 && dir.at(1) == QLatin1Char(':'), qPrintable(dir));
        const QString rest = dir.mid(2);  // "/..."
        const QString share = QStringLiteral("localhost/%1$").arg(dir.at(0).toUpper());
        QString dbPath;
        switch (shape) {
        case 0:
            dbPath = QDir::toNativeSeparators(QStringLiteral("//") + share + rest + QStringLiteral("/AetherSDR.db"));
            break;
        case 1:
            dbPath = QStringLiteral("//") + share + rest + QStringLiteral("/AetherSDR.db");
            break;
        default:
            dbPath = QDir::toNativeSeparators(QStringLiteral("//?/UNC/") + share + rest +
                                              QStringLiteral("/AetherSDR.db"));
            break;
        }
        if (!QFileInfo::exists(dbPath)) {
            skipOrFailUnreachableShare(dbPath);
            return;
        }
        const asb::gui::ThemeNameRead r = asb::gui::readActiveThemeName(dbPath, asb::gui::defaultSqliteOpen());
        QVERIFY2(r.status == asb::gui::ThemeNameRead::Status::Ok, qPrintable(dbPath + QStringLiteral(": ") + r.error));
        QCOMPARE(r.name, kLight);
    }

    // The follower itself, with its settings database on a UNC path (a redirected AppData).
    void followerReadsDatabaseOnUncPath() {
        activateBuiltIn(kDark);
        const QString dir = newAetherDir();
        QVERIFY(!dir.isEmpty());
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), kLight));
        QVERIFY2(dir.size() > 3 && dir.at(1) == QLatin1Char(':'), qPrintable(dir));
        const QString uncDir = QDir::toNativeSeparators(QStringLiteral("//localhost/%1$").arg(dir.at(0).toUpper()) +
                                                        dir.mid(2));
        if (!QFileInfo::exists(uncDir + QStringLiteral("\\AetherSDR.db"))) {
            skipOrFailUnreachableShare(uncDir);
            return;
        }
        auto rec = std::make_shared<OpenRecorder>();
        ThemeFollower f(optionsFor(uncDir, rec));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        const Applied a = nextApplied(spy, 0);
        QVERIFY2(a.name == kLight, qPrintable(a.reason));
        std::lock_guard<std::mutex> lock(rec->mutex);
        QVERIFY(rec->uris.first().startsWith(QLatin1String("file:////localhost/")));
    }
#else
    // A path that starts with "//" is a plain path on POSIX (the kernel collapses it), and the old
    // builder turned it into an authority all the same: "file://tmp/...". Real open, real read.
    void leadingDoubleSlashPathOpens() {
        const QString dir = newAetherDir();
        QVERIFY(!dir.isEmpty());
        QVERIFY(dir.startsWith(QLatin1Char('/')));
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), kLight));
        const QString dbPath = QStringLiteral("/") + dir + QStringLiteral("/AetherSDR.db");
        QVERIFY(dbPath.startsWith(QLatin1String("//")));
        QVERIFY(asb::gui::themeDbUri(dbPath).startsWith(QLatin1String("file:////")));
        const asb::gui::ThemeNameRead r = asb::gui::readActiveThemeName(dbPath, asb::gui::defaultSqliteOpen());
        QVERIFY2(r.status == asb::gui::ThemeNameRead::Status::Ok, qPrintable(r.error));
        QCOMPARE(r.name, kLight);
    }
#endif

    void heapLimitSet() {
        const QString dir = newAetherDir();
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), kDark));
        ThemeFollower f(optionsFor(dir, std::make_shared<OpenRecorder>()));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        QVERIFY(nextApplied(spy, 0).ok);
        QCOMPARE(sqlite3_hard_heap_limit64(-1), static_cast<sqlite3_int64>(8 * 1024 * 1024));
    }

    // ---------------------------------------------------------------------------------------
    // INV-85: bounded, nonblocking, regular-file reads
    // ---------------------------------------------------------------------------------------

    void tooManyEntriesNoFollow() {
        activateBuiltIn(kLight);
        const QString dir = newAetherDir();
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), QStringLiteral("ASB Many")));
        QVERIFY(writeFile(dir + QStringLiteral("/themes/many.json"),
                          userTheme(QStringLiteral("ASB Many"), QStringLiteral("#010101"))));
        for (int i = 0; i < 64; ++i) {  // 65 .json entries in all
            QVERIFY(writeFile(dir + QStringLiteral("/themes/other%1.json").arg(i),
                              userTheme(QStringLiteral("ASB Other %1").arg(i), QStringLiteral("#020202"))));
        }
        ThemeFollower f(optionsFor(dir, std::make_shared<OpenRecorder>()));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        const Applied a = nextApplied(spy, 0);
        QCOMPARE(a.name, kDark);
        QVERIFY2(a.reason.contains(QLatin1String("more than 64 theme files")), qPrintable(a.reason));

        // 64 .json entries: followed.
        QVERIFY(QFile::remove(dir + QStringLiteral("/themes/other0.json")));
        QCOMPARE(asb::gui::findUserTheme(dir + QStringLiteral("/themes"), QStringLiteral("ASB Many")).status,
                 ThemeDiscovery::Status::Found);
    }

    // A FIFO named like a theme is opened without blocking and rejected on the handle; the real
    // theme next to it is still followed, well within the UI deadline.
    void fifoRejectedWithoutBlocking() {
#ifndef Q_OS_UNIX
        QSKIP("FIFOs in a directory are a POSIX thing; Windows rejects non-disk handles (GetFileType)");
#else
        activateBuiltIn(kDark);
        const QString dir = newAetherDir();
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), QStringLiteral("ASB Fifo")));
        const QString fifo = dir + QStringLiteral("/themes/aaa-fifo.json");
        QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
        QVERIFY(writeFile(dir + QStringLiteral("/themes/zzz-user.json"),
                          userTheme(QStringLiteral("ASB Fifo"), QStringLiteral("#0f0f0f"))));

        // If the open ever blocks, this releases it after 2.5 s (so the test can finish), and
        // records that it had to. (Time only decides when a stuck run gives up, never a pass.)
        std::atomic<bool> stop{false};
        std::atomic<bool> unblocked{false};
        std::thread releaser([&]() {
            QElapsedTimer t;
            t.start();
            while (!stop && t.elapsed() < 2500) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            for (int i = 0; i < 100 && !stop; ++i) {
                const int fd = ::open(QFile::encodeName(fifo).constData(), O_WRONLY | O_NONBLOCK);
                if (fd >= 0) {
                    unblocked = true;
                    ::close(fd);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });

        ThemeFollower f(optionsFor(dir, std::make_shared<OpenRecorder>()));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        // The outcome is the assertion, not how long it took: a follower that blocked on the FIFO
        // is only freed by the releaser, which records that it had to. The wait outlasts the
        // releaser, so a blocked open ends in `unblocked`, not in a timeout that looks like load.
        const Applied a = nextApplied(spy, 0, 2 * kWaitMs);
        stop = true;
        releaser.join();
        QVERIFY(a.ok);
        QVERIFY2(a.name == QStringLiteral("ASB Fifo"), qPrintable(a.reason));
        QVERIFY2(!unblocked, "the open blocked on the FIFO until the releaser opened it");
#endif
    }

    // Links and directories named like themes are never followed or read as themes.
    void nonRegularEntriesRejected() {
        const QString dir = newAetherDir();
        const QString themes = dir + QStringLiteral("/themes");
        QVERIFY(QDir().mkpath(themes + QStringLiteral("/dir.json")));
        bool linked = false;
#ifdef Q_OS_UNIX
        const QString target = dir + QStringLiteral("/outside.json");
        QVERIFY(writeFile(target, userTheme(QStringLiteral("ASB Linked"), QStringLiteral("#0c0c0c"))));
        QCOMPARE(::symlink(QFile::encodeName(target).constData(),
                           QFile::encodeName(themes + QStringLiteral("/link.json")).constData()),
                 0);
        linked = true;
#endif
        const ThemeDiscovery d = asb::gui::findUserTheme(themes, QStringLiteral("ASB Linked"));
        QCOMPARE(d.status, ThemeDiscovery::Status::NotFound);
        QVERIFY2(d.detail.contains(QLatin1String("dir.json")), qPrintable(d.detail));
        if (linked) {
            QVERIFY2(d.detail.contains(QLatin1String("link.json: a link")), qPrintable(d.detail));
        }
    }

    // A Unix-domain socket named like a theme fails the open (ENXIO), but it is not a regular
    // file, so it does not count: the one real theme next to it is followed, not "ambiguous".
    void socketNamedLikeThemeDoesNotCount() {
#ifndef Q_OS_UNIX
        QSKIP("Unix-domain sockets in a directory are a POSIX thing here (see ThemeDiscovery.h for Windows)");
#else
        activateBuiltIn(kLight);
        const QString dir = newAetherDir();
        const QString themes = dir + QStringLiteral("/themes");
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), QStringLiteral("ASB Sock")));
        QVERIFY(writeFile(themes + QStringLiteral("/real.json"),
                          userTheme(QStringLiteral("ASB Sock"), QStringLiteral("#0c0d0e"))));
        const QByteArray sockPath = QFile::encodeName(themes + QStringLiteral("/sock.json"));
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        QVERIFY2(static_cast<size_t>(sockPath.size()) < sizeof(addr.sun_path), sockPath.constData());
        std::memcpy(addr.sun_path, sockPath.constData(), static_cast<size_t>(sockPath.size()));
        const int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
        QVERIFY(sock >= 0);
        const auto closeSock = qScopeGuard([sock]() { ::close(sock); });
        QCOMPARE(::bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0);
        struct stat st {};
        QCOMPARE(::lstat(sockPath.constData(), &st), 0);
        QVERIFY(S_ISSOCK(st.st_mode));
        // The open the discovery makes really does fail on it.
        QCOMPARE(::open(sockPath.constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC), -1);

        const ThemeDiscovery d = asb::gui::findUserTheme(themes, QStringLiteral("ASB Sock"));
        QVERIFY2(d.status == ThemeDiscovery::Status::Found, qPrintable(d.detail));
        QVERIFY2(d.detail.contains(QLatin1String("sock.json: not a regular file")), qPrintable(d.detail));

        ThemeFollower f(optionsFor(dir, std::make_shared<OpenRecorder>()));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        const Applied a = nextApplied(spy, 0);
        QVERIFY2(a.name == QStringLiteral("ASB Sock"), qPrintable(a.reason));
#endif
    }

    // The file grows past 256 KiB between the open (when fstat said it was small) and the read:
    // the cap is on what the handle returns, so it is rejected. The growth is whitespace, so the
    // grown file is still a valid theme of the right name.
    void growingFileCapped() {
        activateBuiltIn(kLight);
        const QString dir = newAetherDir();
        const QString file = dir + QStringLiteral("/themes/grow.json");
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), QStringLiteral("ASB Grow")));
        QVERIFY(writeFile(file, userTheme(QStringLiteral("ASB Grow"), QStringLiteral("#030303"))));
        std::atomic<int> grown{0};
        auto grow = [&grown, file](const QString& path) {
            if (path != file) {
                return;
            }
            QFile f(file);
            if (f.open(QIODevice::Append)) {
                f.write(QByteArray(300 * 1024, ' '));
                ++grown;
            }
        };

        // Discovery: the grown file is skipped as too large; no bytes come back.
        const ThemeDiscovery d =
            asb::gui::findUserTheme(dir + QStringLiteral("/themes"), QStringLiteral("ASB Grow"), {grow, {}});
        QCOMPARE(grown.load(), 1);
        QCOMPARE(d.status, ThemeDiscovery::Status::NotFound);
        QVERIFY(d.bytes.isEmpty());
        QVERIFY2(d.detail.contains(QLatin1String("larger than 256 KiB")), qPrintable(d.detail));

        // The follower: not found (rejected by the read cap), not "failed validation".
        QVERIFY(writeFile(file, userTheme(QStringLiteral("ASB Grow"), QStringLiteral("#030303"))));
        auto opts = optionsFor(dir, std::make_shared<OpenRecorder>());
        opts.afterThemeFileOpened = grow;
        ThemeFollower f(opts);
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        const Applied a = nextApplied(spy, 0);
        QCOMPARE(grown.load(), 2);
        QCOMPARE(a.name, kDark);
        QVERIFY2(a.reason.contains(QLatin1String("was not found")), qPrintable(a.reason));
        // Valid JSON after growth: only the cap stood between it and being followed.
        QVERIFY(QJsonDocument::fromJson(readFile(file)).isObject());
    }

    // Every candidate is examined: two files claiming the active name are ambiguous, wherever
    // they sit in the listing.
    void duplicateNamesAcrossAllRejected() {
        activateBuiltIn(kLight);
        const QString dir = newAetherDir();
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), QStringLiteral("ASB Dup")));
        QVERIFY(writeFile(dir + QStringLiteral("/themes/aaa.json"),
                          userTheme(QStringLiteral("ASB Dup"), QStringLiteral("#040404"))));
        for (int i = 0; i < 20; ++i) {
            QVERIFY(writeFile(dir + QStringLiteral("/themes/mid%1.json").arg(i),
                              userTheme(QStringLiteral("ASB Mid %1").arg(i), QStringLiteral("#050505"))));
        }
        QVERIFY(writeFile(dir + QStringLiteral("/themes/zzz.json"),
                          userTheme(QStringLiteral("ASB Dup"), QStringLiteral("#060606"))));
        ThemeFollower f(optionsFor(dir, std::make_shared<OpenRecorder>()));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        const Applied a = nextApplied(spy, 0);
        QCOMPARE(a.name, kDark);
        QVERIFY2(a.reason.contains(QLatin1String("ambiguous")), qPrintable(a.reason));
        QVERIFY(a.reason.contains(QLatin1String("aaa.json")) && a.reason.contains(QLatin1String("zzz.json")));
    }

    // One file has the active name and a second candidate that could not be examined might have
    // it too (AetherSDR loads files of any size): a single match is not trusted, it is ambiguous.
    // A directory named like a theme does not count (AetherSDR never loads one).
    void oversizedDuplicateIsAmbiguous() {
        activateBuiltIn(kLight);
        const QString dir = newAetherDir();
        const QString themes = dir + QStringLiteral("/themes");
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), QStringLiteral("ASB Big")));
        QVERIFY(writeFile(themes + QStringLiteral("/small.json"),
                          userTheme(QStringLiteral("ASB Big"), QStringLiteral("#080808"))));
        QVERIFY(QDir().mkpath(themes + QStringLiteral("/folder.json")));
        // Valid JSON of the same name, over the cap only by trailing whitespace.
        const QByteArray big = userTheme(QStringLiteral("ASB Big"), QStringLiteral("#090909")) +
                               QByteArray(300 * 1024, ' ');
        QVERIFY(QJsonDocument::fromJson(big).isObject());
        QVERIFY(writeFile(themes + QStringLiteral("/big.json"), big));

        const ThemeDiscovery d = asb::gui::findUserTheme(themes, QStringLiteral("ASB Big"));
        QCOMPARE(d.status, ThemeDiscovery::Status::Ambiguous);
        QVERIFY(d.bytes.isEmpty());
        QVERIFY2(d.detail.contains(QLatin1String("small.json has that name")), qPrintable(d.detail));
        QVERIFY2(d.detail.contains(QLatin1String("big.json (larger than 256 KiB)")), qPrintable(d.detail));
        QVERIFY2(!d.detail.contains(QLatin1String("folder.json")), qPrintable(d.detail));

        ThemeFollower f(optionsFor(dir, std::make_shared<OpenRecorder>()));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        const Applied a = nextApplied(spy, 0);
        QCOMPARE(a.name, kDark);
        QVERIFY2(a.reason.contains(QLatin1String("ambiguous")) && a.reason.contains(QLatin1String("big.json")),
                 qPrintable(a.reason));

        // Without the oversized file: the directory alone does not make it ambiguous.
        QVERIFY(QFile::remove(themes + QStringLiteral("/big.json")));
        QCOMPARE(asb::gui::findUserTheme(themes, QStringLiteral("ASB Big")).status, ThemeDiscovery::Status::Found);
    }

    // The same with a link: it is not followed, so its name is unknown, and AetherSDR follows
    // links, so it may be the file AetherSDR shows.
    void linkedDuplicateIsAmbiguous() {
#ifndef Q_OS_UNIX
        QSKIP("creating a symbolic link needs a privilege on Windows");
#else
        activateBuiltIn(kLight);
        const QString dir = newAetherDir();
        const QString themes = dir + QStringLiteral("/themes");
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), QStringLiteral("ASB Twin")));
        QVERIFY(writeFile(themes + QStringLiteral("/real.json"),
                          userTheme(QStringLiteral("ASB Twin"), QStringLiteral("#0b0b0b"))));
        const QString target = dir + QStringLiteral("/elsewhere.json");
        QVERIFY(writeFile(target, userTheme(QStringLiteral("ASB Twin"), QStringLiteral("#0e0e0e"))));
        QCOMPARE(::symlink(QFile::encodeName(target).constData(),
                           QFile::encodeName(themes + QStringLiteral("/twin.json")).constData()),
                 0);

        const ThemeDiscovery d = asb::gui::findUserTheme(themes, QStringLiteral("ASB Twin"));
        QCOMPARE(d.status, ThemeDiscovery::Status::Ambiguous);
        QVERIFY2(d.detail.contains(QLatin1String("twin.json (a link)")), qPrintable(d.detail));

        ThemeFollower f(optionsFor(dir, std::make_shared<OpenRecorder>()));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        const Applied a = nextApplied(spy, 0);
        QCOMPARE(a.name, kDark);
        QVERIFY2(a.reason.contains(QLatin1String("ambiguous")) && a.reason.contains(QLatin1String("twin.json")),
                 qPrintable(a.reason));

        QVERIFY(QFile::remove(themes + QStringLiteral("/twin.json")));
        QCOMPARE(asb::gui::findUserTheme(themes, QStringLiteral("ASB Twin")).status, ThemeDiscovery::Status::Found);
#endif
    }

    // The listing fails right after the first ".json" name is listed (whichever of the two it
    // is): what was listed holds exactly one match, but the folder holds two. An incomplete
    // listing establishes neither uniqueness nor the budgets, so nothing is followed, on the
    // POSIX (readdir) and Windows (FindNextFileW) paths alike. Without the fault the same folder
    // is ambiguous, and with one file it is found.
    void incompleteListingNeverFollowed() {
        activateBuiltIn(kLight);
        const QString dir = newAetherDir();
        const QString themes = dir + QStringLiteral("/themes");
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), QStringLiteral("ASB Partial")));
        QVERIFY(writeFile(themes + QStringLiteral("/one.json"),
                          userTheme(QStringLiteral("ASB Partial"), QStringLiteral("#0d0d0d"))));
        QVERIFY(writeFile(themes + QStringLiteral("/two.json"),
                          userTheme(QStringLiteral("ASB Partial"), QStringLiteral("#0a0a0a"))));
        std::atomic<int> injected{0};
        const auto failAfterFirstJson = [&injected](int jsonListed) {
            if (jsonListed == 1) {
                ++injected;
                return true;
            }
            return false;
        };
        asb::gui::ThemeDiscoveryHooks hooks;
        hooks.failListing = failAfterFirstJson;

        const ThemeDiscovery d = asb::gui::findUserTheme(themes, QStringLiteral("ASB Partial"), hooks);
        QCOMPARE(injected.load(), 1);
        QCOMPARE(d.status, ThemeDiscovery::Status::Error);
        QVERIFY(d.bytes.isEmpty());
        QVERIFY2(d.detail.contains(QLatin1String("listing the theme folder stopped early")), qPrintable(d.detail));
        QCOMPARE(asb::gui::findUserTheme(themes, QStringLiteral("ASB Partial")).status,
                 ThemeDiscovery::Status::Ambiguous);

        // Through the follower: Default Dark, and the reason says why.
        auto opts = optionsFor(dir, std::make_shared<OpenRecorder>());
        opts.failThemeListing = failAfterFirstJson;
        {
            ThemeFollower f(opts);
            QSignalSpy spy(&f, &ThemeFollower::applied);
            f.start(ThemeFollower::Choice::Follow);
            const Applied a = nextApplied(spy, 0);
            QCOMPARE(a.name, kDark);
            QVERIFY2(a.reason.contains(QLatin1String("Could not read AetherSDR's theme folder (listing the theme "
                                                     "folder stopped early")),
                     qPrintable(a.reason));
        }

        // One file only: the listed part holds the one match and it is the right file, yet an
        // incomplete listing still cannot tell, so it is not followed; without the fault it is.
        QVERIFY(QFile::remove(themes + QStringLiteral("/two.json")));
        QCOMPARE(asb::gui::findUserTheme(themes, QStringLiteral("ASB Partial"), hooks).status,
                 ThemeDiscovery::Status::Error);
        QCOMPARE(asb::gui::findUserTheme(themes, QStringLiteral("ASB Partial")).status, ThemeDiscovery::Status::Found);
    }

    // Class audit (round 2): every way discovery could say "exactly one" without having
    // examined every candidate. A candidate that vanishes between the listing and its open could
    // not be examined, so the one match next to it is not trusted (the next refresh, with the
    // folder settled, finds it).
    void vanishedDuplicateIsAmbiguous() {
        const QString dir = newAetherDir();
        const QString themes = dir + QStringLiteral("/themes");
        const QString a = themes + QStringLiteral("/a.json");
        const QString b = themes + QStringLiteral("/b.json");
        QVERIFY(writeFile(a, userTheme(QStringLiteral("ASB Gone"), QStringLiteral("#101010"))));
        QVERIFY(writeFile(b, userTheme(QStringLiteral("ASB Gone"), QStringLiteral("#111111"))));
        int removed = 0;
        // The first file opened removes the other one, which is then not there to open.
        auto removeOther = [&](const QString& path) {
            if (removed == 0) {
                removed = QFile::remove(path == a ? b : a) ? 1 : -1;
            }
        };
        const ThemeDiscovery d = asb::gui::findUserTheme(themes, QStringLiteral("ASB Gone"), {removeOther, {}});
        QCOMPARE(removed, 1);
        QVERIFY2(d.status == ThemeDiscovery::Status::Ambiguous, qPrintable(d.detail));
        QVERIFY2(d.detail.contains(QLatin1String("(cannot open)")), qPrintable(d.detail));
        QCOMPARE(asb::gui::findUserTheme(themes, QStringLiteral("ASB Gone")).status, ThemeDiscovery::Status::Found);
    }

    // AetherSDR lists themes with QDir::entryList({"*.json"}, QDir::Files): no QDir::Hidden, so a
    // hidden duplicate (a backup, say) is invisible to it, and must not make the visible theme
    // "ambiguous". Hidden is Qt's notion: a leading dot on Unix; on Windows only the hidden
    // attribute (a leading dot is an ordinary name there, so the same folder IS ambiguous).
    // FILE_ATTRIBUTE_SYSTEM is ignored by Qt (no QDir::System still lists files), so a duplicate
    // carrying it still counts.
    void hiddenDuplicateIsNotACandidate() {
        activateBuiltIn(kLight);
        const QString dir = newAetherDir();
        const QString themes = dir + QStringLiteral("/themes");
        const QString name = QStringLiteral("ASB Hidden");
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), name));
        QVERIFY(writeFile(themes + QStringLiteral("/Foo.json"), userTheme(name, QStringLiteral("#0a0b0c"))));
        QVERIFY(writeFile(themes + QStringLiteral("/.Foo.json"), userTheme(name, QStringLiteral("#0c0b0a"))));

        const auto follows = [&]() {
            ThemeFollower f(optionsFor(dir, std::make_shared<OpenRecorder>()));
            QSignalSpy spy(&f, &ThemeFollower::applied);
            f.start(ThemeFollower::Choice::Follow);
            return nextApplied(spy, 0);
        };

        const ThemeDiscovery d = asb::gui::findUserTheme(themes, name);
#ifdef Q_OS_WIN
        QVERIFY2(d.status == ThemeDiscovery::Status::Ambiguous, qPrintable(d.detail));
        QVERIFY(QFile::remove(themes + QStringLiteral("/.Foo.json")));

        // The hidden attribute: AetherSDR does not list it, so the visible theme is followed.
        const QString attr = themes + QStringLiteral("/attr.json");
        QVERIFY(writeFile(attr, userTheme(name, QStringLiteral("#0d0e0f"))));
        const std::wstring native = QDir::toNativeSeparators(attr).toStdWString();
        const auto resetAttributes = qScopeGuard([native]() { SetFileAttributesW(native.c_str(), FILE_ATTRIBUTE_NORMAL); });
        QVERIFY(SetFileAttributesW(native.c_str(), FILE_ATTRIBUTE_HIDDEN));
        QVERIFY((GetFileAttributesW(native.c_str()) & FILE_ATTRIBUTE_HIDDEN) != 0);
        const ThemeDiscovery h = asb::gui::findUserTheme(themes, name);
        QVERIFY2(h.status == ThemeDiscovery::Status::Found, qPrintable(h.detail));
        QVERIFY2(h.path.endsWith(QLatin1String("/Foo.json")), qPrintable(h.path));
        QVERIFY2(h.detail.contains(QLatin1String("attr.json: hidden")), qPrintable(h.detail));
        const Applied a = follows();
        QVERIFY2(a.name == name, qPrintable(a.reason));

        // The system attribute alone: AetherSDR lists it, so it is a second file of that name.
        QVERIFY(SetFileAttributesW(native.c_str(), FILE_ATTRIBUTE_SYSTEM));
        const ThemeDiscovery sys = asb::gui::findUserTheme(themes, name);
        QVERIFY2(sys.status == ThemeDiscovery::Status::Ambiguous, qPrintable(sys.detail));
#else
        QVERIFY2(d.status == ThemeDiscovery::Status::Found, qPrintable(d.detail));
        QVERIFY2(d.path.endsWith(QLatin1String("/Foo.json")), qPrintable(d.path));
        QVERIFY2(d.detail.contains(QLatin1String(".Foo.json: hidden")), qPrintable(d.detail));
        const Applied a = follows();
        QVERIFY2(a.name == name, qPrintable(a.reason));

        // A hidden link is dropped before it is looked at as a link.
        const QString target = dir + QStringLiteral("/elsewhere.json");
        QVERIFY(writeFile(target, userTheme(name, QStringLiteral("#0e0e0e"))));
        QCOMPARE(::symlink(QFile::encodeName(target).constData(),
                           QFile::encodeName(themes + QStringLiteral("/.link.json")).constData()),
                 0);
        const ThemeDiscovery l = asb::gui::findUserTheme(themes, name);
        QVERIFY2(l.status == ThemeDiscovery::Status::Found, qPrintable(l.detail));
        QVERIFY2(l.detail.contains(QLatin1String(".link.json: hidden")), qPrintable(l.detail));

        // Made visible, the same duplicate is a second file of that name.
        QVERIFY(QFile::rename(themes + QStringLiteral("/.Foo.json"), themes + QStringLiteral("/Bar.json")));
        QCOMPARE(asb::gui::findUserTheme(themes, name).status, ThemeDiscovery::Status::Ambiguous);
#endif
    }

    // AetherSDR's listing resolves links and, without QDir::System, drops one whose target does
    // not resolve (a dangling link, a loop); without QDir::Dirs it drops a link to a folder. None
    // of those can be the theme AetherSDR shows, so none makes the one real match ambiguous.
    void brokenLinkDoesNotCount() {
#ifndef Q_OS_UNIX
        QSKIP("creating a symbolic link needs a privilege on Windows");
#else
        activateBuiltIn(kLight);
        const QString dir = newAetherDir();
        const QString themes = dir + QStringLiteral("/themes");
        const QString name = QStringLiteral("ASB Dangling");
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), name));
        QVERIFY(writeFile(themes + QStringLiteral("/real.json"), userTheme(name, QStringLiteral("#0f0e0d"))));
        const auto link = [&](const QString& target, const QString& linkName) {
            return ::symlink(QFile::encodeName(target).constData(),
                             QFile::encodeName(themes + QLatin1Char('/') + linkName).constData()) == 0;
        };
        QVERIFY(link(dir + QStringLiteral("/missing.json"), QStringLiteral("broken.json")));
        QVERIFY(link(themes + QStringLiteral("/loop.json"), QStringLiteral("loop.json")));
        QVERIFY(QDir().mkpath(dir + QStringLiteral("/folder")));
        QVERIFY(link(dir + QStringLiteral("/folder"), QStringLiteral("folder.json")));
        struct stat st {};
        QCOMPARE(::lstat(QFile::encodeName(themes + QStringLiteral("/broken.json")).constData(), &st), 0);
        QVERIFY(S_ISLNK(st.st_mode));
        QVERIFY(::stat(QFile::encodeName(themes + QStringLiteral("/broken.json")).constData(), &st) != 0);

        const ThemeDiscovery d = asb::gui::findUserTheme(themes, name);
        QVERIFY2(d.status == ThemeDiscovery::Status::Found, qPrintable(d.detail));
        QVERIFY2(d.detail.contains(QLatin1String("broken.json: a broken link")), qPrintable(d.detail));
        QVERIFY2(d.detail.contains(QLatin1String("loop.json: a broken link")), qPrintable(d.detail));
        QVERIFY2(d.detail.contains(QLatin1String("folder.json: a link to a folder")), qPrintable(d.detail));

        ThemeFollower f(optionsFor(dir, std::make_shared<OpenRecorder>()));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        const Applied a = nextApplied(spy, 0);
        QVERIFY2(a.name == name, qPrintable(a.reason));

        // The dangling link's target appears, as another file of the same name: AetherSDR would
        // now list it, so the bridge (which does not follow it) cannot say which one is shown.
        QVERIFY(writeFile(dir + QStringLiteral("/missing.json"), userTheme(name, QStringLiteral("#0d0e0f"))));
        const ThemeDiscovery r = asb::gui::findUserTheme(themes, name);
        QVERIFY2(r.status == ThemeDiscovery::Status::Ambiguous, qPrintable(r.detail));
        QVERIFY2(r.detail.contains(QLatin1String("broken.json (a link)")), qPrintable(r.detail));
#endif
    }

    // A candidate this process may not read could still be one AetherSDR shows (another user,
    // another ACL): the one readable match next to it is not trusted.
    void unreadableDuplicateIsAmbiguous() {
#ifndef Q_OS_UNIX
        QSKIP("denying read access to the owner is a POSIX mode bit here");
#else
        if (::geteuid() == 0) {
            QSKIP("root reads a mode-000 file");
        }
        const QString dir = newAetherDir();
        const QString themes = dir + QStringLiteral("/themes");
        QVERIFY(writeFile(themes + QStringLiteral("/open.json"),
                          userTheme(QStringLiteral("ASB Locked"), QStringLiteral("#121212"))));
        const QString locked = themes + QStringLiteral("/locked.json");
        QVERIFY(writeFile(locked, userTheme(QStringLiteral("ASB Locked"), QStringLiteral("#131313"))));
        QCOMPARE(::chmod(QFile::encodeName(locked).constData(), 0), 0);
        const auto restore = qScopeGuard([&]() { ::chmod(QFile::encodeName(locked).constData(), 0600); });
        const ThemeDiscovery d = asb::gui::findUserTheme(themes, QStringLiteral("ASB Locked"));
        QVERIFY2(d.status == ThemeDiscovery::Status::Ambiguous, qPrintable(d.detail));
        QVERIFY2(d.detail.contains(QLatin1String("locked.json (cannot open)")), qPrintable(d.detail));
#endif
    }

    // Names that differ only in case are two files on a case-sensitive filesystem: both are
    // listed (".JSON" matches, like AetherSDR's filter) and both are examined.
    void caseVariantFileNamesBothExamined() {
        const QString dir = newAetherDir();
        const QString themes = dir + QStringLiteral("/themes");
        QVERIFY(writeFile(themes + QStringLiteral("/Twin.json"),
                          userTheme(QStringLiteral("ASB Case"), QStringLiteral("#141414"))));
        QVERIFY(writeFile(themes + QStringLiteral("/twin.JSON"),
                          userTheme(QStringLiteral("ASB Case"), QStringLiteral("#151515"))));
        if (QDir(themes).entryList(QDir::Files).size() != 2) {
            QSKIP("this filesystem folds case: the two names are one file");
        }
        const ThemeDiscovery d = asb::gui::findUserTheme(themes, QStringLiteral("ASB Case"));
        QVERIFY2(d.status == ThemeDiscovery::Status::Ambiguous, qPrintable(d.detail));
        QVERIFY2(d.detail.contains(QLatin1String("Twin.json")) && d.detail.contains(QLatin1String("twin.JSON")),
                 qPrintable(d.detail));
    }

    // Theme names are compared exactly, as AetherSDR's own lookup does (a QMap keyed by the
    // name): a name in another Unicode normalisation, or another case, is a different theme, so
    // it neither matches nor makes the real one ambiguous.
    void lookalikeThemeNamesAreDistinct() {
        const QString dir = newAetherDir();
        const QString themes = dir + QStringLiteral("/themes");
        const QString nfc = QStringLiteral("ASB Café");
        const QString nfd = QStringLiteral("ASB Café");
        QVERIFY(nfc != nfd);
        QCOMPARE(nfd.normalized(QString::NormalizationForm_C), nfc);
        QVERIFY(writeFile(themes + QStringLiteral("/nfc.json"), userTheme(nfc, QStringLiteral("#161616"))));
        QVERIFY(writeFile(themes + QStringLiteral("/nfd.json"), userTheme(nfd, QStringLiteral("#171717"))));
        QVERIFY(writeFile(themes + QStringLiteral("/upper.json"),
                          userTheme(QStringLiteral("ASB CAFÉ"), QStringLiteral("#181818"))));
        const ThemeDiscovery d = asb::gui::findUserTheme(themes, nfc);
        QVERIFY2(d.status == ThemeDiscovery::Status::Found, qPrintable(d.detail));
        QVERIFY(d.path.endsWith(QLatin1String("/nfc.json")));
        const ThemeDiscovery e = asb::gui::findUserTheme(themes, nfd);
        QVERIFY2(e.status == ThemeDiscovery::Status::Found, qPrintable(e.detail));
        QVERIFY(e.path.endsWith(QLatin1String("/nfd.json")));
    }

    // ---------------------------------------------------------------------------------------
    // INV-95: budgets and single flight
    // ---------------------------------------------------------------------------------------

    // 300 non-JSON entries next to a valid theme of the active name: enumeration stops at the
    // 257th entry and nothing is followed. With 256 entries in all, the theme is followed.
    void enumerationBudgetStops() {
        activateBuiltIn(kLight);
        const QString dir = newAetherDir();
        const QString themes = dir + QStringLiteral("/themes");
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), QStringLiteral("ASB Budget")));
        QVERIFY(writeFile(themes + QStringLiteral("/budget.json"),
                          userTheme(QStringLiteral("ASB Budget"), QStringLiteral("#070707"))));
        for (int i = 0; i < 300; ++i) {
            QVERIFY(writeFile(themes + QStringLiteral("/note%1.txt").arg(i, 3, 10, QLatin1Char('0')), "x"));
        }
        ThemeFollower f(optionsFor(dir, std::make_shared<OpenRecorder>()));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        const Applied a = nextApplied(spy, 0);
        QCOMPARE(a.name, kDark);
        QVERIFY2(a.reason.contains(QLatin1String("too many entries in the theme folder to check safely")),
                 qPrintable(a.reason));

        // Down to 256 entries (255 notes + the theme): within budget.
        for (int i = 255; i < 300; ++i) {
            QVERIFY(QFile::remove(themes + QStringLiteral("/note%1.txt").arg(i, 3, 10, QLatin1Char('0'))));
        }
        QCOMPARE(asb::gui::findUserTheme(themes, QStringLiteral("ASB Budget")).status, ThemeDiscovery::Status::Found);
        QVERIFY(writeFile(themes + QStringLiteral("/note255.txt"), "x"));  // 257
        QCOMPARE(asb::gui::findUserTheme(themes, QStringLiteral("ASB Budget")).status,
                 ThemeDiscovery::Status::TooManyEntries);
    }

    // 20 refreshes while one read is stalled in the VFS: exactly one worker thread exists.
    void repeatedRefreshUnderStallSpawnsOneWorker() {
        const QString dir = newAetherDir();
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), kLight));
        auto rec = std::make_shared<OpenRecorder>();
        rec->slow = true;
        ThemeFollower f(optionsFor(dir, rec));
        const int startedBefore = ThemeFollower::workersStarted();
        const int stallsBefore = asb_slowvfs_stalls_done();
        asb_slowvfs_arm_gated(1);
        auto release = qScopeGuard([] { asb_slowvfs_release(); });
        f.start(ThemeFollower::Choice::Follow);
        // The read is held on the gate from the moment it starts to read, for as long as this test
        // needs; wait for it to be there before counting anything.
        QTRY_COMPARE_WITH_TIMEOUT(asb_slowvfs_stalls_done(), stallsBefore + 1, kWaitMs);
        for (int i = 1; i < 20; ++i) {
            QTest::qWait(40);
            f.refresh();
        }
        QCOMPARE(ThemeFollower::workersStarted() - startedBefore, 1);
        QCOMPARE(ThemeFollower::workersRunning(), 1);
#ifdef Q_OS_LINUX
        QCOMPARE(threadsNamed("asb-theme-read"), 1);
#endif
        {
            std::lock_guard<std::mutex> lock(rec->mutex);
            QCOMPARE(rec->flags.size(), 1);  // one database open
        }
        asb_slowvfs_release();
        QVERIFY(ThemeFollower::waitUntilIdle(kWaitMs));
        QCOMPARE(ThemeFollower::workersRunning(), 0);
    }

    // Once the outstanding read has been stalled for more than 5 s, a refresh stops waiting,
    // starts nothing, and applies the last good result at once.
    void stalledReadUsesLastGood() {
        activateBuiltIn(kDark);
        const QString dir = newAetherDir();
        QVERIFY(makeDb(dir + QStringLiteral("/AetherSDR.db"), kLight));
        auto rec = std::make_shared<OpenRecorder>();
        // Setting up the last good result is not about the deadline; the 750 ms comes in after.
        ThemeFollower f(optionsFor(dir, rec));
        QSignalSpy spy(&f, &ThemeFollower::applied);
        f.start(ThemeFollower::Choice::Follow);
        QCOMPARE(nextApplied(spy, 0).name, kLight);  // the last good result
        QVERIFY(ThemeFollower::waitUntilIdle(kWaitMs));
        f.setUiDeadlineMs(0);

        rec->slow = true;
        asb_slowvfs_arm_gated(1);  // held for as long as the test needs: 5 s is a wait, not a race
        auto release = qScopeGuard([] { asb_slowvfs_release(); });
        const int startedBefore = ThemeFollower::workersStarted();
        f.refresh();
        // The read was started inside refresh(); 5 s from here is later than 5 s from when it started.
        QElapsedTimer sinceRefresh;
        sinceRefresh.start();
        const Applied timedOut = nextApplied(spy, 1);
        QVERIFY(timedOut.ok);
        QCOMPARE(timedOut.name, kLight);
        QVERIFY2(timedOut.reason.contains(QLatin1String("did not answer within 750 ms")), qPrintable(timedOut.reason));
        // Lower bound only (the read must have been outstanding for more than 5 s): wait it out.
        while (sinceRefresh.elapsed() <= ThemeFollower::kStalledAfterMs + 100) {
            QTest::qWait(50);
        }
        QCOMPARE(ThemeFollower::workersRunning(), 1);  // still held: it is the stall the rule is about

        const int before = spy.count();
        f.refresh();
        // Applied inside refresh() itself, not after a 750 ms deadline: no event loop ran.
        QCOMPARE(spy.count(), before + 1);
        const Applied stalled = nextApplied(spy, before);
        QVERIFY(stalled.ok);
        QCOMPARE(stalled.name, kLight);
        QVERIFY2(stalled.reason.contains(QLatin1String("more than 5 s")), qPrintable(stalled.reason));
        QCOMPARE(ThemeFollower::workersStarted() - startedBefore, 1);
        asb_slowvfs_release();
        QVERIFY(ThemeFollower::waitUntilIdle(kWaitMs));
    }
};

QTEST_MAIN(TstThemeFollower)
#include "tst_ThemeFollower.moc"
