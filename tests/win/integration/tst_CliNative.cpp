// WS-9: the CLI's Windows operation path on the real SCM and the real WMI firewall, before any station runs it. The
// console executable itself (aether-serial-bridge.exe, as an operator types it) runs --status, --install,
// --add-port, --apply, --status, --stop, --start, --uninstall: each operation on its OperationHost worker thread,
// under the process guard (job object), with the CIM connection made on that worker. A dynamic-Qt test build needs
// the Qt DLLs beside the installed exe, as WS-7's rows and harness deploy them (the shipped exe is static, D3).
// Everything the row creates is removed by it.
//
// WS-16 (INV-125, INV-126): the uninstall removes the program folder; --remove-settings removes only the known files;
// the Apps & features entry is written by --install and removed last; the installed exe uninstalling itself renames
// its image aside and schedules it (and the folder) for deletion at the next restart; --uninstall-from-apps' elevated
// half (this runner is an administrator) with the test message box (ASB_TEST_MESSAGEBOX, every native path that can
// reach MessageBoxW sets it). A dynamic-Qt test build's program folder also holds the Qt DLLs this test copied there
// (deployDevRuntime): the product never put them there and never removes what it does not know, so they are the one
// thing allowed to remain after an uninstall, and the row removes them.
//
// The release workflow also runs this suite on the static release exe, which has no test seams (WS-13): there the
// two rows that answer the message box through a seam skip with that reason (ASB_REQUIRE_SEAMS), and the release
// workflow runs them on the static seam build of the same commit instead.

#include "ServiceNativeSupport.h"
#include "WinNativeSupport.h"
#include "platform/win/CimFirewall.h"
#include "platform/win/WbemSession.h"

#include "platform/win/UninstallEntry.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QProcess>
#include <QRegularExpression>
#include <QTest>

#include <windows.h>

using namespace asb;
using namespace asb::test;

namespace {

constexpr int kPort = 45219;

struct Run {
    int code = -1;
    QString out;
};

// Runs `exe` (the build's console exe, or the installed copy) with `args`; `extraEnv` = "NAME=value" entries.
Run runExe(const QString& exe, const QStringList& args, const QStringList& extraEnv = {}, int ms = 300000)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    // Where each operation's time goes (the station measured --apply and --start at ~14 s each).
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("ASB_PHASE_TIMES"), QStringLiteral("1"));
    for (const QString& kv : extraEnv) {
        const qsizetype eq = kv.indexOf(QLatin1Char('='));
        env.insert(kv.left(eq), kv.mid(eq + 1));
    }
    p.setProcessEnvironment(env);
    p.start(exe, args);
    Run r;
    QElapsedTimer took;
    took.start();
    if (!p.waitForStarted(30000)) {
        r.out = QStringLiteral("did not start: %1").arg(p.errorString());
        return r;
    }
    if (!p.waitForFinished(ms)) {
        p.kill();
        p.waitForFinished(10000);
        r.out = QString::fromLocal8Bit(p.readAll()) + QStringLiteral("\n(killed after %1 ms)").arg(ms);
        return r;
    }
    r.code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -2;
    r.out = QString::fromLocal8Bit(p.readAll());
    if (p.exitStatus() != QProcess::NormalExit)
        r.out += QStringLiteral("\n(crashed: exit code 0x%1)").arg(quint32(p.exitCode()), 8, 16, QLatin1Char('0'));
    // The row's evidence: every command and what it printed (the ctest log keeps it).
    qInfo("$ %s %s  -> exit %d (%.2f s)\n%s", qPrintable(QDir::toNativeSeparators(exe)),
          qPrintable(args.join(QLatin1Char(' '))), r.code, double(took.elapsed()) / 1000.0, qPrintable(r.out.trimmed()));
    return r;
}

Run cli(const QStringList& args, const QStringList& extraEnv = {}, int ms = 300000)
{
    return runExe(svc::headlessExe(), args, extraEnv, ms);
}

