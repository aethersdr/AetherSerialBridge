// INV-70: the application identity is set before any path use (D20).
//
// Process tests: each case runs the REAL executable (headless, GUI) through its test-build
// `--identity-probe` seam and reads back what the process captured at startup. Linux redirects
// every XDG directory into a scratch tree and asserts nothing outside it is touched; Windows has
// no environment override for the known folders, so it compares against Qt's own generic location.

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "app/Identity.h"

namespace {

QString exePath(const char* envName) { return qEnvironmentVariable(envName); }

struct Probe {
    bool ran = false;
    int exitCode = -1;
    QProcess::ExitStatus status = QProcess::NormalExit;
    QString stderrText;
    QJsonObject json;
};

class Sandbox {
public:
    Sandbox() {
        QVERIFY2(m_dir.isValid(), "scratch directory");
        m_env = QProcessEnvironment::systemEnvironment();
#ifndef Q_OS_WIN
        for (const char* d : {"cfg", "data", "cache", "home"}) {
            QDir(m_dir.path()).mkpath(QString::fromLatin1(d));
        }
        m_env.insert("XDG_CONFIG_HOME", m_dir.filePath("cfg"));
        m_env.insert("XDG_DATA_HOME", m_dir.filePath("data"));
        m_env.insert("XDG_CACHE_HOME", m_dir.filePath("cache"));
        m_env.insert("HOME", m_dir.filePath("home"));
#endif
    }
    QString path() const { return m_dir.path(); }
    const QProcessEnvironment& env() const { return m_env; }

    // The directory the process must report as its AppConfigLocation.
    QString expectedAppConfig() const {
#ifdef Q_OS_WIN
        return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
               QStringLiteral("/AetherSerialBridge");
#else
        return m_dir.filePath("cfg") + QStringLiteral("/AetherSerialBridge");
#endif
    }
    QString expectedAppData() const {
#ifdef Q_OS_WIN
        // AppDataLocation is under the roaming profile, GenericDataLocation under the local one:
        // take the parent of this process's own AppDataLocation and add the expected name.
        return QFileInfo(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).path() +
               QStringLiteral("/AetherSerialBridge");
#else
        return m_dir.filePath("data") + QStringLiteral("/AetherSerialBridge");
#endif
    }

    // Nothing may be created under the redirected locations: resolving a path is not creating it.
    bool untouched() const {
#ifdef Q_OS_WIN
        return true;
#else
        for (const char* d : {"cfg", "data", "cache"}) {
            if (!QDir(m_dir.filePath(QString::fromLatin1(d))).isEmpty()) {
                return false;
            }
        }
        return true;
#endif
    }

private:
    QTemporaryDir m_dir;
    QProcessEnvironment m_env;
};

Probe run(const QString& exe, const QStringList& args, const QProcessEnvironment& env,
          const QString& probeFile = QString()) {
    Probe p;
    QProcess proc;
    proc.setProcessEnvironment(env);
    proc.start(exe, args);
    p.ran = proc.waitForFinished(30000);
    if (!p.ran) {
        proc.kill();
        proc.waitForFinished(5000);
    }
    p.exitCode = proc.exitCode();
    p.status = proc.exitStatus();
    p.stderrText = QString::fromLocal8Bit(proc.readAllStandardError());
    if (!probeFile.isEmpty()) {
        QFile f(probeFile);
        if (f.open(QIODevice::ReadOnly)) {
            p.json = QJsonDocument::fromJson(f.readAll()).object();
        }
    }
    return p;
}

}  // namespace

class tst_Identity : public QObject {
    Q_OBJECT

private:
    // Which executables this run has: the headless one always, the GUI one when it was built.
    static void exeRows() {
        QTest::addColumn<QString>("exe");
        QTest::addColumn<QString>("kind");
        QTest::newRow("headless") << exePath("ASB_HEADLESS_EXE") << "headless";
        QTest::newRow("gui") << exePath("ASB_GUI_EXE") << "gui";
    }

