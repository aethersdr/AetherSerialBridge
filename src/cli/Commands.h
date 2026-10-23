#pragma once

// Every command of both executables (master §4.10, WS-9): runCommand() takes the arguments and the host the
// executable supplies (the OS: paths, the process guard, elevation, the Applier's Platform, the scratch folder,
// and for the headless executable the engine), runs the command and returns its exit code:
//   0 ok; 1 failed; 2 usage (also a refused --config path, a conflict, a duplicate); 3 untrusted (the data folder,
//   or the scratch config's folder, failed its trust check: nothing was read or changed).
//
// Mode entry (S17, INV-112): a mutating command (one that runs the Applier) first enters the process guard, before
// the QCoreApplication, the Platform or any Applier object exists; only then is elevation checked. The Applier
// runs on an OperationHost worker; an interim notification is one line on stderr, and runCommand returns only
// after the operation's final result was delivered, with that result's code (D57, INV-118).
//
// The managed config is changed only through the Applier: --add-port / --remove-port / --set-enabled edit
// config.pending.json through Applier::edit; --config <scratch> edits that file only, after the scratch rules
// (ScratchRun.h). Read-only commands (--status, --show-config) never write (INV-44).

#include "cli/CommandLine.h"
#include "cli/ScratchRun.h"
#include "core/Interfaces.h"

#include <QString>
#include <QStringList>

#include <chrono>
#include <memory>
#include <optional>

namespace asb::cli {

// Where output goes. Lines carry no trailing newline.
class Console {
public:
    virtual ~Console() = default;
    virtual void out(const QString& line) = 0;
    virtual void err(const QString& line) = 0;
};
// stdout / stderr, flushed per line.
Console& standardConsole();

// A folder opened for a scratch config: its IDataFolder (Scratch trust: the invoking user is a trusted owner) and
// the FileIo that writes beside the scratch file with the invoking user's ownership.
struct ScratchFolder {
    std::unique_ptr<IDataFolder> folder;
    std::unique_ptr<FileIo> io;
};

class Host {
public:
    virtual ~Host() = default;

    virtual bool windows() const = 0;
    // The managed locations, for messages and for the scratch rules. Empty when they cannot be determined
    // (Windows: kWindowsDataFolderError); the commands then refuse with exit 3.
    virtual QString managedConfigFile() const = 0;
    virtual QString dataFolder() const = 0;

    // Mode entry: installs the process-creation guard (Windows job, Linux seccomp). Does not return on failure
    // (the process exits 5). Must be the first thing a mutating command and the diagnostic --run do.
    virtual void enterGuardedMode() = 0;
    // v1.1 WS-17 (D65, INV-130): a standard user's Windows window: a job of its own (limit 2, kill-on-job-close)
    // instead of the v1.0 guard, so Restart as administrator's `runas` relaunch is not blocked. It builds no Applier
    // object. No-op elsewhere.
    virtual void enterStandardUserJob() {}
    // Test builds only: right after the guard's call site and before the first Applier object, try to create a
    // process and say on stderr whether that worked (guard_installed_before_first_object). No-op otherwise.
    virtual void guardProbe() {}
    // Before a foreground run's QCoreApplication: blocks the stop signals (SIGINT, SIGTERM, SIGHUP) in this thread, so
    // every later thread inherits the mask and only the run's signalfd receives them (POSIX; a no-op elsewhere).
    virtual void blockStopSignals() {}
    virtual bool elevated() = 0;

    // The Applier's Platform, built on first use (after the guard, after the QCoreApplication). nullptr with *error
    // and *code (1, or 3 when the data folder cannot be determined) when it cannot be built.
    virtual Platform* platform(QString* error, int* code) = 0;
    // What the adapters reported during an operation that Result does not carry: lock files secured or replaced,
    // the service's files written or removed. Folded into the printed result; taken (cleared) by the call.
    virtual QStringList takeReports() { return {}; }
    // Who holds engine.lock, when an operation could not take it (R17); empty when unknown.
    virtual QString engineLockHolder() { return {}; }
    virtual IClock& clock() { return systemClock(); }
    // This build, as a question names it before replacing a different installed build: the version, and on Windows
    // the digest of the program it installs ("0.1.0, program sha256 0123456789ab").
    virtual QString thisBuild();
    virtual std::chrono::milliseconds interimPoll() { return std::chrono::milliseconds(250); }
    // Test builds: called with every final Result and the reports taken with it (the systemd suite reads them as
    // JSON). No-op otherwise.
    virtual void resultHook(const Result&, const QStringList& /*reports*/) {}

    // A scratch config's folder (absolute path of the directory).
    virtual ScratchFolder scratchFolder(const QString& directory) = 0;
    virtual std::optional<ScratchRun::FileKey> fileKey(const QString& path) { return ScratchRun::fileKey(path); }
    virtual QString ownExecutable() = 0;
    virtual QString installedExecutable() = 0;

    // The diagnostic run of a validated scratch config whose listeners are already loopback-only; returns the exit
    // code when it ends (Ctrl+C). Only the headless executable has an engine; the GUI's host refuses.
    virtual int runScratch(const QString& configFile, const BridgeConfig& loopbackConfig, Console&) = 0;
    // --service: never returns in production.
    virtual int runService() = 0;
    // --list-ports: names only; never opens a device (INV-32).
    virtual int listPorts(Console&) = 0;
    // Asks a yes/no question on the terminal; nullopt when there is no terminal to ask on.
    virtual std::optional<bool> confirm(const QString& question) = 0;

    // ---- --uninstall-from-apps (Windows; WS-16, D63, D64) ---------------------------------------------------------
    // Settings starts the entry's UninstallString; its answers are message boxes, not a console.
    enum class Box { Question, Information, Error };
    // A message box owned by no window, in front (MB_SETFOREGROUND). Question: Yes/No with No the default; returns true
    // for Yes. Test builds: ASB_TEST_MESSAGEBOX=yes|no answers it (and the other kinds go to stderr).
    virtual bool messageBox(const QString& /*text*/, Box) { return false; }
    // Leaves the console Windows may have created for this process (FreeConsole), so only the message box shows.
    virtual void detachConsole() {}
    // Starts this executable's own image elevated with exactly `arguments` and waits for it (SelfElevate, D64).
    struct Relaunch {
        bool started = false;
        bool cancelled = false;   // UAC answered No: nothing ran
        int exitCode = 0;
        QString error;
    };
    virtual Relaunch relaunchElevated(const QStringList& /*arguments*/) { return {}; }
};

// Parses `args` and runs the command. Usage errors print "error: ..." and a pointer to --help, and return 2.
int runCommand(const QStringList& args, Host&, Console& = standardConsole());
// The same for arguments parse() already accepted (the executables' dispatch parses first).
int runCommand(const Parsed&, const QStringList& args, Host&, Console& = standardConsole());

// The usage text (--help).
QString usageText(bool guiExecutable, bool windows);

}  // namespace asb::cli
