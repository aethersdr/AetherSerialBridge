// Patches P1 and P2 on the vendored ThemeManager (master §3.4, WS-2).
//
// INV-52: loaded theme bytes = validated bytes; no ThemeManager user-dir scan.
// INV-84: theme activation clears all previously loaded state.
//
// Two layers:
//   in-process  the ThemeManager singleton of this process, constructed in initTestCase() after
//               valid themes were planted in the test-mode user theme directory AND in the
//               working directory; then driven through activateValidatedTheme().
//   probe       asb_theme_probe (the bridge's host) and asb_theme_probe_aetherhost (AetherSDR's
//               host, the positive control) each construct one ThemeManager in a fresh process.
//               On Linux they run under strace, and the syscalls between the probe's two markers
//               are what ThemeManager did: open observation (N4), not a mocked filesystem. strace
//               is REQUIRED on Linux; its absence fails the test rather than skipping it.

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "support/ThemeFixtures.h"

using AetherSDR::AppSettings;
using AetherSDR::ThemeManager;
using namespace asb::test;

namespace {

const QStringList kBuiltIns{QStringLiteral("Default Dark"), QStringLiteral("Default Light")};

bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QByteArray sha256(const QByteArray& b) {
    return QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex();
}

QStringList sorted(QStringList l) {
    l.sort(Qt::CaseInsensitive);
    return l;
}

// Every root token's resolved value, plus the scope tree: what "the loaded theme" is.
QMap<QString, QString> snapshot(const ThemeManager& tm) {
    QMap<QString, QString> s;
    for (const QString& key : tm.allTokenKeys()) {
        s.insert(key, tm.value(key) + QLatin1Char('|') + tm.cssFragment(key));
    }
    s.insert(QStringLiteral("<containers>"), sorted(tm.containerPaths()).join(QLatin1Char(',')));
    return s;
}

struct ProbeRun {
    bool ok = false;
    QString error;
    QJsonObject json;
    QStringList window;  // strace lines between the markers, same thread (Linux only)
    bool traced = false;
};

// Runs a probe in a fresh sandbox (HOME, settings dir and working directory of its own); on
// Linux under strace.
ProbeRun runProbe(const QString& exe, const QString& mode, const QString& sandbox) {
    ProbeRun r;
    if (exe.isEmpty() || !QFileInfo::exists(exe)) {
        r.error = QStringLiteral("probe executable not found: '%1'").arg(exe);
        return r;
    }
    const QString cwd = sandbox + QStringLiteral("/cwd");
    QDir().mkpath(cwd);
    QDir().mkpath(sandbox + QStringLiteral("/home"));
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    env.insert(QStringLiteral("AETHER_SETTINGS_DIR"), sandbox + QStringLiteral("/settings"));
#ifndef Q_OS_WIN
    env.insert(QStringLiteral("HOME"), sandbox + QStringLiteral("/home"));
    env.remove(QStringLiteral("XDG_CONFIG_HOME"));
#endif
    QProcess p;
    p.setProcessEnvironment(env);
    p.setWorkingDirectory(cwd);
    const QString log = sandbox + QStringLiteral("/strace.log");
#ifdef Q_OS_LINUX
    const QString strace = QStandardPaths::findExecutable(QStringLiteral("strace"));
    if (strace.isEmpty()) {
        r.error = QStringLiteral("strace is required on Linux (INV-52 open observation); install it");
        return r;
    }
    r.traced = true;
    // -s 65535: full path strings (the default 32 would truncate every temp path, and a "path not
    // seen" assertion would then pass vacuously). -y: fd arguments printed with their paths, so a
    // getdents64 on a directory names it.
    p.start(strace, {QStringLiteral("-f"), QStringLiteral("-qq"), QStringLiteral("-s"),
                     QStringLiteral("65535"), QStringLiteral("-y"), QStringLiteral("-e"),
                     QStringLiteral("trace=%file,getdents64"), QStringLiteral("-o"), log, exe, mode});
#else
    p.start(exe, {mode});
#endif
    if (!p.waitForFinished(60000)) {
        p.kill();
        p.waitForFinished();
        r.error = QStringLiteral("probe did not finish");
        return r;
    }
    const QByteArray out = p.readAllStandardOutput();
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        r.error = QStringLiteral("probe failed (exit %1): %2")
                      .arg(p.exitCode())
                      .arg(QString::fromLocal8Bit(p.readAllStandardError()));
        return r;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(out.trimmed());
    if (!doc.isObject()) {
        r.error = QStringLiteral("probe printed no JSON: %1").arg(QString::fromLocal8Bit(out));
        return r;
    }
    r.json = doc.object();
    if (r.traced) {
        const QStringList lines =
            QString::fromUtf8(readFile(log)).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        static const QRegularExpression tidRe(QStringLiteral("^(\\d+)\\s"));
        auto tidOf = [](const QString& line) {
            const auto m = tidRe.match(line);
            return m.hasMatch() ? m.captured(1) : QString();
        };
        int begin = -1;
        int end = -1;
        for (int i = 0; i < lines.size(); ++i) {
            if (begin < 0 && lines[i].contains(QLatin1String("/ASB_MARK_BEGIN"))) {
                begin = i;
            } else if (begin >= 0 && lines[i].contains(QLatin1String("/ASB_MARK_END"))) {
                end = i;
                break;
            }
        }
        if (begin < 0 || end < 0) {
            r.error = QStringLiteral("strace log has no marker pair (%1 lines)").arg(lines.size());
            return r;
        }
        // -y also decorates AT_FDCWD with the working directory ("AT_FDCWD</path/cwd>") on every
        // *at() call, naming the directory on calls that do not touch it. Drop that decoration:
        // an access of the directory itself still shows, as an fd decoration ("3</path/cwd>"), as a
        // relative "." argument, or as the absolute path.
        static const QRegularExpression fdcwdRe(QStringLiteral("AT_FDCWD<[^>]*>"));
        const QString tid = tidOf(lines[begin]);
        for (int i = begin + 1; i < end; ++i) {
            if (tidOf(lines[i]) == tid) {
                QString line = lines[i];
                r.window.append(line.replace(fdcwdRe, QStringLiteral("AT_FDCWD")));
            }
        }
    }
    r.ok = true;
    return r;
}

QStringList linesContaining(const QStringList& lines, const QString& needle) {
    QStringList hits;
    for (const QString& l : lines) {
        if (l.contains(needle)) {
            hits.append(l);
        }
    }
    return hits;
}

}  // namespace

