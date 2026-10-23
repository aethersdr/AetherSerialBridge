// Mode dispatch (master 4.10): the display/TTY rules, and the real executables' exit codes.
// The rules are pure (decide()); the process cases prove the executables obey them and that a
// QApplication is constructed only on the GUI path.

#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QProcessEnvironment>
#include <QtTest>

#include "app/Dispatch.h"

using namespace asb::app;

namespace {

DispatchEnv env(bool tty, bool display, bool windows, const char* term = "xterm") {
    DispatchEnv e;
    e.stdinIsTty = tty;
    e.stdoutIsTty = tty;
    e.hasDisplay = display;
    e.windows = windows;
    e.term = term;
    return e;
}

struct Result {
    int exitCode = -1;
    QProcess::ExitStatus status = QProcess::CrashExit;
    QString out;
    QString err;
};

Result run(const QString& exe, const QStringList& args, const QProcessEnvironment& pe) {
    QProcess proc;
    proc.setProcessEnvironment(pe);
    proc.start(exe, args);
    if (!proc.waitForFinished(30000)) {
        proc.kill();
        proc.waitForFinished(5000);
    }
    return {proc.exitCode(), proc.exitStatus(), QString::fromLocal8Bit(proc.readAllStandardOutput()),
            QString::fromLocal8Bit(proc.readAllStandardError())};
}

}  // namespace