    static bool skipMissing(const QString& exe, const QString& kind) {
        if (exe.isEmpty()) {
            QTest::qSkip(qPrintable(kind + " executable not part of this configuration"), __FILE__,
                         __LINE__);
            return true;
        }
        return false;
    }

    static Probe probe(const QString& exe, Sandbox& sb) {
        const QString out = sb.path() + QStringLiteral("/probe.json");
        return run(exe, {QStringLiteral("--identity-probe"), out}, sb.env(), out);
    }

private slots:
    void identity_data() { exeRows(); }
    // D20: application name AetherSerialBridge, empty organisation and domain, and the name
    // survives constructing the application object.
    void identity() {
        QFETCH(QString, exe);
        QFETCH(QString, kind);
        if (skipMissing(exe, kind)) return;
        Sandbox sb;
        const Probe p = probe(exe, sb);
        QVERIFY2(p.ran, "probe did not finish");
        QVERIFY2(p.exitCode == 0 && p.status == QProcess::NormalExit, qPrintable(p.stderrText));
        QCOMPARE(p.json.value("kind").toString(), kind);
        QCOMPARE(p.json.value("identityApplied").toBool(), true);
        QCOMPARE(p.json.value("applicationName").toString(), QStringLiteral("AetherSerialBridge"));
        QCOMPARE(p.json.value("organizationName").toString(), QString());
        QCOMPARE(p.json.value("organizationDomain").toString(), QString());
        QCOMPARE(p.json.value("applicationNameAfterConstruction").toString(),
                 QStringLiteral("AetherSerialBridge"));
    }

    void paths_use_identity_data() { exeRows(); }
    // The per-user directories the process captured at startup are under AetherSerialBridge, not
    // under the executable's file name, and resolving them created nothing.
    void paths_use_identity() {
        QFETCH(QString, exe);
        QFETCH(QString, kind);
        if (skipMissing(exe, kind)) return;
        Sandbox sb;
        const Probe p = probe(exe, sb);
        QVERIFY2(p.ran && p.exitCode == 0, qPrintable(p.stderrText));
        QCOMPARE(QDir::cleanPath(p.json.value("appConfigAtStartup").toString()),
                 QDir::cleanPath(sb.expectedAppConfig()));
        QCOMPARE(QDir::cleanPath(p.json.value("appDataAtStartup").toString()),
                 QDir::cleanPath(sb.expectedAppData()));
        QVERIFY2(sb.untouched(), "resolving a path created something under the XDG directories");
    }

    // The GUI settings file: <AppConfigLocation>/gui.ini, resolved at the process's first path
    // use. This is the case that kills "identity set after AppSettings" (INV-70 mutant).
    void gui_ini_path() {
        const QString exe = exePath("ASB_GUI_EXE");
        if (skipMissing(exe, QStringLiteral("gui"))) return;
        Sandbox sb;
        const Probe p = probe(exe, sb);
        QVERIFY2(p.ran && p.exitCode == 0 && p.status == QProcess::NormalExit,
                 qPrintable(p.stderrText));
        QCOMPARE(QDir::cleanPath(p.json.value("guiIniPathAtStartup").toString()),
                 QDir::cleanPath(sb.expectedAppConfig() + QStringLiteral("/gui.ini")));
    }

    // The same probe on the headless executable reports the headless startup paths too: nothing in
    // either executable resolves a path under another name.
    void headless_ini_path_is_never_under_the_exe_name() {
        const QString exe = exePath("ASB_HEADLESS_EXE");
        if (skipMissing(exe, QStringLiteral("headless"))) return;
        Sandbox sb;
        const Probe p = probe(exe, sb);
        QVERIFY2(p.ran && p.exitCode == 0, qPrintable(p.stderrText));
        const QString ini = p.json.value("guiIniPathAtStartup").toString();
        QVERIFY2(ini.endsWith(QStringLiteral("/AetherSerialBridge/gui.ini")), qPrintable(ini));
        QVERIFY2(!ini.contains(QStringLiteral("aether-serial-bridge")), qPrintable(ini));
    }

