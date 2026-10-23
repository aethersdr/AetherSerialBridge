#include "tui/Tui.h"

#include "tui/TuiApp.h"
#include "tui/TuiView.h"

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/screen/terminal.hpp>

#include <QByteArray>
#include <QCoreApplication>
#include <QTimer>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <QSocketNotifier>
#include <langinfo.h>
#include <signal.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/signalfd.h>
#endif
#endif

#include <chrono>
#include <cstring>
#include <thread>

namespace asb::tui {

namespace {

void ensureApplication()
{
    if (QCoreApplication::instance()) return;
    static int argc = 1;
    static char name[] = "aether-serial-bridge";
    static char* argv[] = {name, nullptr};
    new QCoreApplication(argc, argv);
}

#ifdef _WIN32
// The console's control events, read by the loop's timer. Ctrl+C and Ctrl+Break are keys like q (refused while an
// operation runs); closing the console is a hangup. Windows ends the process a few seconds after the close event
// whatever the handler does, so the handler only holds it while an operation is still outstanding (R18: if it is
// killed anyway, the operation is an ordinary interrupted transaction).
std::atomic<bool> g_interrupt{false};
std::atomic<bool> g_hangup{false};
std::atomic<bool> g_busy{false};

BOOL WINAPI consoleControl(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT: g_interrupt = true; return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT: {
        g_hangup = true;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(4500);
        while (g_busy.load() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return TRUE;
    }
    default: break;
    }
    return FALSE;
}
#endif

bool utf8Terminal()
{
#ifdef _WIN32
    // UTF-8 output first; when the console refuses it, the logo and the glyphs fall back to ASCII.
    const bool out = SetConsoleOutputCP(CP_UTF8) != 0;
    SetConsoleCP(CP_UTF8);
    return out;
#else
    // QCoreApplication has applied the environment's locale (setlocale(LC_ALL, "")).
    const char* cs = nl_langinfo(CODESET);
    return cs && (std::strcmp(cs, "UTF-8") == 0 || std::strcmp(cs, "utf8") == 0);
#endif
}

int runLoop(cli::Host& host, Platform* platform, const QString& platformError, TuiApp::Env env)
{
    TuiApp app(host, platform, platformError, env);

    auto screen = ftxui::App::Fullscreen();
    // Ctrl+C is a key the TUI answers (refused while busy, asks when dirty); Ctrl+Z does not suspend.
    screen.ForceHandleCtrlC(false);
    screen.ForceHandleCtrlZ(false);
    ftxui::Component root = ftxui::Renderer([&app] {
        const ftxui::Dimensions d = ftxui::Terminal::Size();
        return app.render(d.dimx, d.dimy);
    });
    root = ftxui::CatchEvent(root, [&app](const ftxui::Event& e) { return app.onEvent(e); });

    // Installs the terminal: raw mode, the alternate screen, FTXUI's terminal queries.
    ftxui::Loop loop(&screen, root);
    app.setColorMode(currentColorMode());
    app.changed = [&screen] { screen.PostEvent(ftxui::Event::Custom); };

#ifdef _WIN32
    if (HANDLE in = GetStdHandle(STD_INPUT_HANDLE); in != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        // Ctrl+C as a key event, not a console signal (FTXUI restores the mode on exit).
        if (GetConsoleMode(in, &mode)) SetConsoleMode(in, mode & ~DWORD(ENABLE_PROCESSED_INPUT));
    }
    SetConsoleCtrlHandler(&consoleControl, TRUE);
#elif defined(__linux__)
    // The stop signals were blocked at mode entry (Host::blockStopSignals) in every thread; they arrive here.
    sigset_t stopSignals;
    sigemptyset(&stopSignals);
    sigaddset(&stopSignals, SIGHUP);
    sigaddset(&stopSignals, SIGINT);
    sigaddset(&stopSignals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stopSignals, nullptr);
    const int sigfd = ::signalfd(-1, &stopSignals, SFD_CLOEXEC | SFD_NONBLOCK);
    std::unique_ptr<QSocketNotifier> notifier;
    if (sigfd >= 0) {
        notifier = std::make_unique<QSocketNotifier>(sigfd, QSocketNotifier::Read);
        QObject::connect(notifier.get(), &QSocketNotifier::activated, [sigfd, &app] {
            signalfd_siginfo si{};
            while (::read(sigfd, &si, sizeof si) == ssize_t(sizeof si)) {
                if (si.ssi_signo == SIGHUP) app.hangup();
                else if (si.ssi_signo == SIGTERM) app.terminate();
                else if (si.ssi_signo == SIGINT) app.onEvent(ftxui::Event::CtrlC);
            }
        });
    }
#endif

    QTimer tick;
    tick.setInterval(16);
    QObject::connect(&tick, &QTimer::timeout, [&] {
#ifdef _WIN32
        g_busy = app.busy();
        if (g_interrupt.exchange(false)) app.onEvent(ftxui::Event::CtrlC);
        if (g_hangup.exchange(false)) app.hangup();
#endif
        if (app.quitRequested()) {
            if (!app.drawing()) {
                // The terminal is gone: do not draw the exit either.
                QCoreApplication::quit();
                return;
            }
            screen.Exit();
        }
        // Stop drawing once the terminal went away; the event loop keeps running until the operation's final result.
        if (app.drawing() || app.quitRequested()) loop.RunOnce();
        if (loop.HasQuitted()) QCoreApplication::quit();
    });
    tick.start();
    screen.PostEvent(ftxui::Event::Custom);   // the first frame
    QCoreApplication::exec();
    tick.stop();
#ifdef __linux__
    notifier.reset();
    if (sigfd >= 0) ::close(sigfd);
#endif
    return 0;
}

}  // namespace

bool terminalUsable()
{
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
#else
    if (::isatty(0) == 0 || ::isatty(1) == 0) return false;
    return qgetenv("TERM") != "dumb";
#endif
}

int run(cli::Host& host, const Options& opts)
{
    // Mode entry (INV-112): the guard before the QCoreApplication, the Platform or any Applier object.
    host.enterGuardedMode();
    host.guardProbe();
    host.blockStopSignals();
    if (!terminalUsable()) return opts.notATerminal ? opts.notATerminal() : 2;

    ensureApplication();
    TuiApp::Env env;
    env.windows = host.windows();
    env.utf8 = utf8Terminal();
    env.readOnlyExecutable = opts.readOnlyExecutable;
    env.elevated = !opts.readOnlyExecutable && host.elevated();
    // The update instructions name this program, not the installed one. An AppImage runs
    // from a mount that is gone when it exits: the file the user started is $APPIMAGE, trusted only when this process
    // runs from that mount (an inherited $APPIMAGE names some other AppImage).
    env.program = env.windows ? QString()
                              : appImageOf(qEnvironmentVariable("APPIMAGE"), qEnvironmentVariable("APPDIR"),
                                           QCoreApplication::applicationFilePath());
    env.appImage = !env.program.isEmpty();
    if (env.program.isEmpty()) env.program = QCoreApplication::applicationFilePath();
    env.headlessProgram = opts.headlessProgram;
    QString error;
    int code = 1;
    Platform* platform = host.platform(&error, &code);
    return runLoop(host, platform, error, env);
}

void exitProcess(int code)
{
    std::fflush(nullptr);
    std::_Exit(code);
}

}  // namespace asb::tui