class tst_Dispatch : public QObject {
    Q_OBJECT

private slots:
    // --- the pure rules (WS-9 "Dispatch") -----------------------------------------------------
    void guiBuild_noArgs_windows_isGui() {
        QCOMPARE(decide(BuildKind::Gui, env(false, false, true), {}).action, Action::Gui);
    }
    void guiBuild_noArgs_linuxWithDisplay_isGui() {
        QCOMPARE(decide(BuildKind::Gui, env(true, true, false), {}).action, Action::Gui);
    }
    // No arguments on a terminal mean the TUI (WS-12; kTuiAvailable kept the help, exit 2, before it existed).
    void guiBuild_noArgs_linuxNoDisplayTty_isTuiWhenThereIsOne() {
        const Decision d = decide(BuildKind::Gui, env(true, false, false), {});
        QCOMPARE(d.action, kTuiAvailable ? Action::Tui : Action::Help);
        if constexpr (!kTuiAvailable) QCOMPARE(d.exitCode, 2);
    }
    void guiBuild_noArgs_linuxNoDisplayNoTty_isHelpExit2() {
        const Decision d = decide(BuildKind::Gui, env(false, false, false), {});
        QCOMPARE(d.action, Action::Help);
        QCOMPARE(d.exitCode, 2);
    }
    void headless_noArgs_tty_isTuiWhenThereIsOne() {
        const Decision d = decide(BuildKind::Headless, env(true, false, false), {});
        QCOMPARE(d.action, kTuiAvailable ? Action::Tui : Action::Help);
        if constexpr (!kTuiAvailable) QCOMPARE(d.exitCode, 2);
    }
    void headless_noArgs_ttyIgnoresDisplay() {
        QCOMPARE(decide(BuildKind::Headless, env(true, true, false), {}).action,
                 kTuiAvailable ? Action::Tui : Action::Help);
    }
    void headless_noArgs_dumbTerm_isHelpExit2() {
        const Decision d = decide(BuildKind::Headless, env(true, false, false, "dumb"), {});
        QCOMPARE(d.action, Action::Help);
        QCOMPARE(d.exitCode, 2);
    }
    void headless_noArgs_noTty_isHelpExit2() {
        const Decision d = decide(BuildKind::Headless, env(false, true, false), {});
        QCOMPARE(d.action, Action::Help);
        QCOMPARE(d.exitCode, 2);
    }
    void headless_gui_isExit2() {
        const Decision d = decide(BuildKind::Headless, env(true, true, false), {"--gui"});
        QCOMPARE(d.action, Action::UsageError);
        QCOMPARE(d.exitCode, 2);
    }
    void guiBuild_gui_needsDisplayOnLinux() {
        QCOMPARE(decide(BuildKind::Gui, env(true, false, false), {"--gui"}).exitCode, 2);
        QCOMPARE(decide(BuildKind::Gui, env(true, true, false), {"--gui"}).action, Action::Gui);
        QCOMPARE(decide(BuildKind::Gui, env(false, false, true), {"--gui"}).action, Action::Gui);
    }
    void windowsGuiExe_tui_pointsAtConsoleExe() {
        const Decision d = decide(BuildKind::Gui, env(true, false, true), {"--tui"});
        QCOMPARE(d.exitCode, 2);
        QVERIFY(QString::fromStdString(d.message).contains("aether-serial-bridge.exe --tui"));
    }
    void headless_tui_forced() {
        QCOMPARE(decide(BuildKind::Headless, env(false, false, false), {"--tui"}).action,
                 Action::Tui);
    }
    void help_isExit0_andTakesNoArguments() {
        QCOMPARE(decide(BuildKind::Headless, env(false, false, false), {"--help"}).exitCode, 0);
        QCOMPARE(decide(BuildKind::Headless, env(false, false, false), {"--help", "x"}).exitCode, 2);
    }
    void status_isExit0_andTakesNoArguments() {
        const Decision ok = decide(BuildKind::Headless, env(false, false, false), {"--status"});
        QCOMPARE(ok.action, Action::Command);
        QCOMPARE(ok.parsed.command, asb::cli::Command::Status);
        QCOMPARE(ok.exitCode, 0);
        // A typo after --status is a usage error, like every other command.
        const Decision bad =
            decide(BuildKind::Headless, env(false, false, false), {"--status", "--bogus"});
        QCOMPARE(bad.exitCode, 2);
        QVERIFY2(QString::fromStdString(bad.message).contains("unknown option '--bogus'"), bad.message.c_str());
    }
    void unknownArgument_isExit2() {
        QCOMPARE(decide(BuildKind::Headless, env(true, false, false), {"--bogus"}).exitCode, 2);
        QCOMPARE(decide(BuildKind::Headless, env(true, false, false), {"install"}).exitCode, 2);
    }
    // WS-9: the commands run through cli::runCommand, --helper too (WS-9b: Linux, the console executable only).
    void commands_areRunByRunCommand() {
        for (const char* c : {"--install", "--apply", "--start", "--stop", "--uninstall", "--repair", "--show-config",
                              "--list-ports", "--discard-pending"}) {
            const Decision d = decide(BuildKind::Headless, env(true, false, false), {c});
            QCOMPARE(d.action, Action::Command);
            QCOMPARE(d.exitCode, 0);
        }
        const Decision helper = decide(BuildKind::Headless, env(false, false, false), {"--helper"});
        QCOMPARE(helper.action, Action::Command);
        QCOMPARE(helper.parsed.command, asb::cli::Command::Helper);
        QCOMPARE(decide(BuildKind::Headless, env(false, false, true), {"--helper"}).exitCode, 2);   // Windows
        const Decision gui = decide(BuildKind::Gui, env(false, false, false), {"--helper"});   // not the GUI's
        QCOMPARE(gui.action, Action::UsageError);
        QCOMPARE(gui.exitCode, 2);
        QVERIFY(gui.message.find("aether-serial-bridge --helper") != std::string::npos);
    }
    // The GUI executable shows the managed state only; every other command is the console executable's.
    void guiBuild_readOnlyCommandsOnly() {
        QCOMPARE(decide(BuildKind::Gui, env(false, false, false), {"--status"}).action, Action::Command);
        QCOMPARE(decide(BuildKind::Gui, env(false, false, false), {"--show-config"}).action, Action::Command);
        for (const char* c : {"--install", "--stop", "--run", "--list-ports"}) {
            const Decision d = decide(BuildKind::Gui, env(false, false, false), {c, "--config", "x.json"});
            QCOMPARE(d.exitCode, 2);
        }
        const Decision d = decide(BuildKind::Gui, env(false, false, true), {"--install"});
        QCOMPARE(d.exitCode, 2);
        QVERIFY2(QString::fromStdString(d.message).contains("aether-serial-bridge.exe --install"), d.message.c_str());
        QCOMPARE(decide(BuildKind::Gui, env(false, false, false), {"--status", "--config", "x.json"}).exitCode, 2);
    }
    // --list-ports: the WS-8 port listing on Linux (INV-32), QSerialPortInfo on Windows (WS-9).
    void listPorts_bothOses() {
        const Decision d = decide(BuildKind::Headless, env(false, false, false), {"--list-ports"});
        QCOMPARE(d.action, Action::Command);
        QCOMPARE(d.exitCode, 0);
        QCOMPARE(decide(BuildKind::Headless, env(false, false, false), {"--list-ports", "x"}).exitCode, 2);
        DispatchEnv w = env(false, false, false);
        w.windows = true;
        QCOMPARE(decide(BuildKind::Headless, w, {"--list-ports"}).action, Action::Command);
    }
    // --service is the service host on both OSes (Linux: WS-8, main_headless runs it; Windows: WS-7), which refuses
    // by itself when the service manager did not start it (INV-90: tst_ServiceHost::serviceRequiresServiceManager).
    void service_isTheLinuxServiceHost() {
        const Decision d = decide(BuildKind::Headless, env(false, false, false), {"--service"});
        QCOMPARE(d.action, Action::Service);
        QCOMPARE(d.exitCode, 0);
        // The GUI executable never runs the service; extra arguments are a usage error.
        QCOMPARE(decide(BuildKind::Gui, env(false, false, true), {"--service"}).exitCode, 2);
        QCOMPARE(decide(BuildKind::Headless, env(false, false, true), {"--service", "x"}).action, Action::UsageError);
    }
    // WS-7: on Windows the headless executable's --service is the service host (it exits 2 itself when the
    // service manager did not start it).
    void service_isTheWindowsServiceHost() {
        const Decision d = decide(BuildKind::Headless, env(false, false, true), {"--service"});
        QCOMPARE(d.action, Action::Service);
        QCOMPARE(d.exitCode, 0);
    }