    void renamed_exe_keeps_identity_data() { exeRows(); }
    // The identity must not depend on the executable's file name. After a Q*Application exists, Qt's
    // default application name is the executable's base name, and on Windows the GUI executable's
    // base name IS "AetherSerialBridge": there, "set the identity only once the application object
    // exists" is indistinguishable from the real thing. A copy under another name tells them
    // apart. The copy sits next to the original so the Qt libraries resolve exactly as they do for
    // the original (same directory, same RUNPATH / PATH).
    void renamed_exe_keeps_identity() {
        QFETCH(QString, exe);
        QFETCH(QString, kind);
        if (skipMissing(exe, kind)) return;
        const QFileInfo orig(exe);
#ifdef Q_OS_WIN
        const QString suffix = QStringLiteral(".exe");
#else
        const QString suffix;
#endif
        const QString copy = orig.absolutePath() + QStringLiteral("/asb-renamed-") + kind + suffix;
        QFile::remove(copy);
        QVERIFY2(QFile::copy(exe, copy), qPrintable(copy));
        const auto cleanup = qScopeGuard([&] { QFile::remove(copy); });
        Sandbox sb;
        const Probe p = probe(copy, sb);
        QVERIFY2(p.ran && p.exitCode == 0 && p.status == QProcess::NormalExit,
                 qPrintable(p.stderrText));
        QCOMPARE(p.json.value("applicationName").toString(), QStringLiteral("AetherSerialBridge"));
        QCOMPARE(p.json.value("applicationNameAfterConstruction").toString(),
                 QStringLiteral("AetherSerialBridge"));
        QCOMPARE(QDir::cleanPath(p.json.value("appConfigAtStartup").toString()),
                 QDir::cleanPath(sb.expectedAppConfig()));
        QCOMPARE(QDir::cleanPath(p.json.value("appDataAtStartup").toString()),
                 QDir::cleanPath(sb.expectedAppData()));
        QCOMPARE(QDir::cleanPath(p.json.value("guiIniPathAtStartup").toString()),
                 QDir::cleanPath(sb.expectedAppConfig() + QStringLiteral("/gui.ini")));
    }

    void path_before_identity_is_refused_data() { exeRows(); }
    // A path resolved before the identity exists is refused: the process exits with
    // kExitIdentityViolation and says why, instead of resolving it with no application name.
    void path_before_identity_is_refused() {
        QFETCH(QString, exe);
        QFETCH(QString, kind);
        if (skipMissing(exe, kind)) return;
        Sandbox sb;
        const Probe p = run(exe, {QStringLiteral("--probe-path-before-identity")}, sb.env());
        QVERIFY2(p.ran, "probe did not finish");
        QCOMPARE(p.exitCode, asb::app::kExitIdentityViolation);
        QVERIFY2(p.stderrText.contains(QStringLiteral("INV-70")), qPrintable(p.stderrText));
        QVERIFY2(sb.untouched(), "the refused run still created something");
    }

    // HeadlessDigest.h: the digest compiled into the GUI is the SHA-256 of the headless
    // executable that was linked before it.
    void gui_embeds_headless_digest() {
        const QString gui = exePath("ASB_GUI_EXE");
        const QString headless = exePath("ASB_HEADLESS_EXE");
        if (skipMissing(gui, QStringLiteral("gui"))) return;
        Sandbox sb;
        const Probe p = probe(gui, sb);
        QVERIFY2(p.ran && p.exitCode == 0, qPrintable(p.stderrText));
        QFile f(headless);
        QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(headless));
        QCryptographicHash h(QCryptographicHash::Sha256);
        QVERIFY(h.addData(&f));
        QCOMPARE(p.json.value("headlessDigest").toString(), QString::fromLatin1(h.result().toHex()));
    }
};

QTEST_GUILESS_MAIN(tst_Identity)
#include "tst_Identity.moc"