// A run that may reach a message box gets less than QtTest's 300 s function watchdog: should a real MessageBoxW ever
// wait for a click, runExe kills it and the row fails with cleanup() still to run (the watchdog would abort the whole
// test process instead, leaving the box up, the installation present and the log file held open by the child).
constexpr int kMessageBoxRunMs = 120000;

// The seams (environment variable names) of `names` that the exe's image does not contain, the way
// tools/release_audit.py looks for them (ASCII or UTF-16LE). A release exe has none of them by design (WS-13).
QStringList missingSeams(const QString& exe, const QStringList& names)
{
    QFile f(exe);
    if (!f.open(QIODevice::ReadOnly)) return names;
    const QByteArray image = f.readAll();
    QStringList missing;
    for (const QString& n : names) {
        const QByteArray ascii = n.toLatin1();
        QByteArray wide;
        for (const char c : ascii) wide.append(c).append('\0');
        if (!image.contains(ascii) && !image.contains(wide)) missing << n;
    }
    return missing;
}

}  // namespace

// A row that answers a message box through a test seam runs only on an exe that has the seams. On the release
// workflow's runs against the static release exe (ASB_HEADLESS_STATIC=1, no seams by design) it is skipped, by name
// and with its reason; on any other build a missing seam is a failure, so a dev build can never skip it silently.
#define ASB_REQUIRE_SEAMS(...)                                                                           \
    do {                                                                                                 \
        const QStringList missing_ = missingSeams(svc::headlessExe(), {__VA_ARGS__});                    \
        if (!missing_.isEmpty()) {                                                                       \
            const QString why_ =                                                                         \
                QStringLiteral("needs the %1 seam; dev build only").arg(missing_.join(QStringLiteral(", "))); \
            if (qEnvironmentVariableIntValue("ASB_HEADLESS_STATIC") == 1) QSKIP(qPrintable(why_));       \
            QFAIL(qPrintable(QStringLiteral("%1, and this is not the release run (%2)").arg(why_, svc::headlessExe()))); \
        }                                                                                                \
    } while (false)

