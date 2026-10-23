// The GUI executable as a process (WS-10): the hidden --self-test-init (WS-13's release self-test, INV-88, run here on
// the development build), and INV-32's GUI row: the window lists device names and never opens a device.

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#ifndef _WIN32
#include "cli/HelperClient.h"
#include "cli/StagedHelper.h"
#include "gui/PkexecClient.h"
#include <QFile>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#endif

namespace {

struct Run {
    int exitCode = -1;
    QProcess::ExitStatus status = QProcess::CrashExit;
    QByteArray out, err;
};

Run run(const QString& exe, const QStringList& args, const QProcessEnvironment& pe, int ms = 60000)
{
    QProcess p;
    p.setProcessEnvironment(pe);
    p.start(exe, args);
    if (!p.waitForFinished(ms)) {
        p.kill();
        p.waitForFinished(5000);
    }
    return {p.exitCode(), p.exitStatus(), p.readAllStandardOutput(), p.readAllStandardError()};
}

QProcessEnvironment guiEnv()
{
    QProcessEnvironment pe = QProcessEnvironment::systemEnvironment();
    pe.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    pe.insert(QStringLiteral("DISPLAY"), QStringLiteral(":99"));   // the display rule (Linux); offscreen ignores it
    return pe;
}

}  // namespace

class tst_GuiProcess : public QObject {
    Q_OBJECT

private slots:
    void selfTestInitReportsExposedPaintedAndModules();
    void portListOpensNothing();
    void pkexecClientRunsTheProgramWithTheRequest();
    void pkexecClientReportsAProgramThatCannotStart();
    void pkexecClientStagedLaunch();
    void pkexecClientStagedCancelWithQueuedPayload();
};

// --self-test-init: the window is built, exposed and painted; one JSON line names every loaded module (the
// executable itself among them); exit 0.
void tst_GuiProcess::selfTestInitReportsExposedPaintedAndModules()
{
    const QString exe = qEnvironmentVariable("ASB_GUI_EXE");
    if (exe.isEmpty()) {
        if (qEnvironmentVariableIsSet("CI")) QFAIL("ASB_GUI_EXE is not set on CI");   // a skip would read as a pass
        QSKIP("the GUI executable is not part of this configuration");
    }
    const Run r = run(exe, {QStringLiteral("--self-test-init")}, guiEnv());
    QCOMPARE(r.status, QProcess::NormalExit);
    QVERIFY2(r.exitCode == 0, r.err.constData());
    const QList<QByteArray> lines = r.out.trimmed().split('\n');
    QJsonParseError e;
    const QJsonDocument d = QJsonDocument::fromJson(lines.last(), &e);
    QVERIFY2(d.isObject(), qPrintable(e.errorString() + QLatin1Char(' ') + QString::fromLocal8Bit(r.out)));
    const QJsonObject o = d.object();
    QCOMPARE(o.value(QStringLiteral("exposed")).toBool(), true);
    QCOMPARE(o.value(QStringLiteral("painted")).toBool(), true);
    const QJsonArray modules = o.value(QStringLiteral("modules")).toArray();
    QVERIFY(!modules.isEmpty());
    const QString self = QDir::cleanPath(QFileInfo(exe).canonicalFilePath());
    bool found = false;
    for (const QJsonValue& m : modules)
        if (QDir::cleanPath(QFileInfo(m.toString()).canonicalFilePath()).compare(self, Qt::CaseInsensitive) == 0) found = true;
    QVERIFY2(found, qPrintable(QStringLiteral("%1 not among the modules").arg(self)));
}