class TstThemePatches : public QObject {
    Q_OBJECT

    QTemporaryDir m_scratch;
    QString m_previousCwd;
    QString m_userThemeDir;
    QStringList m_plantedFiles;

private slots:
    void initTestCase() {
        QVERIFY(m_scratch.isValid());
        QStandardPaths::setTestModeEnabled(true);
#ifndef Q_OS_WIN
        // QDir::homePath() reads HOME on every call, so test-mode paths land in the scratch tree.
        // Windows resolves known folders through the shell: there the test-mode location is
        // Qt's own (…\qttest), and the planted files are removed in cleanupTestCase().
        QDir().mkpath(m_scratch.filePath(QStringLiteral("home")));
        qputenv("HOME", m_scratch.filePath(QStringLiteral("home")).toUtf8());
        qunsetenv("XDG_CONFIG_HOME");
#endif
        qputenv("AETHER_SETTINGS_DIR", m_scratch.filePath(QStringLiteral("settings")).toUtf8());

        // What an unpatched ThemeManager would scan: <GenericConfigLocation>/AetherSDR/themes.
        m_userThemeDir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
                         QStringLiteral("/AetherSDR/themes");
        QVERIFY(QDir().mkpath(m_userThemeDir));
        const QString userFile = m_userThemeDir + QStringLiteral("/asb-planted-user-theme.json");
        QVERIFY(writeFile(userFile, themeVariant(QStringLiteral("ASB Planted User Theme"))));
        m_plantedFiles << userFile;

        // And what P1 without its isEmpty() guard would scan: QDir("") is the working directory.
        const QString cwd = m_scratch.filePath(QStringLiteral("cwd"));
        QVERIFY(QDir().mkpath(cwd));
        m_previousCwd = QDir::currentPath();
        QVERIFY(QDir::setCurrent(cwd));
        QVERIFY(writeFile(QDir(cwd).filePath(QStringLiteral("asb-planted-cwd-theme.json")),
                          themeVariant(QStringLiteral("ASB Planted CWD Theme"))));

        AppSettings::instance();
        ThemeManager::instance();  // constructed now, with both directories planted
    }

