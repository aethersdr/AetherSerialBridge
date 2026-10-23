// User report 2026-10-16 (the first use of the window on a station): with the elevated window open after Stop, Save &
// apply and Start, every start of the service ended about 3 s in (System log 7031, "terminated unexpectedly"), and the
// service started normally seconds after the window was closed. The cause is not established. This test runs the REAL
// window executable (offscreen, test seams on) against the real SCM, the real WMI firewall provider and the real data
// folder; it exercises the window's side of that sequence, it does not prove a station fix:
//
//  1. the console executable installs the service and applies one port (as an operator would before opening the window);
//  2. the window runs Stop, Save & apply, Start (ASB_TEST_GUI_OPS), each result printed on its stdout; with its own
//     firewall check, the bridge then stops (through SCM) and the window's Start is pressed the moment its check
//     begins (start-on-check);
//  3. with the window STILL OPEN, the service is stopped and started through SCM twice (what SCM's recovery and an
//     operator's Start do) and must reach Running and still be running after its startup firewall check's 5 s budget:
//     round 0 after the window's check ended, round 1 the moment the window's check begins.
//
// Three rows: with the window's own stale-rule check (the product), without it (ASB_TEST_GUI_NO_FIREWALL_CHECK), and
// a report-only row with the window as bca3030 built it (ASB_TEST_GUI_AS_BCA3030), which prints REPRODUCED or NOT
// REPRODUCED and passes either way. On a failure a row prints SCM's exit codes, the window's output, whether
// engine.lock can be taken from here, and a fresh ActiveStore verdict with its reason. Everything a row creates is
// removed by it.
//
// v1.1 WS-17 adds the window's lifecycle on the real system, each window action followed by the handle probe
// (HandleProbe.h: no handle of the window's process under the program or the data folder, INV-128):
// installFromWindow, installFromMainButton (the v1.1 fix after WS-17), updateFromWindow (cancel, keep, update), uninstallFromWindow (settings kept / removed),
// standardUserWindow (INV-130: the window started as a fresh standard user), handleSurvey (report only: every file
// handle after each existing action) and handleProbeControl (a window that keeps the v1.0 lock-directory handle must
// fail the probe). The first row's start-on-check also asserts the GUI thread never blocked while the check ran
// (INV-129: the window's 100 ms heartbeat has no gap over 500 ms).

#include "HandleProbe.h"
#include "ServiceNativeSupport.h"
#include "WinNativeSupport.h"
#include "core/ConfigJson.h"
#include "platform/win/CimFirewall.h"
#include "platform/win/WbemSession.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTest>

#include <userenv.h>

using namespace asb;
using namespace asb::test;

namespace {

struct Run {
    int code = -1;
    QString out;
};

Run cli(const QStringList& args, int ms = 300000)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(svc::headlessExe(), args);
    Run r;
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
    qInfo("$ aether-serial-bridge %s  -> exit %d\n%s", qPrintable(args.join(QLatin1Char(' '))), r.code, qPrintable(r.out.trimmed()));
    return r;
}

// The rules in the bridge's firewall group (Active store).
int ruleCount()
{
    win::WbemSession cim;
    cim.connect();
    return int(win::CimFirewall::readGroup(cim, win::cim::Store::Active).size());
}

QString sha256Of(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QStringLiteral("(unreadable: %1)").arg(f.errorString());
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return QString::fromLatin1(h.result().toHex());
}