// INV-32 (WS-8's invariant; the GUI row): the window enumerates device names and opens none. A pty slave is injected
// into the list (test builds), the window runs under strace, and no open of the slave path, or of a /proc alias of it,
// appears; the window's own line proves the name was listed.
void tst_GuiProcess::portListOpensNothing()
{
#ifdef _WIN32
    QSKIP("strace observation runs on Linux (the Windows enumeration reads the registry only)");
#else
    const QString exe = qEnvironmentVariable("ASB_GUI_EXE");
    if (exe.isEmpty()) QSKIP("the GUI executable is not part of this configuration");
    const QString strace = QStandardPaths::findExecutable(QStringLiteral("strace"));
    if (strace.isEmpty()) {
        if (qEnvironmentVariableIsSet("CI")) QFAIL("strace is required on the CI Linux leg");
        QSKIP("strace not installed");
    }
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    QVERIFY(master >= 0);
    QVERIFY(grantpt(master) == 0 && unlockpt(master) == 0);
    const QString slave = QString::fromLocal8Bit(ptsname(master));
    QVERIFY(!slave.isEmpty());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString log = dir.filePath(QStringLiteral("strace.log"));
    QProcessEnvironment pe = guiEnv();
    pe.insert(QStringLiteral("ASB_TEST_EXTRA_PORTS"), slave);
    pe.insert(QStringLiteral("ASB_TEST_GUI_QUIT_MS"), QStringLiteral("1500"));
    pe.insert(QStringLiteral("HOME"), dir.path());
    pe.insert(QStringLiteral("XDG_CONFIG_HOME"), dir.filePath(QStringLiteral("config")));
    pe.insert(QStringLiteral("XDG_CACHE_HOME"), dir.filePath(QStringLiteral("cache")));
    const Run r = run(strace, {QStringLiteral("-f"), QStringLiteral("-qq"), QStringLiteral("-e"),
                               QStringLiteral("trace=open,openat,openat2,creat"), QStringLiteral("-o"), log, exe,
                               QStringLiteral("--gui")},
                      pe);
    ::close(master);
    QCOMPARE(r.status, QProcess::NormalExit);
    QVERIFY2(r.exitCode == 0, r.err.constData());
    QVERIFY2(r.err.contains("ASB-TEST-PORTS listed:") && r.err.contains(slave.toLocal8Bit()), r.err.constData());
    QFile f(log);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray trace = f.readAll();
    QVERIFY2(trace.contains("openat("), "strace saw no open at all");   // the observation worked
    QVERIFY2(!trace.contains(slave.toLocal8Bit()), "the window opened the device it listed");
    const QByteArray alias = QStringLiteral("/proc/%1/fd/%2").arg(QCoreApplication::applicationPid()).arg(master).toLocal8Bit();
    QVERIFY(!trace.contains(alias));
#endif
}

// The Linux window's process boundary (WS-10 for WS-9b): the program is started, the request arrives on its stdin
// followed by EOF, and its exit code and outputs come back once, later, on this thread. A stand-in program takes
// pkexec's place (sh: copies stdin to a file, prints a response, exits 1).
void tst_GuiProcess::pkexecClientRunsTheProgramWithTheRequest()
{
#ifdef _WIN32
    QSKIP("the pkexec client is Linux only");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString got = dir.filePath(QStringLiteral("request"));
    asb::gui::PkexecClient client(QStringLiteral("/bin/sh"),
                                  {QStringLiteral("-c"), QStringLiteral("cat > \"$0\"; printf 'response'; printf 'note' >&2; exit 1"), got});
    std::optional<asb::cli::IHelperProcess::Exit> exit;
    bool synchronous = true;
    client.run(QByteArrayLiteral("{\"op\":\"start\"}"), [&](const asb::cli::IHelperProcess::Exit& e) { exit = e; });
    synchronous = exit.has_value();
    QVERIFY(!synchronous);
    QVERIFY(QTest::qWaitFor([&] { return exit.has_value(); }, 20000));
    QVERIFY(exit->started);
    QVERIFY(!exit->crashed);
    QCOMPARE(exit->code, 1);
    QCOMPARE(exit->out, QByteArrayLiteral("response"));
    QCOMPARE(exit->err, QByteArrayLiteral("note"));
    QFile f(got);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArrayLiteral("{\"op\":\"start\"}"));
#endif
}

void tst_GuiProcess::pkexecClientReportsAProgramThatCannotStart()
{
#ifdef _WIN32
    QSKIP("the pkexec client is Linux only");
#else
    QTemporaryDir dir;
    asb::gui::PkexecClient client(dir.filePath(QStringLiteral("no-such-pkexec")), {QStringLiteral("--helper")});
    int calls = 0;
    std::optional<asb::cli::IHelperProcess::Exit> exit;
    client.run(QByteArrayLiteral("{}"), [&](const asb::cli::IHelperProcess::Exit& e) {
        ++calls;
        exit = e;
    });
    QCOMPARE(calls, 0);   // never from inside run()
    QVERIFY(QTest::qWaitFor([&] { return exit.has_value(); }, 20000));
    QTest::qWait(50);
    QCOMPARE(calls, 1);
    QVERIFY(!exit->started);
    // HelperClient reads it as "nothing was changed".
    asb::cli::helper::Request req{asb::cli::helper::Op::Start, std::nullopt};
    const auto o = asb::cli::HelperClient::interpret(req, {}, 0, *exit);
    QCOMPARE(o.kind, asb::cli::HelperClient::Outcome::Kind::Refused);
#endif
}