namespace {

// What is left in the program folder after an uninstall, besides the dev runtime this test copied there and the
// names in `alsoAllowed` (empty = as expected). Without a dev runtime (a static exe) the folder itself must be gone.
QString programFolderLeft(const QString& folder, const QStringList& devRuntime, const QStringList& alsoAllowed = {})
{
    if (!QFileInfo::exists(folder)) return {};
    QStringList other;
    if (devRuntime.isEmpty() && alsoAllowed.isEmpty()) other << QStringLiteral("(the folder itself)");
    for (const QString& n : QDir(folder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
        if (!devRuntime.contains(n, Qt::CaseInsensitive) && !alsoAllowed.contains(n, Qt::CaseInsensitive)) other << n;
    return other.join(QStringLiteral(", "));
}

// The renamed-aside images (<exe>.removed-<16 hex>) in the program folder.
QStringList asideImages(const QString& folder)
{
    return QDir(folder).entryList({QStringLiteral("aether-serial-bridge.exe.removed-*")}, QDir::Files | QDir::Hidden | QDir::System);
}

int ruleCount()
{
    win::WbemSession cim;
    cim.connect();
    return int(win::CimFirewall::readGroup(cim, win::cim::Store::Active).size());
}

}  // namespace

class tst_CliNative : public QObject {
    Q_OBJECT

    std::unique_ptr<win::WindowsPlatform> m_p;
    bool m_hadInstallFolders = false;

private slots:
    void initTestCase()
    {
        QString err;
        m_p = svc::platform(&err);
        QVERIFY2(m_p, qPrintable(err));
        m_hadInstallFolders = QFileInfo::exists(m_p->lockDirectory()) || QFileInfo::exists(m_p->dataFolder());
        svc::removeServiceHard(m_p->installedExe());
    }
    // After every row, whatever happened: the service and its rules, the entry, an aside image and its restart
    // deletions, and (on a box that had none) both folders, so that one failed row does not fail the next ones.
    void cleanup()
    {
        if (!m_p) return;
        if (svc::query().exists) cli({QStringLiteral("--uninstall")});
        svc::removeServiceHard(m_p->installedExe());   // and the Apps & features entry
        for (const QString& n : asideImages(m_p->lockDirectory())) QFile::remove(m_p->lockDirectory() + QLatin1Char('\\') + n);
        svc::dropRestartDeletionsUnder(m_p->lockDirectory());
        if (!m_hadInstallFolders) {
            native::removeTree(m_p->lockDirectory());
            native::removeTree(m_p->dataFolder());
        }
    }

    // Whatever happened: the service, the rules and the folders the rows made go.
    void cleanupTestCase()
    {
        if (!m_p) return;
        if (svc::query().exists) cli({QStringLiteral("--uninstall")});
        svc::removeServiceHard(m_p->installedExe());
        svc::dropRestartDeletionsUnder(m_p->lockDirectory());
        if (!m_hadInstallFolders) {
            native::removeTree(m_p->lockDirectory());
            native::removeTree(m_p->dataFolder());
        }
    }

    void cli_install_apply_stop_start_uninstall()
    {
        Run r = cli({QStringLiteral("--status")});
        QCOMPARE(r.code, 0);
        QVERIFY2(r.out.contains(QLatin1String("Service: not installed")), qPrintable(r.out));

        // The dev runtime beside the exe --install will copy (test builds only; WS-7's deployDevRuntime).
        qInfo("dev runtime: %s", qPrintable(svc::deployDevRuntime(m_p->lockDirectory()).join(QStringLiteral(", "))));

        r = cli({QStringLiteral("--install")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY(svc::waitState(SERVICE_RUNNING, 15000));

        // A port on a COM name that does not exist: the device is opened only when a client connects (INV-1).
        r = cli({QStringLiteral("--add-port"), QStringLiteral("Native"), QStringLiteral("COM250"), QString::number(kPort),
                 QStringLiteral("--bind"), QStringLiteral("127.0.0.1")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY2(r.out.contains(QLatin1String("Added Native: COM250 at 38400 8N1 on 127.0.0.1:")), qPrintable(r.out));

        r = cli({QStringLiteral("--apply")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY(svc::waitState(SERVICE_RUNNING, 15000));
        QCOMPARE(ruleCount(), 1);

        r = cli({QStringLiteral("--status")});
        QCOMPARE(r.code, 0);
        QVERIFY2(r.out.contains(QLatin1String("Service: running (pid ")), qPrintable(r.out));
        QVERIFY2(r.out.contains(QLatin1String("Native")), qPrintable(r.out));
        QVERIFY2(r.out.contains(QLatin1String("Config: the bridge reported loading the saved config.json.")), qPrintable(r.out));
        QVERIFY(svc::canConnect(kPort, 5000));

        r = cli({QStringLiteral("--stop")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY(svc::waitState(SERVICE_STOPPED, 30000));
        r = cli({QStringLiteral("--status")});
        QVERIFY2(r.out.contains(QLatin1String("Activation: none")), qPrintable(r.out));

        r = cli({QStringLiteral("--start")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY2(r.out.contains(QLatin1String("Started: the bridge is running the saved settings.")), qPrintable(r.out));
        QVERIFY(svc::waitState(SERVICE_RUNNING, 15000));

        r = cli({QStringLiteral("--uninstall")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY2(r.out.contains(QLatin1String("The settings are kept in")), qPrintable(r.out));
        // The installed exe goes with the service, and so do the lock files (WS-16: deleted when the uninstall closes
        // them).
        QVERIFY2(r.out.contains(QLatin1String("removed ")) && r.out.contains(QLatin1String("aether-serial-bridge.exe")), qPrintable(r.out));
        QVERIFY(!QFileInfo::exists(m_p->installedExe()));
        QVERIFY(!QFileInfo::exists(m_p->lockDirectory() + QStringLiteral("\\apply.lock")));
        QVERIFY(svc::waitDeleted(60000));
        QCOMPARE(svc::query().exists, false);   // sc query: 1060
        QCOMPARE(ruleCount(), 0);                // the firewall group is empty
        r = cli({QStringLiteral("--status")});
        QVERIFY2(r.out.contains(QLatin1String("Service: not installed")), qPrintable(r.out));
        native::removeTree(m_p->lockDirectory());   // the dev runtime (uninstallRemovesProgramFolder checks the rest)
    }

    // INV-126: --install writes the Apps & features entry with exactly these values; --uninstall removes it.
    void installWritesEntry()
    {
        svc::deployDevRuntime(m_p->lockDirectory());
        Run r = cli({QStringLiteral("--install")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY(svc::appsEntryExists());
        // A written entry is reported as written: the flush succeeds, so no "not written" warning.
        QVERIFY2(!r.out.contains(QLatin1String("not written")), qPrintable(r.out));   // stdout and stderr merged
        const QString exe = m_p->installedExe();
        QCOMPARE(svc::appsEntryValue("DisplayName"), QStringLiteral("Aether Serial Bridge"));
        QCOMPARE(svc::appsEntryValue("Publisher"), QStringLiteral("AetherSDR"));
        QCOMPARE(svc::appsEntryValue("InstallLocation"), m_p->lockDirectory());
        QCOMPARE(svc::appsEntryValue("DisplayIcon"), QStringLiteral("\"%1\",0").arg(exe));
        QCOMPARE(svc::appsEntryValue("UninstallString"),
                 QStringLiteral("\"%1\" %2").arg(exe, QLatin1String(win::UninstallEntry::kAppsArgument)));
        QCOMPARE(svc::appsEntryValue("NoModify"), QStringLiteral("1"));
        QCOMPARE(svc::appsEntryValue("NoRepair"), QStringLiteral("1"));
        QVERIFY(!svc::appsEntryValue("DisplayVersion").isEmpty());
        QCOMPARE(svc::appsEntryValue("InstallDate").size(), qsizetype(8));
        QVERIFY(QFileInfo::exists(exe));

        r = cli({QStringLiteral("--uninstall")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY2(r.out.contains(QLatin1String("removed the Apps & features entry")), qPrintable(r.out));
        QVERIFY(!svc::appsEntryExists());
        QVERIFY(svc::waitDeleted(60000));
        native::removeTree(m_p->lockDirectory());
    }

    // INV-125: after --uninstall, %ProgramFiles%\AetherSerialBridge is gone (only this test's dev runtime may stay).
    void uninstallRemovesProgramFolder()
    {
        const QStringList dev = svc::deployDevRuntime(m_p->lockDirectory());
        Run r = cli({QStringLiteral("--install")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY(svc::waitState(SERVICE_RUNNING, 15000));
        r = cli({QStringLiteral("--uninstall")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY(svc::waitDeleted(60000));
        QCOMPARE(ruleCount(), 0);
        QVERIFY(!svc::appsEntryExists());
        const QString left = programFolderLeft(m_p->lockDirectory(), dev);
        QVERIFY2(left.isEmpty(), qPrintable(QStringLiteral("left in the program folder: %1\n%2").arg(left, r.out)));
        QVERIFY(svc::restartDeletionsUnder(m_p->lockDirectory()).isEmpty());   // nothing was in use: nothing scheduled
        native::removeTree(m_p->lockDirectory());
        QVERIFY(!QFileInfo::exists(m_p->lockDirectory()));
    }

    // INV-125: --remove-settings removes the files this program writes and nothing else.
    void removeSettingsKeepsPlantedFile()
    {
        svc::deployDevRuntime(m_p->lockDirectory());
        Run r = cli({QStringLiteral("--install")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        r = cli({QStringLiteral("--add-port"), QStringLiteral("Settings"), QStringLiteral("COM250"), QString::number(kPort),
                 QStringLiteral("--bind"), QStringLiteral("127.0.0.1")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        r = cli({QStringLiteral("--apply")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        const QString data = m_p->dataFolder();
        const QString planted = data + QStringLiteral("\\notes.txt");
        QVERIFY(native::writeFile(planted, QByteArrayLiteral("mine")));
        QVERIFY(QFileInfo::exists(data + QStringLiteral("\\config.json")));

        r = cli({QStringLiteral("--uninstall"), QStringLiteral("--remove-settings")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY2(r.out.contains(QLatin1String("The settings were removed.")), qPrintable(r.out));   // stdout and stderr merged
        QVERIFY2(r.out.contains(QLatin1String("not a file this program writes")), qPrintable(r.out));
        QCOMPARE(native::readFile(planted).value_or(QByteArray()), QByteArrayLiteral("mine"));
        const QStringList inData = QDir(data).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        qInfo("left in the data folder: %s", qPrintable(inData.join(QStringLiteral(", "))));
        QVERIFY2(!inData.contains(QStringLiteral("config.json"), Qt::CaseInsensitive), qPrintable(inData.join(QStringLiteral(", "))));
        QVERIFY(svc::waitDeleted(60000));
        QCOMPARE(ruleCount(), 0);
        native::removeTree(data);
        native::removeTree(m_p->lockDirectory());
    }

    // INV-125: the installed exe uninstalling itself: its image cannot be deleted while it runs, so it is renamed
    // aside and scheduled for deletion at the next restart (and the folder after it, when nothing else is in it).
    // Kills inv125-running-image-kept-not-scheduled.
    void uninstallFromInstalledExe()
    {
        const QStringList dev = svc::deployDevRuntime(m_p->lockDirectory());
        Run r = cli({QStringLiteral("--install")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        const QString exe = m_p->installedExe();
        const QString folder = m_p->lockDirectory();
        QVERIFY(svc::restartDeletionsUnder(folder).isEmpty());

        r = runExe(exe, {QStringLiteral("--uninstall")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY2(r.out.contains(QLatin1String("it is deleted when Windows restarts")), qPrintable(r.out));
        QVERIFY(svc::waitDeleted(60000));
        QCOMPARE(ruleCount(), 0);
        QVERIFY(!QFileInfo::exists(exe));
        QVERIFY(!svc::appsEntryExists());   // the program is gone from its name: the entry goes
        const QStringList aside = asideImages(folder);
        QCOMPARE(aside.size(), qsizetype(1));
        QVERIFY2(QRegularExpression(QStringLiteral("^aether-serial-bridge\\.exe\\.removed-[0-9a-f]{16}$")).match(aside.first()).hasMatch(),
                 qPrintable(aside.first()));
        const QStringList scheduled = svc::restartDeletionsUnder(folder);
        qInfo("restart deletions under the program folder: %s", qPrintable(scheduled.join(QStringLiteral(" | "))));
        const QString asidePath = folder + QLatin1Char('\\') + aside.first();
        const auto names = [&scheduled](const QString& path) {   // whatever NT prefix Windows wrote before it
            for (const QString& s : scheduled)
                if (s.endsWith(path, Qt::CaseInsensitive)) return true;
            return false;
        };
        QVERIFY2(names(asidePath), qPrintable(scheduled.join(QLatin1Char('|'))));
        if (dev.isEmpty())   // a static exe: only the image was left, so the folder is scheduled after it
            QVERIFY2(names(folder), qPrintable(scheduled.join(QLatin1Char('|'))));
        const QString left = programFolderLeft(folder, dev, aside);
        QVERIFY2(left.isEmpty(), qPrintable(QStringLiteral("left in the program folder: %1").arg(left)));

        // The row's own clean-up: the image (no longer running), its scheduled deletions, the folder.
        QVERIFY(QFile::remove(asidePath));
        QVERIFY(svc::dropRestartDeletionsUnder(folder));
        QVERIFY(svc::restartDeletionsUnder(folder).isEmpty());
        native::removeTree(folder);
    }

    // INV-126: --uninstall-from-apps elevated (the runner is an administrator), the question answered Yes: the entry,
    // the service, the rules and the program folder are gone, and the result is shown in a (test) message box.
    void uninstallFromAppsEntry()
    {
        ASB_REQUIRE_SEAMS(QStringLiteral("ASB_TEST_MESSAGEBOX"));
        const QStringList dev = svc::deployDevRuntime(m_p->lockDirectory());
        Run r = cli({QStringLiteral("--install")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY(svc::appsEntryExists());

        r = cli({QLatin1String(win::UninstallEntry::kAppsArgument)}, {QStringLiteral("ASB_TEST_MESSAGEBOX=yes")}, kMessageBoxRunMs);
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY2(r.out.contains(QLatin1String("[message box] Uninstall Aether Serial Bridge?")), qPrintable(r.out));
        QVERIFY2(r.out.contains(QLatin1String("[message box] Uninstalled:")), qPrintable(r.out));
        QVERIFY(svc::waitDeleted(60000));
        QCOMPARE(ruleCount(), 0);
        QVERIFY(!svc::appsEntryExists());
        QVERIFY(!QFileInfo::exists(m_p->installedExe()));
        const QString left = programFolderLeft(m_p->lockDirectory(), dev);
        QVERIFY2(left.isEmpty(), qPrintable(QStringLiteral("left in the program folder: %1\n%2").arg(left, r.out)));
        native::removeTree(m_p->lockDirectory());
    }

    // INV-126 / D69: the lock clean-up fails (ASB_TEST_FAIL_LOCK_DISPOSITION): the service and the rules are gone, the
    // program and the entry are kept together. Then exactly the entry's command line, without the seam, finishes it.
    // Kills inv126-program-removed-before-lock-cleanup at the native layer.
    void appsEntryRetryAfterLockCleanupFailure()
    {
        ASB_REQUIRE_SEAMS(QStringLiteral("ASB_TEST_MESSAGEBOX"), QStringLiteral("ASB_TEST_FAIL_LOCK_DISPOSITION"));
        const QStringList dev = svc::deployDevRuntime(m_p->lockDirectory());
        Run r = cli({QStringLiteral("--install")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        const QString exe = m_p->installedExe();
        const QString folder = m_p->lockDirectory();

        r = cli({QLatin1String(win::UninstallEntry::kAppsArgument)},
                {QStringLiteral("ASB_TEST_MESSAGEBOX=yes"), QStringLiteral("ASB_TEST_FAIL_LOCK_DISPOSITION=1")}, kMessageBoxRunMs);
        QVERIFY2(r.code != 0, qPrintable(r.out));
        QVERIFY(svc::waitDeleted(60000));
        QCOMPARE(ruleCount(), 0);
        QVERIFY2(svc::appsEntryExists(), qPrintable(r.out));
        QVERIFY2(QFileInfo::exists(exe), qPrintable(r.out));

        const QString command = svc::appsEntryValue("UninstallString");
        QStringList args = QProcess::splitCommand(command);
        QVERIFY2(!args.isEmpty(), qPrintable(command));
        const QString program = args.takeFirst();
        QCOMPARE(program, exe);
        QVERIFY(QFileInfo::exists(program));
        r = runExe(program, args, {QStringLiteral("ASB_TEST_MESSAGEBOX=yes")}, kMessageBoxRunMs);
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY(!svc::appsEntryExists());
        QVERIFY(!QFileInfo::exists(exe));
        const QStringList aside = asideImages(folder);   // it ran from the installed image: renamed aside, scheduled
        QCOMPARE(aside.size(), qsizetype(1));
        const QString left = programFolderLeft(folder, dev, aside);
        QVERIFY2(left.isEmpty(), qPrintable(QStringLiteral("left in the program folder: %1\n%2").arg(left, r.out)));

        QVERIFY(QFile::remove(folder + QLatin1Char('\\') + aside.first()));
        QVERIFY(svc::dropRestartDeletionsUnder(folder));
        QVERIFY(svc::restartDeletionsUnder(folder).isEmpty());
        native::removeTree(folder);
    }
};

QTEST_GUILESS_MAIN(tst_CliNative)
#include "tst_CliNative.moc"
