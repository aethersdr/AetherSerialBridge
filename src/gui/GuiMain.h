#pragma once

// The GUI path of the GUI executable (master §5.1, WS-10): main_gui decides (identity, dispatch, the display check)
// and calls one of these.
//
// runGui: mode entry (Windows: the process-creation guard, INV-112, before any Platform or Applier object; the
// process COM security on a helper thread, so the GUI thread stays Qt's single-threaded apartment) ->
// QApplication -> Qt library policy -> Fusion -> Inter -> ThemeManager::instance() -> ThemeFollower::start() ->
// application sheet -> the window -> exec.
//
// runSelfTestInit (hidden --self-test-init, WS-13's release self-test, INV-88): the same initialisation with an
// empty view (no data folder is read) and Qt's test-mode locations (gui.ini and the image caches go to a test
// folder), shows the window, waits for it to be exposed and painted, and prints one JSON line
// {"exposed":bool,"painted":bool,"modules":[path...]} on stdout. Exit 0 when exposed and painted, else 1.

#include "cli/Commands.h"
#include "platform/win/ProcessPolicy.h"

#include <QByteArray>
#include <QString>

#include <functional>

namespace asb::gui {

struct GuiHosts {
    // The window's read-only Platform (status, config, service state).
    cli::Host* view = nullptr;
    // The Applier's Platform (the elevated Windows GUI runs the Applier in-process); nullptr where this process
    // never runs it (Linux: the pkexec helper does).
    cli::Host* apply = nullptr;
    // Test builds: called right after the Windows guard's call site, before any Platform or Applier object
    // (guard_installed_before_first_object_gui, INV-112).
    void (*guardProbe)() = nullptr;
    // This build as the window names it before replacing a different installed build: "0.9.0, program sha256
    // 0123456789ab" (both OSes since v1.1: the digest of the program it installs; on Linux the program its AppImage
    // embeds, D61).
    QString thisBuild = QString();
    // Linux (v1.1 WS-15, D60): the window's own program for the staged install, update and uninstall: the digest
    // compiled into the window, and a reader (WS-14's windowProgram(), wired by main_gui.cpp) that returns its path and
    // bytes, or why it cannot be used. The reader runs on a worker thread. Unused on Windows.
    QByteArray programSha256 = QByteArray();
    std::function<void(QString* path, QByteArray* bytes, QString* problem)> readProgram = nullptr;
    // Linux (WS-17): why the window's own program cannot be used (WS-14's windowProgram().problem), empty when it can.
    // Runs on a worker thread when the window opens, so Install, Update and Uninstall are greyed out before a click.
    std::function<QString()> probeProgram = nullptr;
    // Windows (WS-17 §3, D64): Restart as administrator for a standard user's window: SelfElevate::relaunch of this
    // program with "--gui --intent=<intent>", without waiting.
    struct Relaunch {
        bool started = false;
        bool cancelled = false;
        QString error;
    };
    std::function<Relaunch(const QString& intent)> restartAsAdmin = nullptr;
    // The elevated window's --intent (install | update | uninstall | none), from the command line; ignored by a standard
    // user's window.
    QString intent = QString();
};

int runGui(int argc, char** argv, const GuiHosts& hosts, win::PluginPolicy plugins);
int runSelfTestInit(int argc, char** argv, win::PluginPolicy plugins);

}  // namespace asb::gui