QByteArray fileBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// What is left in the program folder besides the dev runtime this test copied there (empty = nothing).
QString programFolderLeft(const QString& folder, const QStringList& devRuntime)
{
    if (!QFileInfo::exists(folder)) return {};
    QStringList other;
    if (devRuntime.isEmpty()) other << QStringLiteral("(the folder itself)");
    for (const QString& n : QDir(folder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
        if (!devRuntime.contains(n, Qt::CaseInsensitive)) other << n;
    return other.join(QStringLiteral(", "));
}

QString describe(const svc::ScmState& s)
{
    return QStringLiteral("SCM: exists %1, state %2, pid %3, win32 exit %4, service exit %5")
        .arg(int(s.exists))
        .arg(s.state)
        .arg(s.pid)
        .arg(s.win32ExitCode)
        .arg(s.specificExitCode);
}

}  // namespace

class tst_GuiNative : public QObject {
    Q_OBJECT

    std::unique_ptr<win::WindowsPlatform> m_p;
    bool m_hadInstallFolders = false;
    QProcess* m_gui = nullptr;
    QStringList m_guiLines;
    QString m_scratch;     // this test's scratch folder (asb-ws6-gui-*): the wait handshake's file, copies, records
    QString m_continue;    // ASB_TEST_GUI_CONTINUE: the window's "wait" op goes on when it appears

    // What the window printed so far (its stdout and stderr, merged).
    void readGui()
    {
        if (!m_gui) return;
        while (m_gui->canReadLine()) {
            const QString line = QString::fromUtf8(m_gui->readLine()).trimmed();
            if (line.isEmpty()) continue;
            m_guiLines << line;
            qInfo("window: %s", qPrintable(line));
        }
    }

    void stopGui()
    {
        if (!m_gui) return;
        readGui();
        if (m_gui->state() != QProcess::NotRunning) {
            m_gui->kill();
            m_gui->waitForFinished(10000);
        }
        readGui();
        delete m_gui;
        m_gui = nullptr;
    }

    // The evidence for a failed start: SCM, the window, engine.lock from here, a fresh ActiveStore verdict.
    QString diagnose(const svc::ScmState& s)
    {
        readGui();
        QStringList d;
        d << describe(s);
        d << QStringLiteral("window running: %1").arg(int(m_gui && m_gui->state() != QProcess::NotRunning));
        LockResult lr = LockResult::Busy;
        {
            std::unique_ptr<LockHold> hold = m_p->platform().engineLock.tryAcquire(&lr);
            d << QStringLiteral("engine.lock from the test process: %1").arg(hold ? QStringLiteral("free") : QStringLiteral("not acquired (%1)").arg(int(lr)));
        }
        try {
            QString why;
            const std::optional<IStartToken::Token> token = m_p->platform().token.read(&why);
            QFile f(QDir(m_p->dataFolder()).filePath(QStringLiteral("config.json")));
            std::optional<BridgeConfig> config;
            if (f.open(QIODevice::ReadOnly)) config = fromJson(f.readAll(), nullptr);
            if (token && config) {
                win::WbemSession cim;
                win::CimFirewallMonitor monitor(cim, m_p->installedExe());
                monitor.threadInit();
                const IFirewallMonitor::Check c = monitor.checkOnce(*config, token->generation);
                d << QStringLiteral("ActiveStore verdict now: %1 (%2)").arg(int(c)).arg(monitor.lastWhy());
            } else {
                d << QStringLiteral("no verdict: token %1 (%2), config %3").arg(int(token.has_value())).arg(why).arg(int(config.has_value()));
            }
        } catch (const std::exception& e) {
            d << QStringLiteral("no verdict: %1").arg(QString::fromLocal8Bit(e.what()));
        }
        d << QStringLiteral("window output:\n  ") + m_guiLines.join(QStringLiteral("\n  "));
        return d.join(QLatin1Char('\n'));
    }

private slots:
    void initTestCase()
    {
        QString err;
        m_p = svc::platform(&err);
        QVERIFY2(m_p, qPrintable(err));
        QVERIFY2(!qEnvironmentVariable("ASB_GUI_EXE").isEmpty(), "ASB_GUI_EXE is not set");
        m_hadInstallFolders = QFileInfo::exists(m_p->lockDirectory()) || QFileInfo::exists(m_p->dataFolder());
        svc::removeServiceHard(m_p->installedExe());
        m_scratch = native::scratch(QStringLiteral("gui"));
        m_continue = m_scratch + QStringLiteral("\\continue");
    }

    void cleanup() { stopGui(); }

    void cleanupTestCase()
    {
        stopGui();
        if (!m_p) return;
        if (svc::query().exists) cli({QStringLiteral("--uninstall")});
        svc::removeServiceHard(m_p->installedExe());
        if (!m_hadInstallFolders) {
            native::removeTree(m_p->lockDirectory());
            native::removeTree(m_p->dataFolder());
        }
        if (!m_scratch.isEmpty()) native::removeTree(m_scratch);
    }

private:
    // The index of the first line at or after `from` that starts with `prefix`, reading the window while waiting; -1.
    int waitLine(const QString& prefix, int from, int ms)
    {
        QElapsedTimer t;
        t.start();
        for (;;) {
            readGui();
            for (int i = from; i < m_guiLines.size(); ++i)
                if (m_guiLines[i].startsWith(prefix)) return i;
            if (t.elapsed() >= ms || !m_gui || m_gui->state() == QProcess::NotRunning) return -1;
            m_gui->waitForReadyRead(200);
        }
    }

    // The real window with its test seams: `ops` (ASB_TEST_GUI_OPS), `answers` (ASB_TEST_GUI_ANSWERS), and more.
    bool startWindow(const QString& ops, const QString& answers, const QList<QPair<QString, QString>>& more = {})
    {
        stopGui();
        QFile::remove(m_continue);
        m_guiLines.clear();
        m_gui = new QProcess;
        m_gui->setProcessChannelMode(QProcess::MergedChannels);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        env.insert(QStringLiteral("ASB_TEST_GUI_OPS"), ops);
        env.insert(QStringLiteral("ASB_TEST_GUI_ANSWERS"), answers);
        env.insert(QStringLiteral("ASB_TEST_GUI_CONTINUE"), m_continue);
        env.insert(QStringLiteral("ASB_TEST_GUI_QUIT_MS"), QStringLiteral("900000"));
        for (const auto& kv : more) env.insert(kv.first, kv.second);
        m_gui->setProcessEnvironment(env);
        m_gui->start(qEnvironmentVariable("ASB_GUI_EXE"), {});
        return m_gui->waitForStarted(30000);
    }

    // The result line of `op` at or after `from` ("" when none came within `ms`).
    QString opResult(const QString& op, int from, int* at, int ms = 300000)
    {
        *at = waitLine(QStringLiteral("asb-gui-op %1 ").arg(op), from, ms);
        return *at >= 0 ? m_guiLines[*at] : QString();
    }

    // After an op: wait for the window's "wait" (it is paused now), return its line index; -1 when it never came.
    int paused(int from) { return waitLine(QStringLiteral("asb-gui-op wait"), from, 120000); }

    // Releases the paused window; the index of its "wait done" line (-1 when it did not go on).
    int release(int from)
    {
        QFile f(m_continue);
        if (!f.open(QIODevice::WriteOnly)) return -1;
        f.close();
        return waitLine(QStringLiteral("asb-gui-op wait done"), from, 30000);
    }

    // INV-128: the window's process holds no handle under the program folder or the data folder. Empty = nothing held.
    QString heldByWindow(const QString& when)
    {
        if (!m_gui || m_gui->state() == QProcess::NotRunning) return QStringLiteral("%1: the window is not running").arg(when);
        const handles::Probe p = handles::probe(quint32(m_gui->processId()), {m_p->lockDirectory(), m_p->dataFolder()});
        if (!p.ok) return QStringLiteral("%1: the handle probe failed: %2").arg(when, p.error);
        qInfo("%s: %d disk-file handle(s) in the window, %d under the program or data folder", qPrintable(when),
              int(p.files.size()), int(p.under.size()));
        if (p.under.isEmpty()) return {};
        return QStringLiteral("%1: the window holds\n%2").arg(when, p.describe());
    }

    // Installed from the console with one loopback port, running.
    QString installWithPort(const QString& name, const QString& com, int port)
    {
        svc::deployDevRuntime(m_p->lockDirectory());
        Run r = cli({QStringLiteral("--install")});
        if (r.code != 0) return r.out;
        if (!svc::waitState(SERVICE_RUNNING, 15000)) return QStringLiteral("not running after --install");
        r = cli({QStringLiteral("--add-port"), name, com, QString::number(port), QStringLiteral("--bind"), QStringLiteral("127.0.0.1")});
        if (r.code != 0) return r.out;
        r = cli({QStringLiteral("--apply")});
        if (r.code != 0) return r.out;
        if (!svc::waitState(SERVICE_RUNNING, 15000)) return QStringLiteral("not running after --apply");
        return {};
    }

    // The window, the Qt DLLs and Qt's plugin folders, copied into `to` (standardUserWindow). The copy's path, or empty.
    static QString copyWindow(const QString& to, QString* error)
    {
        auto copyFile = [&](const QString& from, const QString& dest) {
            QDir().mkpath(QFileInfo(dest).absolutePath());
            if (QFile::copy(from, dest)) return true;
            *error = QStringLiteral("could not copy %1 to %2").arg(from, dest);
            return false;
        };
        const QString exe = QDir::toNativeSeparators(qEnvironmentVariable("ASB_GUI_EXE"));
        const QString target = to + QLatin1Char('\\') + QFileInfo(exe).fileName();
        if (!copyFile(exe, target)) return {};
        const QDir bin(QLibraryInfo::path(QLibraryInfo::BinariesPath));
        for (const QFileInfo& f : bin.entryInfoList({QStringLiteral("*.dll")}, QDir::Files))
            if (!copyFile(f.absoluteFilePath(), to + QLatin1Char('\\') + f.fileName())) return {};
        const QString plugins = QLibraryInfo::path(QLibraryInfo::PluginsPath);
        QDirIterator it(plugins, {QStringLiteral("*.dll")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString f = it.next();
            if (!copyFile(f, QDir::toNativeSeparators(to + QLatin1Char('/') + QDir(plugins).relativeFilePath(f)))) return {};
        }
        return target;
    }

    // After a row: no service, no entry, no rules; the folders as before the test.
    void removeEverything()
    {
        stopGui();
        if (svc::query().exists) cli({QStringLiteral("--uninstall")});
        svc::removeServiceHard(m_p->installedExe());
        svc::removeAppsEntry();
        if (!m_hadInstallFolders) {
            native::removeTree(m_p->lockDirectory());
            native::removeTree(m_p->dataFolder());
        }
    }

private slots:
    void serviceStartsWhileTheWindowIsOpen_data()
    {
        QTest::addColumn<QString>("mode");   // product | no-window-check | bca3030 (report only)
        QTest::addColumn<int>("port");
        QTest::addColumn<QString>("com");   // each row adds its own port to the kept config: its own device name too
        QTest::newRow("window-with-its-firewall-check") << QStringLiteral("product") << 45231 << QStringLiteral("COM250");
        QTest::newRow("window-without-its-firewall-check") << QStringLiteral("no-window-check") << 45232 << QStringLiteral("COM251");
        // The same run as the first row with the window as bca3030 built it (no wait before an
        // operation, no deadline on its check, a check while starting, the worker's session kept). Report only: the
        // row records whether the station's failure reproduces here and passes either way.
        QTest::newRow("bca3030-behaviour-report-only") << QStringLiteral("bca3030") << 45233 << QStringLiteral("COM252");
    }

    void serviceStartsWhileTheWindowIsOpen()
    {
        QFETCH(QString, mode);
        QFETCH(int, port);
        QFETCH(QString, com);
        const bool windowCheck = mode != QLatin1String("no-window-check");
        const bool reportOnly = mode == QLatin1String("bca3030");
        QStringList failures;   // report-only row: what failed (the product rows fail at once instead)
        auto expect = [&](bool ok, const QString& what) {
            if (ok) return true;
            failures << what;
            return false;
        };

        // 1. Installed and applied from the console, as on the station before the window was opened.
        svc::removeServiceHard(m_p->installedExe());
        qInfo("dev runtime: %s", qPrintable(svc::deployDevRuntime(m_p->lockDirectory()).join(QStringLiteral(", "))));
        Run r = cli({QStringLiteral("--install")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY(svc::waitState(SERVICE_RUNNING, 15000));
        r = cli({QStringLiteral("--add-port"), QStringLiteral("GuiNative%1").arg(port), com, QString::number(port),
                 QStringLiteral("--bind"), QStringLiteral("127.0.0.1")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        r = cli({QStringLiteral("--apply")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY(svc::waitState(SERVICE_RUNNING, 15000));

        // 2. The window: Stop, Save & apply, Start; then (with its check) the Start pressed the moment the window's
        //    own firewall check begins, after the bridge stopped by itself (here: through SCM).
        m_guiLines.clear();
        m_gui = new QProcess;
        m_gui->setProcessChannelMode(QProcess::MergedChannels);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        env.insert(QStringLiteral("ASB_TEST_GUI_OPS"),
                   windowCheck ? QStringLiteral("stop,save,start,start-on-check") : QStringLiteral("stop,save,start"));
        env.insert(QStringLiteral("ASB_TEST_GUI_QUIT_MS"), QStringLiteral("600000"));
        if (!windowCheck) env.insert(QStringLiteral("ASB_TEST_GUI_NO_FIREWALL_CHECK"), QStringLiteral("1"));
        if (reportOnly) env.insert(QStringLiteral("ASB_TEST_GUI_AS_BCA3030"), QStringLiteral("1"));
        m_gui->setProcessEnvironment(env);
        m_gui->start(qEnvironmentVariable("ASB_GUI_EXE"), {});
        QVERIFY2(m_gui->waitForStarted(30000), qPrintable(m_gui->errorString()));
        for (const char* op : {"stop", "save", "start"}) {
            const int at = waitLine(QStringLiteral("asb-gui-op %1 ").arg(QLatin1String(op)), 0, 240000);
            QVERIFY2(at >= 0, qPrintable(QStringLiteral("no result for %1\n%2").arg(QLatin1String(op), diagnose(svc::query()))));
            QVERIFY2(m_guiLines[at].startsWith(QStringLiteral("asb-gui-op %1 Ok:").arg(QLatin1String(op))),
                     qPrintable(diagnose(svc::query())));
        }
        svc::ScmState st;
        QVERIFY2(svc::waitState(SERVICE_RUNNING, 15000, &st), qPrintable(diagnose(st)));
        if (windowCheck) {
            QVERIFY2(waitLine(QStringLiteral("asb-gui-op start-on-check armed"), 0, 30000) >= 0, qPrintable(diagnose(st)));
            const int from = m_guiLines.size();
            QCOMPARE(svc::stopSync(), 0u);   // the bridge stops (SCM); the token stays, so the window checks it
            const int pressed = waitLine(QStringLiteral("asb-gui-op start-on-check pressed"), from, 30000);
            QVERIFY2(pressed >= 0, qPrintable(QStringLiteral("the window's check never began\n%1").arg(diagnose(svc::query()))));
            const int result = waitLine(QStringLiteral("asb-gui-op start-on-check "), pressed + 1, 120000);
            QVERIFY2(result >= 0, qPrintable(diagnose(svc::query())));
            const bool ok = m_guiLines[result].startsWith(QStringLiteral("asb-gui-op start-on-check Ok:"));
            const int end = waitLine(QStringLiteral("asb-gui-firewall-check end"), from, 1);
            qInfo("start pressed while the window's check ran: %s (check ended at line %d, result at line %d)",
                  qPrintable(m_guiLines[result]), end, result);
            if (reportOnly) expect(ok, QStringLiteral("start while the window's check ran: ") + m_guiLines[result]);
            else QVERIFY2(ok, qPrintable(diagnose(svc::query())));
            if (!reportOnly)   // the fix: the operation waited for the check, so the check ended before the result
                QVERIFY2(end >= 0 && end < result, qPrintable(QStringLiteral("check end %1, result %2").arg(end).arg(result)));
            if (!reportOnly) {
                // v1.1 INV-129: while that check ran, the window's GUI thread never blocked (its 100 ms heartbeat's
                // longest gap, printed when the check ends).
                static const QRegularExpression gapLine(QStringLiteral("^asb-gui-heartbeat max-gap (\\d+)$"));
                int gaps = 0;
                for (int i = from; i < m_guiLines.size(); ++i) {
                    const QRegularExpressionMatch m = gapLine.match(m_guiLines[i]);
                    if (!m.hasMatch()) continue;
                    ++gaps;
                    QVERIFY2(m.captured(1).toInt() < 500, qPrintable(QStringLiteral("the GUI thread blocked: %1").arg(m_guiLines[i])));
                }
                QVERIFY2(gaps > 0, "no heartbeat line: the window's check never reported its end");
            }
        }
        QVERIFY2(waitLine(QStringLiteral("asb-gui-ops done"), 0, 60000) >= 0, qPrintable(diagnose(svc::query())));
        if (!svc::waitState(SERVICE_RUNNING, 15000, &st)) {
            if (!reportOnly) QFAIL(qPrintable(diagnose(st)));
            expect(false, QStringLiteral("not running after the window's operations: ") + describe(st));
            QCOMPARE(svc::startRaw(), 0u);
            svc::waitState(SERVICE_RUNNING, 20000, &st);
        }

        // 3. With the window still open: SCM stops and starts the service; it must come up and stay up past its
        //    startup firewall check (5 s budget; a station saw it end about 3 s in, error 1067). Round 0
        //    lets the window's check finish first; round 1 starts the service the moment the window's check begins
        //    (no wait), so the two firewall reads overlap.
        for (int round = 0; round < 2; ++round) {
            const int from = m_guiLines.size();
            QCOMPARE(svc::stopSync(), 0u);
            QVERIFY2(svc::waitState(SERVICE_STOPPED, 30000, &st), qPrintable(diagnose(st)));
            QString overlap = QStringLiteral("none");
            if (round == 0) {
                QTest::qWait(3000);   // the window sees it stopped (its check, when on, runs and ends now)
            } else if (windowCheck) {
                const int began = waitLine(QStringLiteral("asb-gui-firewall-check start"), from, 15000);
                overlap = began >= 0 ? (waitLine(QStringLiteral("asb-gui-firewall-check end"), began, 1) >= 0
                                            ? QStringLiteral("the check had already ended")
                                            : QStringLiteral("the window's check was in flight"))
                                     : QStringLiteral("the window's check did not begin within 15 s");
            }
            readGui();
            QVERIFY2(m_gui->state() != QProcess::NotRunning, qPrintable(diagnose(st)));
            QCOMPARE(svc::startRaw(), 0u);
            const bool running = svc::waitState(SERVICE_RUNNING, 20000, &st);
            const quint32 pid = st.pid;
            if (running) QTest::qWait(8000);
            const svc::ScmState after = svc::query();
            const bool stayed = running && after.state == SERVICE_RUNNING && after.pid == pid;
            const QString what = QStringLiteral("round %1 (%2): %3").arg(round).arg(overlap,
                                     stayed ? QStringLiteral("the service started and is still running 8 s later")
                                            : QStringLiteral("the service did not stay up\n") + diagnose(after));
            qInfo("%s", qPrintable(what));
            if (reportOnly) {
                if (!expect(stayed, what)) {
                    svc::waitState(SERVICE_STOPPED, 30000, &st);
                    continue;
                }
            } else {
                QVERIFY2(stayed, qPrintable(what));
                QVERIFY(svc::canConnect(port, 5000));
            }
        }
        if (reportOnly)
            qInfo("bca3030 row (report only): %s", failures.isEmpty()
                      ? "NOT REPRODUCED: every start succeeded with the window as bca3030 built it"
                      : qPrintable(QStringLiteral("REPRODUCED:\n  ") + failures.join(QStringLiteral("\n  "))));
        stopGui();
        r = cli({QStringLiteral("--uninstall")});
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY(svc::waitDeleted(60000));
    }

    // ---- v1.1 WS-17 --------------------------------------------------------------------------------------------

    // Report only (§4's survey): every disk-file handle of the window after each existing action, and those under the
    // program or data folder. The rows below assert; this one prints what is there.
    void handleSurvey()
    {
        removeEverything();
        const QString setup = installWithPort(QStringLiteral("GuiSurvey"), QStringLiteral("COM253"), 45240);
        QVERIFY2(setup.isEmpty(), qPrintable(setup));
        QVERIFY(startWindow(QStringLiteral("wait,stop,wait,save,wait,start,wait,reload,wait"), QStringLiteral("exposure=1")));
        int from = 0;
        for (const char* step : {"load", "stop", "save", "start", "reload"}) {
            const QString what = QLatin1String(step);
            if (what != QLatin1String("load")) {
                int at = -1;
                const QString line = opResult(what, from, &at);
                qInfo("survey %s: %s", step, qPrintable(line.isEmpty() ? QStringLiteral("(no result)") : line));
                if (at >= 0) from = at + 1;
            }
            const int w = paused(from);
            QVERIFY2(w >= 0, qPrintable(diagnose(svc::query())));
            const handles::Probe p = handles::probe(quint32(m_gui->processId()), {m_p->lockDirectory(), m_p->dataFolder()});
            qInfo("survey after %s: probe %s%s\nunder the program or data folder:\n%s\nevery disk-file handle:\n%s", step,
                  p.ok ? "ok" : "FAILED ", qPrintable(p.error), qPrintable(p.describe()), qPrintable(p.describeAll()));
            from = release(w + 1);
            QVERIFY(from >= 0);
        }
        removeEverything();
    }

    // The probe's control: a window that keeps the v1.0 lock-directory handle (ASB_TEST_KEEP_LOCK_DIRECTORY) must
    // fail it after an operation. Without this row a probe that sees nothing would pass every row above.
    void handleProbeControl()
    {
        removeEverything();
        const QString setup = installWithPort(QStringLiteral("GuiControl"), QStringLiteral("COM254"), 45241);
        QVERIFY2(setup.isEmpty(), qPrintable(setup));
        QVERIFY(startWindow(QStringLiteral("stop,wait,start,wait"), QStringLiteral("exposure=1"),
                            {{QStringLiteral("ASB_TEST_KEEP_LOCK_DIRECTORY"), QStringLiteral("1")}}));
        int at = -1;
        QVERIFY2(opResult(QStringLiteral("stop"), 0, &at).startsWith(QStringLiteral("asb-gui-op stop Ok:")), qPrintable(diagnose(svc::query())));
        int w = paused(at + 1);
        QVERIFY(w >= 0);
        const QString held = heldByWindow(QStringLiteral("control, after stop"));
        qInfo("control: %s", qPrintable(held.isEmpty() ? QStringLiteral("nothing found") : held));
        QVERIFY2(held.contains(QLatin1String("the window holds")),
                 "the probe found no handle in a window that keeps the lock directory's handle: it would pass anything");
        QVERIFY2(held.contains(QDir::toNativeSeparators(m_p->lockDirectory()), Qt::CaseInsensitive) ||
                     held.contains(QLatin1String("\\Device\\"), Qt::CaseInsensitive),
                 qPrintable(held));
        QVERIFY(release(w + 1) >= 0);
        QVERIFY2(opResult(QStringLiteral("start"), w + 1, &at).startsWith(QStringLiteral("asb-gui-op start Ok:")), qPrintable(diagnose(svc::query())));
        removeEverything();
    }

    // Nothing installed, the settings kept from an earlier install: the window's Install installs and applies them.
    void installFromWindow()
    {
        removeEverything();
        const int port = 45242;
        const QString setup = installWithPort(QStringLiteral("GuiInstall"), QStringLiteral("COM255"), port);
        QVERIFY2(setup.isEmpty(), qPrintable(setup));
        Run r = cli({QStringLiteral("--uninstall")});   // keeps the settings
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY(svc::waitDeleted(60000));
        QVERIFY(QFileInfo::exists(m_p->dataFolder() + QStringLiteral("\\config.json")));
        svc::deployDevRuntime(m_p->lockDirectory());   // the dev build's service needs its Qt beside it

        QVERIFY(startWindow(QStringLiteral("install,wait"), QStringLiteral("install:install,exposure=1")));
        int at = -1;
        const QString line = opResult(QStringLiteral("install"), 0, &at);
        QVERIFY2(line.startsWith(QStringLiteral("asb-gui-op install Ok:")), qPrintable(line + QLatin1Char('\n') + diagnose(svc::query())));
        const int w = paused(at + 1);
        QVERIFY(w >= 0);
        svc::ScmState st;
        QVERIFY2(svc::waitState(SERVICE_RUNNING, 15000, &st), qPrintable(diagnose(st)));
        QVERIFY(svc::canConnect(port, 5000));
        QCOMPARE(svc::appsEntryValue("DisplayName"), QStringLiteral("Aether Serial Bridge"));
        QVERIFY(!svc::appsEntryValue("DisplayVersion").isEmpty());
        QCOMPARE(sha256Of(m_p->installedExe()), sha256Of(svc::headlessExe()));   // this build's program (D60)
        const QString held = heldByWindow(QStringLiteral("after install"));
        QVERIFY2(held.isEmpty(), qPrintable(held));
        QVERIFY(release(w + 1) >= 0);
        QVERIFY(waitLine(QStringLiteral("asb-gui-ops done"), w + 1, 30000) >= 0);
        removeEverything();
    }

    // v1.1 fix after WS-17: on a clean machine the main button reads Install and apply (the box loads ticked) and its
    // click installs, as Install the bridge service does (the same question); then Uninstall… from the same window.
    // (v1.1 as merged with WS-17 saved and removed instead: "Saved, not applied", nothing installed.)
    void installFromMainButton()
    {
        removeEverything();
        const int port = 45247;
        const QStringList dev = svc::deployDevRuntime(m_p->lockDirectory());
        const QString setup = installWithPort(QStringLiteral("GuiMainButton"), QStringLiteral("COM259"), port);
        QVERIFY2(setup.isEmpty(), qPrintable(setup));
        Run r = cli({QStringLiteral("--uninstall")});   // keeps the settings: the window shows that port
        QVERIFY2(r.code == 0, qPrintable(r.out));
        QVERIFY(svc::waitDeleted(60000));
        svc::deployDevRuntime(m_p->lockDirectory());

        QVERIFY(startWindow(QStringLiteral("save,wait,uninstall,wait"), QStringLiteral("install:install,uninstall:uninstall,exposure=1")));
        int at = -1;
        QString line = opResult(QStringLiteral("save"), 0, &at);
        QVERIFY2(line.startsWith(QStringLiteral("asb-gui-op save Ok:")), qPrintable(line + QLatin1Char('\n') + diagnose(svc::query())));
        int w = paused(at + 1);
        QVERIFY(w >= 0);
        svc::ScmState st;
        QVERIFY2(svc::waitState(SERVICE_RUNNING, 15000, &st), qPrintable(diagnose(st)));
        QVERIFY(svc::canConnect(port, 5000));
        QCOMPARE(sha256Of(m_p->installedExe()), sha256Of(svc::headlessExe()));
        QVERIFY(svc::appsEntryExists());
        QString held = heldByWindow(QStringLiteral("after Install and apply"));
        QVERIFY2(held.isEmpty(), qPrintable(held));

        int done = release(w + 1);
        QVERIFY(done >= 0);
        line = opResult(QStringLiteral("uninstall"), done + 1, &at);
        QVERIFY2(line.startsWith(QStringLiteral("asb-gui-op uninstall Ok:")), qPrintable(line + QLatin1Char('\n') + diagnose(svc::query())));
        w = paused(at + 1);
        QVERIFY(w >= 0);
        QVERIFY(svc::waitDeleted(60000));
        QCOMPARE(ruleCount(), 0);
        QVERIFY(!svc::appsEntryExists());
        const QString left = programFolderLeft(m_p->lockDirectory(), dev);
        QVERIFY2(left.isEmpty(), qPrintable(QStringLiteral("left in the program folder: %1").arg(left)));
        held = heldByWindow(QStringLiteral("after uninstall"));
        QVERIFY2(held.isEmpty(), qPrintable(held));
        QVERIFY(release(w + 1) >= 0);
        QVERIFY(waitLine(QStringLiteral("asb-gui-ops done"), w + 1, 30000) >= 0);
        removeEverything();
    }

    // Another build installed (the console exe with one byte appended: another digest, still runs): Save & apply's
    // build question answered Cancel (nothing changes), No, keep it (applied, that program kept), Update (this build);
    // then, with the other build installed again, the window's Update to this build button.
    void updateFromWindow()
    {
        removeEverything();
        const int port = 45243;
        const QString other = m_scratch + QStringLiteral("\\aether-serial-bridge.exe");
        QFile::remove(other);
        QVERIFY(QFile::copy(svc::headlessExe(), other));
        {
            QFile f(other);
            QVERIFY(f.open(QIODevice::Append));
            QCOMPARE(f.write("\0", 1), qint64(1));
        }
        const QString thisBuild = sha256Of(svc::headlessExe());
        const QString otherBuild = sha256Of(other);
        QVERIFY(thisBuild != otherBuild);
        const QString mine = svc::headlessExe();
        svc::setHeadlessExe(other);
        const QString setup = installWithPort(QStringLiteral("GuiUpdate"), QStringLiteral("COM256"), port);
        svc::setHeadlessExe(mine);
        QVERIFY2(setup.isEmpty(), qPrintable(setup));
        QCOMPARE(sha256Of(m_p->installedExe()), otherBuild);

        struct Round {
            const char* answer;
            bool submitted;
            const char* exeAfter;   // "other" or "this"
        };
        for (const Round& round : {Round{"cancel", false, "other"}, Round{"keep", true, "other"}, Round{"update", true, "this"}}) {
            const QString answer = QLatin1String(round.answer);
            const svc::ScmState before = svc::query();
            const QByteArray configBefore = fileBytes(m_p->dataFolder() + QStringLiteral("\\config.json"));
            QVERIFY(startWindow(QStringLiteral("save,wait"), QStringLiteral("build:%1,exposure=1").arg(answer)));
            int at = -1;
            const QString line = opResult(QStringLiteral("save"), 0, &at);
            qInfo("build question answered %s: %s", round.answer, qPrintable(line));
            QVERIFY2(waitLine(QStringLiteral("asb-gui-ask build -> %1").arg(round.submitted ? answer : QStringLiteral("cancelled")), 0, 1) >= 0,
                     qPrintable(m_guiLines.join(QLatin1Char('\n'))));
            if (round.submitted) QVERIFY2(line.startsWith(QStringLiteral("asb-gui-op save Ok:")), qPrintable(line + QLatin1Char('\n') + diagnose(svc::query())));
            else QVERIFY2(line.startsWith(QStringLiteral("asb-gui-op save not-submitted")), qPrintable(line));
            const int w = paused(at + 1);
            QVERIFY(w >= 0);
            svc::ScmState st;
            QVERIFY2(svc::waitState(SERVICE_RUNNING, 15000, &st), qPrintable(diagnose(st)));
            QCOMPARE(sha256Of(m_p->installedExe()), QLatin1String(round.exeAfter) == QLatin1String("this") ? thisBuild : otherBuild);
            if (!round.submitted) {   // INV-131: Cancel changed nothing
                QCOMPARE(st.pid, before.pid);
                QCOMPARE(fileBytes(m_p->dataFolder() + QStringLiteral("\\config.json")), configBefore);
            }
            QVERIFY(svc::canConnect(port, 5000));
            if (answer == QLatin1String("update")) QVERIFY(!svc::appsEntryValue("DisplayVersion").isEmpty());
            const QString held = heldByWindow(QStringLiteral("after the build question answered %1").arg(answer));
            QVERIFY2(held.isEmpty(), qPrintable(held));
            QVERIFY(release(w + 1) >= 0);
            stopGui();
        }

        // The window's own Update to this build button (the update op, its own question answered Update), with the
        // other build installed again.
        removeEverything();
        svc::setHeadlessExe(other);
        const QString again = installWithPort(QStringLiteral("GuiUpdate"), QStringLiteral("COM256"), port);
        svc::setHeadlessExe(mine);
        QVERIFY2(again.isEmpty(), qPrintable(again));
        QCOMPARE(sha256Of(m_p->installedExe()), otherBuild);
        QVERIFY(startWindow(QStringLiteral("update,wait"), QStringLiteral("update:update,exposure=1")));
        int at = -1;
        const QString line = opResult(QStringLiteral("update"), 0, &at);
        QVERIFY2(line.startsWith(QStringLiteral("asb-gui-op update Ok:")), qPrintable(line + QLatin1Char('\n') + diagnose(svc::query())));
        const int w = paused(at + 1);
        QVERIFY(w >= 0);
        svc::ScmState st;
        QVERIFY2(svc::waitState(SERVICE_RUNNING, 15000, &st), qPrintable(diagnose(st)));
        QCOMPARE(sha256Of(m_p->installedExe()), thisBuild);
        QVERIFY(!svc::appsEntryValue("DisplayVersion").isEmpty());
        QVERIFY(svc::canConnect(port, 5000));
        const QString held = heldByWindow(QStringLiteral("after Update to this build"));
        QVERIFY2(held.isEmpty(), qPrintable(held));
        QVERIFY(release(w + 1) >= 0);
        removeEverything();
        QFile::remove(other);
    }

    void uninstallFromWindow_data()
    {
        QTest::addColumn<bool>("removeSettings");
        QTest::newRow("settings-kept") << false;
        QTest::newRow("settings-removed") << true;
    }

    // Uninstall… from the window, the box unticked (the settings kept, a planted file with them) or ticked (the folder
    // removed); then Install from the same window works again.
    void uninstallFromWindow()
    {
        QFETCH(bool, removeSettings);
        removeEverything();
        const int port = removeSettings ? 45245 : 45244;
        const QStringList dev = svc::deployDevRuntime(m_p->lockDirectory());
        const QString setup = installWithPort(QStringLiteral("GuiUninstall"), removeSettings ? QStringLiteral("COM258") : QStringLiteral("COM257"), port);
        QVERIFY2(setup.isEmpty(), qPrintable(setup));
        const QString planted = m_p->dataFolder() + QStringLiteral("\\notes.txt");
        if (!removeSettings) QVERIFY(native::writeFile(planted, QByteArrayLiteral("mine")));
        const QString op = removeSettings ? QStringLiteral("uninstall-settings") : QStringLiteral("uninstall");

        QVERIFY(startWindow(op + QStringLiteral(",wait,install,wait"), QStringLiteral("uninstall:uninstall,install:install,exposure=1")));
        int at = -1;
        QString line = opResult(op, 0, &at);
        QVERIFY2(line.startsWith(QStringLiteral("asb-gui-op %1 Ok:").arg(op)), qPrintable(line + QLatin1Char('\n') + diagnose(svc::query())));
        int w = paused(at + 1);
        QVERIFY(w >= 0);
        QVERIFY(svc::waitDeleted(60000));
        QCOMPARE(ruleCount(), 0);
        QVERIFY(!svc::appsEntryExists());
        const QString left = programFolderLeft(m_p->lockDirectory(), dev);
        QVERIFY2(left.isEmpty(), qPrintable(QStringLiteral("left in the program folder: %1").arg(left)));
        if (removeSettings) {
            QVERIFY2(!QFileInfo::exists(m_p->dataFolder()), qPrintable(QDir(m_p->dataFolder()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).join(QStringLiteral(", "))));
        } else {
            QVERIFY(QFileInfo::exists(m_p->dataFolder() + QStringLiteral("\\config.json")));
            QCOMPARE(native::readFile(planted).value_or(QByteArray()), QByteArrayLiteral("mine"));
        }
        QString held = heldByWindow(QStringLiteral("after %1").arg(op));
        QVERIFY2(held.isEmpty(), qPrintable(held));

        svc::deployDevRuntime(m_p->lockDirectory());
        int done = release(w + 1);
        QVERIFY(done >= 0);
        line = opResult(QStringLiteral("install"), done + 1, &at);
        QVERIFY2(line.startsWith(QStringLiteral("asb-gui-op install Ok:")), qPrintable(line + QLatin1Char('\n') + diagnose(svc::query())));
        w = paused(at + 1);
        QVERIFY(w >= 0);
        svc::ScmState st;
        QVERIFY2(svc::waitState(SERVICE_RUNNING, 15000, &st), qPrintable(diagnose(st)));
        if (!removeSettings) QVERIFY(svc::canConnect(port, 5000));   // the kept settings were installed again
        held = heldByWindow(QStringLiteral("after install again"));
        QVERIFY2(held.isEmpty(), qPrintable(held));
        QVERIFY(release(w + 1) >= 0);
        removeEverything();
    }

    // INV-130: the window started as a fresh standard user (an asbtest-<hex> account, CreateProcessWithLogonW with its
    // profile): its job has limit 2 with kill-on-close, it built no Applier, and Restart as administrator asks for
    // "--gui --intent=install" (recorded through ASB_TEST_ELEVATOR; UAC itself is HT W-2).
    // Two rows. `recorded`: the relaunch's command line through ASB_TEST_ELEVATOR=record (INV-130). `real-child`
    // (v1.1 station fix, 2026-10-18): the relaunch really started, with exactly those arguments, as a child
    // in the window's job (where runas's child lands: AppInfo makes the window its parent); the window then closes
    // as it does after a started relaunch, and the new window must outlive it and show itself. Kills: the window's
    // job with kill-on-job-close (the new window died with the original, so Restart as administrator opened nothing).
    void standardUserWindow_data()
    {
        QTest::addColumn<bool>("realChild");
        QTest::newRow("recorded") << false;
        QTest::newRow("real-child") << true;
    }

    void standardUserWindow()
    {
        QFETCH(bool, realChild);
        removeEverything();
        native::LocalAccount account(false);
        QVERIFY2(account.created() && account.token(), qPrintable(account.error()));
        const QString dir = native::scratch(QStringLiteral("gui-user"));
        const auto cleanDir = qScopeGuard([&] { native::removeTree(dir); });
        // The account may use this folder: its output, its temp files, the elevator's record.
        QVERIFY(native::setSddl(dir, QStringLiteral("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;%1)").arg(account.sid()),
                                DACL_SECURITY_INFORMATION));
        // The runner's work and Qt cache folders are not readable by a standard user (run 39069183645: error 5), so the
        // window runs from a copy in this folder: the executable, the Qt DLLs beside it and Qt's plugin folders beside
        // it too (Qt looks for plugins in the executable's folder).
        const QString app = dir + QStringLiteral("\\app");
        QString copyError;
        const QString exe = copyWindow(app, &copyError);
        QVERIFY2(!exe.isEmpty(), qPrintable(copyError));
        {   // A launch failure says why: the account must be able to read the window and the Qt it loads.
            native::Impersonate as(account.token());
            QVERIFY(as.ok());
            QStringList unreadable;
            for (const QString& f : {exe, app + QStringLiteral("\\Qt6Core.dll")}) {
                HANDLE h = CreateFileW(native::W(f), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (h == INVALID_HANDLE_VALUE) unreadable << QStringLiteral("%1 (%2)").arg(f).arg(GetLastError());
                else CloseHandle(h);
            }
            QVERIFY2(unreadable.isEmpty(), qPrintable(QStringLiteral("the standard user cannot read: ") + unreadable.join(QStringLiteral(", "))));
        }
        const QString record = dir + QStringLiteral("\\elevate.txt");
        const QString childPid = dir + QStringLiteral("\\child.pid");
        const QString outPath = dir + QStringLiteral("\\window.out");
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE out = CreateFileW(native::W(outPath), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, 0, nullptr);
        QVERIFY(out != INVALID_HANDLE_VALUE);
        const auto closeOut = qScopeGuard([&] { CloseHandle(out); });

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char* k : {"USERPROFILE", "APPDATA", "LOCALAPPDATA", "HOMEPATH", "HOMEDRIVE", "USERNAME", "TEMP", "TMP"})
            env.remove(QLatin1String(k));
        env.insert(QStringLiteral("TEMP"), dir);
        env.insert(QStringLiteral("TMP"), dir);
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        env.insert(QStringLiteral("ASB_TEST_GUI_OPS"), QStringLiteral("restart-admin"));
        env.insert(QStringLiteral("ASB_TEST_GUI_QUIT_MS"), QStringLiteral("120000"));
        env.insert(QStringLiteral("ASB_TEST_ELEVATOR"), realChild ? QStringLiteral("child:") + childPid : QStringLiteral("record:") + record);
        std::wstring block;
        for (const QString& kv : env.toStringList()) {
            block += kv.toStdWString();
            block.push_back(L'\0');
        }
        block.push_back(L'\0');
        std::wstring commandLine = QStringLiteral("\"%1\"").arg(exe).toStdWString();
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = out;
        si.hStdError = out;
        // A window station and desktop of this row's own that the account may use: a service's window station is not
        // usable by another account (run 39072296291: the window ended with STATUS_DLL_INIT_FAILED, user32's
        // initialisation). Both go when their handles close, after the window has ended.
        QByteArray stationSd = native::sddl(QStringLiteral("D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;%1)").arg(account.sid()));
        QVERIFY(!stationSd.isEmpty());
        SECURITY_ATTRIBUTES ssa{sizeof ssa, stationSd.data(), FALSE};
        const QString stationName = QStringLiteral("asbws17-%1").arg(GetCurrentProcessId());
        HWINSTA station = CreateWindowStationW(native::W(stationName), 0, WINSTA_ALL_ACCESS, &ssa);
        QVERIFY2(station, qPrintable(QStringLiteral("CreateWindowStationW %1").arg(GetLastError())));
        const auto closeStation = qScopeGuard([&] { CloseWindowStation(station); });
        HWINSTA ownStation = GetProcessWindowStation();
        QVERIFY(SetProcessWindowStation(station));
        HDESK desktop = CreateDesktopW(L"Default", nullptr, nullptr, 0, GENERIC_ALL, &ssa);
        const DWORD desktopError = desktop ? 0 : GetLastError();
        QVERIFY(SetProcessWindowStation(ownStation));
        QVERIFY2(desktop, qPrintable(QStringLiteral("CreateDesktopW %1").arg(desktopError)));
        const auto closeDesktop = qScopeGuard([&] { CloseDesktop(desktop); });
        std::wstring desktopName = (stationName + QStringLiteral("\\Default")).toStdWString();
        si.lpDesktop = desktopName.data();
        PROCESS_INFORMATION pi{};
        if (!CreateProcessWithLogonW(native::W(account.name()), L".", native::W(account.password()), LOGON_WITH_PROFILE,
                                     native::W(exe), commandLine.data(), CREATE_UNICODE_ENVIRONMENT, block.data(),
                                     native::W(dir), &si, &pi))
            QFAIL(qPrintable(QStringLiteral("CreateProcessWithLogonW as %1: %2").arg(account.name()).arg(GetLastError())));
        const auto closeProcess = qScopeGuard([&] {
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 9);
            WaitForSingleObject(pi.hProcess, 10000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            DeleteProfileW(native::W(account.sid()), nullptr, nullptr);
        });
        const DWORD waited = WaitForSingleObject(pi.hProcess, 120000);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        QVERIFY2(waited == WAIT_OBJECT_0, "the standard user's window did not end after Restart as administrator");
        HANDLE child = nullptr;
        const auto endChild = qScopeGuard([&] {
            if (!child) return;
            if (WaitForSingleObject(child, 0) == WAIT_TIMEOUT) TerminateProcess(child, 9);
            WaitForSingleObject(child, 10000);
            CloseHandle(child);
        });
        if (realChild) {
            const DWORD pid = DWORD(fileBytes(childPid).trimmed().toULong());
            QVERIFY2(pid != 0, "the relaunch recorded no child pid");
            child = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            // Already gone (the original's job killed it as it closed) also fails here: nothing to open.
            QVERIFY2(child, qPrintable(QStringLiteral("the relaunched window (pid %1) did not outlive the original (%2)")
                                           .arg(pid).arg(GetLastError())));
            // It keeps running well after the original ended, and shows its window and runs its loop (its `wait` op).
            QVERIFY2(WaitForSingleObject(child, 3000) == WAIT_TIMEOUT, "the relaunched window ended within 3 s of the original");
            QTRY_VERIFY_WITH_TIMEOUT(QString::fromUtf8(fileBytes(outPath)).contains(QStringLiteral("asb-gui-ops done")), 30000);
            QVERIFY2(WaitForSingleObject(child, 0) == WAIT_TIMEOUT, "the relaunched window ended after showing itself");
        }
        const QString printed = QString::fromUtf8(fileBytes(outPath));
        qInfo("the standard user's window (exit %lu):\n%s", static_cast<unsigned long>(code), qPrintable(printed));
        const QStringList lines = printed.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
        QVERIFY2(lines.contains(QStringLiteral("asb-gui-job inJob=1 limit=2 killOnClose=0")), qPrintable(printed));
        QVERIFY2(lines.contains(QStringLiteral("asb-gui-actions no-applier")), qPrintable(printed));
        QVERIFY2(lines.contains(QStringLiteral("asb-gui-op restart-admin started")), qPrintable(printed));
        if (realChild) {
            // Both windows printed their job and their shown window: the original, then the relaunched one.
            QCOMPARE(lines.count(QStringLiteral("asb-gui-window shown=1")), 2);
            QVERIFY2(lines.contains(QStringLiteral("asb-gui-ops done")), qPrintable(printed));
        } else {
            const QStringList recorded = QString::fromUtf8(fileBytes(record)).split(QLatin1Char('\n'));
            QVERIFY2(recorded.size() >= 2, qPrintable(recorded.join(QLatin1Char('|'))));
            QCOMPARE(QFileInfo(recorded[0].trimmed()).fileName().toLower(), QFileInfo(exe).fileName().toLower());   // its own image
            QCOMPARE(recorded[1].trimmed(), QStringLiteral("--gui --intent=install"));
        }
        QVERIFY(!svc::query().exists);   // a standard user's window changed nothing
    }
};

QTEST_GUILESS_MAIN(tst_GuiNative)
#include "tst_GuiNative.moc"