    void cleanupTestCase() {
        if (!m_previousCwd.isEmpty()) {
            QDir::setCurrent(m_previousCwd);
        }
        for (const QString& f : m_plantedFiles) {
            QFile::remove(f);
        }
    }

    // The GUI resources are the vendored bytes, and both built-ins are listed.
    void resourcesPresent() {
        const QString vendor = QStringLiteral(ASB_AETHERSDR_DIR);
        const QList<QPair<QString, QString>> resources{
            {QStringLiteral(":/themes/default-dark.json"), QStringLiteral("resources/themes/default-dark.json")},
            {QStringLiteral(":/themes/default-light.json"), QStringLiteral("resources/themes/default-light.json")},
            {QStringLiteral(":/fonts/Inter-Regular.ttf"), QStringLiteral("third_party/inter/Inter-Regular.ttf")},
            {QStringLiteral(":/fonts/Inter-Bold.ttf"), QStringLiteral("third_party/inter/Inter-Bold.ttf")},
            {QStringLiteral(":/logo-256.png"), QStringLiteral("docs/assets/logo-circle-256.png")},
            {QStringLiteral(":/icon.png"), QStringLiteral("docs/assets/logo-circle.png")},
        };
        for (const auto& [resource, file] : resources) {
            const QByteArray bytes = readFile(resource);
            QVERIFY2(!bytes.isEmpty(), qPrintable(resource));
            QCOMPARE(sha256(bytes), sha256(readFile(vendor + QLatin1Char('/') + file)));
        }
        const QStringList themes = ThemeManager::instance().availableThemes();
        QVERIFY(themes.contains(QStringLiteral("Default Dark")));
        QVERIFY(themes.contains(QStringLiteral("Default Light")));
    }

    // P1: with the bridge's host (""), only the built-ins are listed, and (Linux) nothing names the
    // user theme directory or its files.
    void noUserDirScan() {
        QCOMPARE(sorted(ThemeManager::instance().availableThemes()), kBuiltIns);

        const ProbeRun r = runProbe(qEnvironmentVariable("ASB_THEME_PROBE"), QStringLiteral("scan"),
                                    m_scratch.filePath(QStringLiteral("probe-scan")));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(sorted(r.json.value(QStringLiteral("themes")).toVariant().toStringList()), kBuiltIns);
        if (r.traced) {
            const QString dir = r.json.value(QStringLiteral("userThemeDir")).toString();
            QVERIFY(dir.endsWith(QLatin1String("/AetherSDR/themes")));
            const QStringList hits = linesContaining(r.window, dir) +
                                     linesContaining(r.window, QStringLiteral("/AetherSDR/themes")) +
                                     linesContaining(r.window, QStringLiteral("asb-planted-"));
            QVERIFY2(hits.isEmpty(), qPrintable(hits.join(QLatin1Char('\n'))));
        }
    }