    // --- the real executables -----------------------------------------------------------------
    void headlessExe_help_status_version() {
        const QString exe = qEnvironmentVariable("ASB_HEADLESS_EXE");
        QVERIFY(!exe.isEmpty());
        const QProcessEnvironment pe = QProcessEnvironment::systemEnvironment();
        const Result h = run(exe, {"--help"}, pe);
        QCOMPARE(h.status, QProcess::NormalExit);
        QCOMPARE(h.exitCode, 0);
        QVERIFY2(h.out.contains("--install"), qPrintable(h.out));
        const Result s = run(exe, {"--status"}, pe);
        QCOMPARE(s.exitCode, 0);
        QVERIFY2(s.out.contains("Service:"), qPrintable(s.out + s.err));
        const Result v = run(exe, {"--version"}, pe);
        QCOMPARE(v.exitCode, 0);
        QVERIFY2(v.out.contains("aether-serial-bridge "), qPrintable(v.out));
    }
    void headlessExe_exitCodes() {
        const QString exe = qEnvironmentVariable("ASB_HEADLESS_EXE");
        const QProcessEnvironment pe = QProcessEnvironment::systemEnvironment();
        QCOMPARE(run(exe, {"--gui"}, pe).exitCode, 2);
        QCOMPARE(run(exe, {"--bogus"}, pe).exitCode, 2);
        QCOMPARE(run(exe, {"--install", "--config", "x.json"}, pe).exitCode, 2);
        QCOMPARE(run(exe, {"--install", "--apply"}, pe).exitCode, 2);
        // Outside the service manager --service refuses (exit 2), before it reads anything.
        QCOMPARE(run(exe, {"--service"}, pe).exitCode, 2);
        // No terminal here (QProcess pipes), so no arguments means help as a usage failure.
        QCOMPARE(run(exe, {}, pe).exitCode, 2);
    }
    // --tui without a terminal (QProcess pipes): the TUI prints the help, exit 2 (section 6.3). The TUI on a
    // terminal is tst_TuiProcess's.
    void headlessExe_tuiWithoutTerminalGetsHelp() {
        const QString exe = qEnvironmentVariable("ASB_HEADLESS_EXE");
        const Result r = run(exe, {"--tui"}, QProcessEnvironment::systemEnvironment());
        QCOMPARE(r.exitCode, 2);
        QVERIFY2(r.err.contains("Exit codes:"), qPrintable(r.err));
    }
    // QApplication exists only on the GUI path: with the offscreen platform, --gui constructs one,
    // runs the loop and exits 0.
    void guiExe_guiPathConstructsQApplication() {
        const QString exe = qEnvironmentVariable("ASB_GUI_EXE");
        if (exe.isEmpty()) QSKIP("GUI executable not part of this configuration");
        QProcessEnvironment pe = QProcessEnvironment::systemEnvironment();
        pe.insert("QT_QPA_PLATFORM", "offscreen");
        pe.insert("DISPLAY", ":99");  // satisfies the display rule; offscreen ignores it
        pe.insert("ASB_TEST_GUI_QUIT_MS", "500");   // the window (WS-10) quits its loop after 0.5 s (test builds)
        const Result r = run(exe, {"--gui"}, pe);
        QCOMPARE(r.status, QProcess::NormalExit);
        QVERIFY2(r.exitCode == 0, qPrintable(r.err));
    }
    void guiExe_nonGuiPathsNeverNeedADisplay() {
        const QString exe = qEnvironmentVariable("ASB_GUI_EXE");
        if (exe.isEmpty()) QSKIP("GUI executable not part of this configuration");
        QProcessEnvironment pe = QProcessEnvironment::systemEnvironment();
        pe.remove("DISPLAY");
        pe.remove("WAYLAND_DISPLAY");
        pe.insert("QT_QPA_PLATFORM", "xcb");  // would abort if a QApplication were constructed
        QCOMPARE(run(exe, {"--help"}, pe).exitCode, 0);
        QCOMPARE(run(exe, {"--status"}, pe).exitCode, 0);
        QCOMPARE(run(exe, {"--version"}, pe).exitCode, 0);
    }
    // v1.1 WS-17 §5 (owner debt): the Windows GUI executable is a WIN32 program with no console of its own, so its
    // refusals, --help and --version went nowhere. Now they reach its parent's console or, failing that, a message box
    // (recorded here through ASB_TEST_MESSAGEBOX=record:<file>); the exit codes are unchanged.
    void guiExeRefusalShownWithoutConsole() {
#ifndef _WIN32
        QSKIP("Windows only: the Linux GUI executable prints to its standard streams");
#else
        const QString exe = qEnvironmentVariable("ASB_GUI_EXE");
        if (exe.isEmpty()) QSKIP("GUI executable not part of this configuration");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString box = dir.filePath(QStringLiteral("box.txt"));
        QProcessEnvironment pe = QProcessEnvironment::systemEnvironment();
        pe.insert("ASB_TEST_MESSAGEBOX", QStringLiteral("record:") + box);
        auto shown = [&] {
            QFile f(box);
            const QString text = f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
            f.close();
            QFile::remove(box);
            return text;
        };
        struct Row {
            QStringList args;
            int code;
            const char* kind;
            const char* says;
        };
        const Row rows[] = {
            {{"--tui"}, 2, "box error", "aether-serial-bridge.exe --tui in a terminal"},
            {{"--install"}, 2, "box error", "run this with the console executable"},
            {{"--no-such-option"}, 2, "box error", "aether-serial-bridge-gui: error:"},
            {{"--version"}, 0, "box info", "aether-serial-bridge-gui "},
            {{"--help"}, 0, "box info", "This executable shows --status and --show-config"},
        };
        for (const Row& row : rows) {
            const Result r = run(exe, row.args, pe);
            QCOMPARE(r.status, QProcess::NormalExit);
            QCOMPARE(r.exitCode, row.code);
            const QString text = shown();
            QVERIFY2(text.startsWith(QLatin1String(row.kind)), qPrintable(row.args.join(' ') + ": " + text));
            QVERIFY2(text.contains(QLatin1String(row.says)), qPrintable(row.args.join(' ') + ": " + text));
            QVERIFY2(r.err.isEmpty() && r.out.isEmpty(), qPrintable(r.out + r.err));   // the box, not the stream
        }
#endif
    }
};

QTEST_GUILESS_MAIN(tst_Dispatch)
#include "tst_Dispatch.moc"