// v1.1 (D60, WS-15): the staged launch is `pkexec /bin/sh -c <the embedded stage.sh> asb-stage <sha256> <program length>
// <request length>`, the program's bytes then the request on stdin, then EOF. A stand-in records its argv and stdin.
void tst_GuiProcess::pkexecClientStagedLaunch()
{
#ifdef _WIN32
    QSKIP("the pkexec client is Linux only");
#else
    // Beside this test binary, not in /tmp: a CI machine may mount /tmp noexec, and the stand-in is executed.
    QTemporaryDir dir(QCoreApplication::applicationDirPath() + QStringLiteral("/tst_GuiProcess-XXXXXX"));
    QVERIFY(dir.isValid());
    const QString standIn = dir.filePath(QStringLiteral("pkexec"));
    {
        QFile f(standIn);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("#!/bin/sh\nd=$(dirname \"$0\")\nfor a in \"$@\"; do printf '%s\\0' \"$a\"; done > \"$d/argv\"\ncat > \"$d/stdin\"\nexit 0\n");
    }
    QVERIFY(QFile::setPermissions(standIn, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    asb::gui::PkexecClient client(standIn, {});
    const QByteArray program(3 * 1024 * 1024 + 7, 'p'), request("{\"op\":\"install\"}\n");
    const QByteArray sha(64, 'a');
    std::optional<asb::cli::IHelperProcess::Exit> exit;
    client.runStaged(sha, program, request, [&](const asb::cli::IHelperProcess::Exit& e) { exit = e; });
    QVERIFY(!exit.has_value());
    QVERIFY(QTest::qWaitFor([&] { return exit.has_value(); }, 20000));
    QCOMPARE(exit->code, 0);
    QFile argv(dir.filePath(QStringLiteral("argv"))), in(dir.filePath(QStringLiteral("stdin")));
    QVERIFY(argv.open(QIODevice::ReadOnly) && in.open(QIODevice::ReadOnly));
    QList<QByteArray> args = argv.readAll().split('\0');
    args.removeLast();
    QCOMPARE(args, (QList<QByteArray>{"/bin/sh", "-c", asb::cli::staged::script(), "asb-stage", sha,
                                      QByteArray::number(program.size()), QByteArray::number(request.size())}));
    QCOMPARE(in.readAll(), program + request);
#endif
}

// A cancelled prompt: pkexec ends (126) without reading stdin while the whole payload is still queued. The window
// survives the closed pipe (QProcess ignores SIGPIPE) and the outcome is Cancelled.
void tst_GuiProcess::pkexecClientStagedCancelWithQueuedPayload()
{
#ifdef _WIN32
    QSKIP("the pkexec client is Linux only");
#else
    // Beside this test binary, not in /tmp: a CI machine may mount /tmp noexec, and the stand-in is executed.
    QTemporaryDir dir(QCoreApplication::applicationDirPath() + QStringLiteral("/tst_GuiProcess-XXXXXX"));
    QVERIFY(dir.isValid());
    const QString standIn = dir.filePath(QStringLiteral("pkexec"));
    {
        QFile f(standIn);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("#!/bin/sh\nsleep 0.2\necho 'Error executing command as another user: Request dismissed' >&2\nexit 126\n");
    }
    QVERIFY(QFile::setPermissions(standIn, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    asb::gui::PkexecClient client(standIn, {});
    const QByteArray program(32 * 1024 * 1024, 'p'), request("{\"op\":\"uninstall\",\"removeSettings\":false}\n");
    std::optional<asb::cli::IHelperProcess::Exit> exit;
    client.runStaged(QByteArray(64, 'a'), program, request, [&](const asb::cli::IHelperProcess::Exit& e) { exit = e; });
    QVERIFY(QTest::qWaitFor([&] { return exit.has_value(); }, 20000));
    QVERIFY(exit->started && !exit->crashed);
    QCOMPARE(exit->code, 126);
    asb::cli::helper::Request req;
    req.op = asb::cli::helper::Op::Uninstall;
    const auto o = asb::cli::HelperClient::interpret(req, {}, 0, *exit, asb::cli::Route::Staged, {});
    QCOMPARE(o.kind, asb::cli::HelperClient::Outcome::Kind::Cancelled);
#endif
}

QTEST_GUILESS_MAIN(tst_GuiProcess)
#include "tst_GuiProcess.moc"