    // P1 must skip the whole user-dir block, not just substitute the path: with an empty path,
    // QDir("") is the working directory, so a valid theme planted there would be listed.
    void emptyHookScansNothingInCwd() {
        QVERIFY(!ThemeManager::instance().availableThemes().contains(
            QStringLiteral("ASB Planted CWD Theme")));

        const ProbeRun r = runProbe(qEnvironmentVariable("ASB_THEME_PROBE"), QStringLiteral("scan"),
                                    m_scratch.filePath(QStringLiteral("probe-cwd")));
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(!r.json.value(QStringLiteral("themes")).toVariant().toStringList().contains(
            QStringLiteral("ASB Planted CWD Theme")));
        if (r.traced) {
            const QString cwdFile = r.json.value(QStringLiteral("cwdFile")).toString();
            const QString cwd = QFileInfo(cwdFile).absolutePath();
            QVERIFY(!cwd.isEmpty());
            const QStringList hits = linesContaining(r.window, cwd) +
                                     linesContaining(r.window, QStringLiteral("\".\"")) +
                                     linesContaining(r.window, QStringLiteral("asb-planted-cwd-theme"));
            QVERIFY2(hits.isEmpty(), qPrintable(hits.join(QLatin1Char('\n'))));
        }
    }

    // Positive control for the two above: the same probe with AetherSDR's own host DOES list the
    // planted user theme, and (Linux) the same observation sees the directory listed and the file
    // opened. Without this, a blind observer would make the negatives pass.
    void aetherHostScansUserDir() {
        const ProbeRun r = runProbe(qEnvironmentVariable("ASB_THEME_PROBE_AETHERHOST"),
                                    QStringLiteral("scan"),
                                    m_scratch.filePath(QStringLiteral("probe-control")));
        QVERIFY2(r.ok, qPrintable(r.error));
        const QStringList themes = r.json.value(QStringLiteral("themes")).toVariant().toStringList();
        QVERIFY2(themes.contains(QStringLiteral("ASB Planted User Theme")), qPrintable(themes.join(", ")));
        QVERIFY(!themes.contains(QStringLiteral("ASB Planted CWD Theme")));
        if (r.traced) {
            const QString dir = r.json.value(QStringLiteral("userThemeDir")).toString();
            const QString file = r.json.value(QStringLiteral("userFile")).toString();
            QVERIFY2(!linesContaining(r.window, QStringLiteral("getdents64(")).filter(dir).isEmpty(),
                     qPrintable(r.window.join(QLatin1Char('\n'))));
            QVERIFY2(!linesContaining(r.window, QStringLiteral("open")).filter(file).isEmpty(),
                     qPrintable(r.window.join(QLatin1Char('\n'))));
        }
    }

    // P2: the active theme is exactly the given bytes (which exist in no file), and (Linux) the
    // activation makes no file-system call at all.
    void activatesFromBytesOnly() {
        auto& tm = ThemeManager::instance();
        const QByteArray bytes = bytesOnlyTheme(QStringLiteral("Default Dark"));
        QVERIFY(!bytes.isEmpty());
        QVERIFY(tm.value(QStringLiteral("color.canon.cyan")) != bytesOnlyCyan());
        QSignalSpy changed(&tm, &ThemeManager::themeChanged);
        QVERIFY(tm.activateValidatedTheme(QStringLiteral("Default Dark"), bytes));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(tm.activeTheme(), QStringLiteral("Default Dark"));
        // Not the bundled Default Dark file's value: the bytes, under a built-in's name.
        QCOMPARE(tm.value(QStringLiteral("color.canon.cyan")), bytesOnlyCyan());

        const ProbeRun r = runProbe(qEnvironmentVariable("ASB_THEME_PROBE"),
                                    QStringLiteral("activate-bytes"),
                                    m_scratch.filePath(QStringLiteral("probe-bytes")));
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.json.value(QStringLiteral("ok")).toBool());
        QCOMPARE(r.json.value(QStringLiteral("cyan")).toString(), bytesOnlyCyan());
        if (r.traced) {
            QVERIFY2(r.window.isEmpty(), qPrintable(r.window.join(QLatin1Char('\n'))));
        }
    }

    // The bytes the caller validated are what loads, even when the file they came from is
    // replaced afterwards. (P2 layer; the follower-layer form of this test is WS-11's.)
    void replaceAfterValidationHasNoEffect() {
        auto& tm = ThemeManager::instance();
        auto withCyan = [](const QString& cyan) {
            return themeVariant(QStringLiteral("ASB Replace Test"), [&cyan](QJsonObject& doc) {
                setPath(doc, rootToken({QStringLiteral("color"), QStringLiteral("canon"), QStringLiteral("cyan")}),
                        cyan);
            });
        };
        const QString file = m_scratch.filePath(QStringLiteral("replace-test.json"));
        QVERIFY(writeFile(file, withCyan(QStringLiteral("#111111"))));
        const QByteArray validated = readFile(file);
        QVERIFY(writeFile(file, withCyan(QStringLiteral("#222222"))));
        QVERIFY(readFile(file) != validated);

        QVERIFY(tm.activateValidatedTheme(QStringLiteral("ASB Replace Test"), validated));
        QCOMPARE(tm.activeTheme(), QStringLiteral("ASB Replace Test"));
        QCOMPARE(tm.value(QStringLiteral("color.canon.cyan")), QStringLiteral("#111111"));
    }

    // INV-84: a root token, a child scope and a primitive from theme A do not survive into B.
    void rootTokenFromPreviousThemeCleared() {
        auto& tm = ThemeManager::instance();
        const QByteArray a = themeVariant(QStringLiteral("ASB Theme A"), [](QJsonObject& doc) {
            setPath(doc, rootToken({QStringLiteral("font"), QStringLiteral("size"), QStringLiteral("freq")}), 33);
            setPath(doc, rootToken({QStringLiteral("color"), QStringLiteral("asbOnlyInA")}),
                    QStringLiteral("#abcdef"));
            setPath(doc, {QStringLiteral("scopes"), QStringLiteral("root"), QStringLiteral("scopes"),
                          QStringLiteral("asbOnlyA"), QStringLiteral("tokens"), QStringLiteral("color"),
                          QStringLiteral("asbChild")},
                    QStringLiteral("#010203"));
            setPath(doc, {QStringLiteral("primitives"), QStringLiteral("asb.primA")},
                    QStringLiteral("#0a0b0c"));
        });
        // B: no font.size.freq token, and a freq compound whose size differs from the seed's (20).
        const QByteArray b = themeVariant(QStringLiteral("ASB Theme B"), [](QJsonObject& doc) {
            setPath(doc, rootToken({QStringLiteral("font"), QStringLiteral("family"), QStringLiteral("freq"),
                                    QStringLiteral("size")}),
                    23);
            setPath(doc, rootToken({QStringLiteral("color"), QStringLiteral("asbAlias")}),
                    QStringLiteral("{asb.primA}"));
        });

        QVERIFY(tm.activateValidatedTheme(QStringLiteral("ASB Theme A"), a));
        QCOMPARE(tm.sizing(QStringLiteral("font.size.freq")), 33);
        QCOMPARE(tm.value(QStringLiteral("color.asbOnlyInA")), QStringLiteral("#abcdef"));
        QVERIFY(tm.containerPaths().contains(QStringLiteral("asbOnlyA")));

        QVERIFY(tm.activateValidatedTheme(QStringLiteral("ASB Theme B"), b));
        QCOMPARE(tm.activeTheme(), QStringLiteral("ASB Theme B"));
        // font.size.freq now falls back to B's freq compound (sizing() and cssFragment() carry
        // that fallback; value() has none and returns "").
        QCOMPARE(tm.sizing(QStringLiteral("font.size.freq")), 23);
        QCOMPARE(tm.cssFragment(QStringLiteral("font.size.freq")), QStringLiteral("23"));
        QVERIFY(!tm.allTokenKeys().contains(QStringLiteral("font.size.freq")));
        QVERIFY(!tm.allTokenKeys().contains(QStringLiteral("color.asbOnlyInA")));
        QCOMPARE(tm.value(QStringLiteral("color.asbOnlyInA")), QString());
        QVERIFY(!tm.containerPaths().contains(QStringLiteral("asbOnlyA")));
        // A's primitive is gone: B's alias to it stays an unresolved literal.
        QCOMPARE(tm.value(QStringLiteral("color.asbAlias")), QStringLiteral("{asb.primA}"));
    }

    // Built-ins go through the same route as any theme (bytes from :/themes), produce the same
    // state as AetherSDR's file route, and persist nothing through AppSettings.
    void builtInsThroughSameRoute() {
        auto& tm = ThemeManager::instance();
        const int writesBefore = AppSettings::instance().ignoredActiveThemeWrites();
        QSignalSpy changed(&tm, &ThemeManager::themeChanged);

        QVERIFY(tm.activateValidatedTheme(QStringLiteral("Default Light"),
                                          builtInThemeBytes(QStringLiteral(":/themes/default-light.json"))));
        QCOMPARE(tm.activeTheme(), QStringLiteral("Default Light"));
        const auto lightByBytes = snapshot(tm);
        QVERIFY(tm.activateValidatedTheme(QStringLiteral("Default Dark"),
                                          builtInThemeBytes(QStringLiteral(":/themes/default-dark.json"))));
        QCOMPARE(tm.activeTheme(), QStringLiteral("Default Dark"));
        const auto darkByBytes = snapshot(tm);
        QCOMPARE(changed.count(), 2);
        QVERIFY(lightByBytes != darkByBytes);
        QCOMPARE(AppSettings::instance().ignoredActiveThemeWrites(), writesBefore);

        // The file route (AetherSDR's setActiveTheme) reaches the same state, and does try to
        // persist the name: the spy sees that write (and the shim drops it).
        QVERIFY(tm.setActiveTheme(QStringLiteral("Default Light")));
        QCOMPARE(snapshot(tm), lightByBytes);
        QCOMPARE(AppSettings::instance().ignoredActiveThemeWrites(), writesBefore + 1);
        QVERIFY(!AppSettings::instance().contains(QStringLiteral("ActiveTheme")));
    }

    // A failed activation changes nothing: no partial load, no signal.
    void failedActivationKeepsPreviousTheme() {
        auto& tm = ThemeManager::instance();
        const QByteArray b = themeVariant(QStringLiteral("ASB Theme Kept"), [](QJsonObject& doc) {
            setPath(doc, rootToken({QStringLiteral("color"), QStringLiteral("asbKept")}),
                    QStringLiteral("#0f0f0f"));
        });
        QVERIFY(tm.activateValidatedTheme(QStringLiteral("ASB Theme Kept"), b));
        const auto before = snapshot(tm);
        QSignalSpy changed(&tm, &ThemeManager::themeChanged);

        QVERIFY(!tm.activateValidatedTheme(QStringLiteral("ASB Broken"), QByteArray("{not json")));
        QVERIFY(!tm.activateValidatedTheme(QStringLiteral("ASB Array"), QByteArray("[1, 2]")));
        QVERIFY(!tm.activateValidatedTheme(
            QStringLiteral("ASB Schema 0"),
            QByteArray(R"({"schemaVersion": 0, "name": "ASB Schema 0", "tokens": {"color": {"asbKept": "#ffffff"}}})")));

        QCOMPARE(changed.count(), 0);
        QCOMPARE(tm.activeTheme(), QStringLiteral("ASB Theme Kept"));
        QCOMPARE(snapshot(tm), before);
        QCOMPARE(tm.value(QStringLiteral("color.asbKept")), QStringLiteral("#0f0f0f"));
    }
};

QTEST_MAIN(TstThemePatches)
#include "tst_ThemePatches.moc"
