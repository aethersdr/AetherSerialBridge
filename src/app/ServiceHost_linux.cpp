// The Linux `--service` host (WS-8; D37, D41, D59; INV-1, 22, 100, 103, 110, 113). The only process that runs
// the managed config.json, started by systemd as the aether-serial-bridge unit's DynamicUser.
//
// Order, each step before the next one may happen:
//   1. the process guard (seccomp, INV-110): before any object and before Qt starts a thread; failure exits 5;
//   2. serviceLocations() (WS-5 hand-off; it cannot fail on Linux, and is checked anyway);
//   3. the activation identity (INV-100): /proc/self/cgroup must be exactly the managed unit, or exit 2 —
//      before the data folder, the lock or the engine is touched;
//   4. SIGTERM/SIGINT/SIGHUP blocked (every later thread inherits the mask) and read through a signalfd;
//   5. the two deadline watchdogs (D59, WS-4): the firewall watchdog (Linux: never armed, NoFirewallMonitor)
//      and the kill deadline;
//   6. QCoreApplication; the side channel (journal on stdout, status.json in /run/aether-serial-bridge with
//      the Runtime policy) after this process's own stale status temps are removed;
//   7. the engine with the serial device factory: registered only here and in --run --config (INV-1);
//   8. BridgeRunner::start(): engine.lock + its id (exit 4), Service-mode trust (exit 3), start.token
//      (exit 6), config.json; every refusal ends through Exit::now(kill deadline, code, no report).
// An orderly stop (SIGTERM from systemd) arms the kill deadline at STOP + 20 s first, then stops the engine,
// publishes the last status, shuts the side channel down for at most 2 s and ends through Exit::now —
// whether or not that shutdown finished. engine.lock is released only by the process ending (INV-103).

#include "app/ServiceHost.h"

#include "core/AtomicJson.h"
#include "core/BridgeRunner.h"
#include "core/DeadlineWatchdog.h"
#include "core/Exit.h"
#include "core/LogSinks.h"
#include "core/SideChannel.h"
#include "core/StartToken.h"
#include "core/StatusStore.h"
#include "engine/BridgeHost.h"
#include "engine/QtSerialDevice.h"
#include "platform/posix/LinuxLocks.h"
#include "platform/posix/LinuxPlatform.h"
#include "platform/posix/PosixDataFolder.h"
#include "platform/posix/PosixFileIo.h"
#include "platform/posix/ProcessGuard.h"
#include "platform/posix/ServiceIdentity.h"

#include <QCoreApplication>
#include <QSocketNotifier>

#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace asb::app {
namespace {

using namespace std::chrono_literals;

constexpr std::chrono::seconds kStopKillDeadline{20};   // D59: STOP + 20 s
constexpr int kKillCode = 1;                             // a stop that hangs ends as a failure (S4)

void say(const char* text)
{
    const ssize_t w = ::write(STDERR_FILENO, text, std::strlen(text));
    (void)w;
}

// Our own leftovers in the runtime directory: status.json temps of an engine killed mid-write (WS-5 hand-off:
// P5 leaves them to the service). Only `status.json.<hex>.asbtmp` regular files are removed.
void sweepStatusTemps(asb::posix::PosixFileIo& io, const asb::posix::PosixDirHandle& dir)
{
    if (!dir.valid()) return;
    const std::optional<QStringList> names = io.sys().listDirectory(dir.fd());
    if (!names) return;
    const QString prefix = QString::fromLatin1(StatusStore::kFileName) + QLatin1Char('.');
    for (const QString& n : *names) {
        if (!n.startsWith(prefix) || !isTempName(n)) continue;
        QString err;
        io.remove(dir, n, &err);
    }
}

#ifdef ASB_TEST_SEAMS
// Test builds only: hold the process between the orderly stop and Exit::now (lock_held_until_process_exit).
void testPauseBeforeExit()
{
    const QByteArray v = qgetenv("ASB_TEST_PAUSE_BEFORE_EXIT_MS");
    if (!v.isEmpty()) std::this_thread::sleep_for(std::chrono::milliseconds(v.toLongLong()));
}
#endif

}  // namespace

int runLinuxService(int argc, char** argv)
{
    // 1. No process creation from here on, in any thread (exit 5 on failure).
    asb::posix::ProcessGuard::installOrExit();

    // 2. The locations (cannot fail on Linux; the check stays so every host follows the same rule).
    const ServiceLocations loc = serviceLocations();
    if (!loc.locations) {
        say("aether-serial-bridge: cannot determine the data folder; refusing to start\n");
        ::_exit(exitCode(loc.failure));
    }

    // 3. Activation identity: the exact cgroup of the managed unit, before anything else is read.
    {
        QString why;
        if (!asb::posix::ServiceIdentity::check(&why)) {
            const QByteArray line = QByteArrayLiteral("aether-serial-bridge: ") + asb::posix::ServiceIdentity::kRefusal +
                                    QByteArrayLiteral(" (") + why.toLocal8Bit() + QByteArrayLiteral(")\n");
            say(line.constData());
            // _exit, like every other end of this process: no atexit work runs under the process guard.
            ::_exit(asb::posix::ServiceIdentity::kExitRefused);
        }
    }

    // 4. Signals: blocked in this thread before any other thread exists, so every thread inherits the mask and
    //    the signalfd is the only receiver.
    sigset_t stopSignals;
    sigemptyset(&stopSignals);
    sigaddset(&stopSignals, SIGTERM);
    sigaddset(&stopSignals, SIGINT);
    sigaddset(&stopSignals, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &stopSignals, nullptr);
    const int sigfd = ::signalfd(-1, &stopSignals, SFD_CLOEXEC | SFD_NONBLOCK);

    // 5. The two watchdogs (D59), at process start; never destroyed (the process ends first).
    auto* firewallWatchdog = new DeadlineWatchdog();
    auto* killDeadline = new DeadlineWatchdog();
    if (sigfd < 0) {
        say("aether-serial-bridge: signalfd failed; refusing to start\n");
        Exit::now(*killDeadline, kKillCode, nullptr);
    }

    // 6. Qt, then the side channel. Production never destroys any of these objects.
    auto* app = new QCoreApplication(argc, argv);
    const asb::posix::LinuxPaths paths = asb::posix::LinuxPaths::from(*loc.locations);
    auto statusIo = std::make_shared<asb::posix::PosixFileIo>(asb::posix::realPosixSys(), asb::posix::PosixOwner::current(), 0644);
    auto runtimeDir = std::make_shared<asb::posix::PosixDirHandle>();
    {
        const QByteArray rd = paths.runtimeDirectory.toLocal8Bit();
        runtimeDir->reset(asb::posix::realPosixSys(), ::open(rd.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    }
    sweepStatusTemps(*statusIo, *runtimeDir);
    auto* side = new SideChannel(std::make_unique<JournalLogSink>(stdout),
                                 runtimeDir->valid() ? statusFileWriter(statusIo, runtimeDir) : StatusWriter{});
    if (!runtimeDir->valid())
        side->log(QtWarningMsg, QStringLiteral("%1 is not available; no status will be published").arg(paths.runtimeDirectory));

    // 7. The engine: the only place (with --run --config) the serial device factory is registered (INV-1).
    auto* engine = new engine::BridgeHost(&engine::makeQtSerialDevice, *side);

    // 8. The runner.
    auto* lock = new asb::posix::EngineLock(asb::posix::EngineLock::Options{paths.lockDirectory});
    auto* folder = new asb::posix::PosixDataFolder(paths.dataFolder);
    auto* folderIo = new asb::posix::PosixFileIo();
    auto* token = new FileStartToken(*folder, *folderIo);
    auto* monitor = new NoFirewallMonitor();
    auto* runner = new BridgeRunner(*lock, *token, *folder, *engine, *side, *monitor, *firewallWatchdog,
                                    StatusStore::newInstanceId());
    const RunnerStart started = runner->start();
    const int code = exitCode(started);
    switch (started) {
    case RunnerStart::Ok:
        side->log(QtInfoMsg, QStringLiteral("started: %1 port(s) listening").arg(engine->bridges().size()));
        break;
    case RunnerStart::ConfigError:
        side->log(QtCriticalMsg, QStringLiteral("config.json is not valid; no ports are bridged: %1").arg(runner->report().configError.value_or(QString())));
        break;
    case RunnerStart::EngineLockBusy:
        side->log(QtCriticalMsg, QStringLiteral("exit 4: the engine lock: %1").arg(lock->lastError()));
        break;
    case RunnerStart::Untrusted:
        side->log(QtCriticalMsg, QStringLiteral("exit 3: the data folder %1 is not trusted; nothing was read or written").arg(paths.dataFolder));
        break;
    case RunnerStart::NotActivated:
        // Not a failure (v1.1 station fix): the unit counts exit 6 as clean (SuccessExitStatus=6), so
        // Restart=on-failure does not restart a stopped bridge every 5 s.
        side->log(QtWarningMsg, QStringLiteral("exit 6: %1").arg(notActivatedLine(runner->tokenRefusal())));
        break;
    default:
        side->log(QtCriticalMsg, QStringLiteral("exit %1").arg(code));
        break;
    }
    if (code != 0) {
        // The reason reaches the journal within a bounded budget; the exit does not wait for anything else.
        side->flush(500ms);
        Exit::now(*killDeadline, code, nullptr);
    }

    // Orderly stop on SIGTERM / SIGINT / SIGHUP.
    auto* notifier = new QSocketNotifier(sigfd, QSocketNotifier::Read);
    QObject::connect(notifier, &QSocketNotifier::activated, app, [=] {
        signalfd_siginfo si{};
        while (::read(sigfd, &si, sizeof si) > 0) {
        }
        notifier->setEnabled(false);
        // D59: the kill deadline is armed before anything that could block.
        killDeadline->arm(std::chrono::steady_clock::now() + kStopKillDeadline, kKillCode);
        side->log(QtInfoMsg, QStringLiteral("stopping"));
        runner->stop([=] {
            const bool flushed = side->shutdown(SideChannel::kShutdownBudget);
            (void)flushed;   // a blocked worker is abandoned; the process ends either way (WS-4)
#ifdef ASB_TEST_SEAMS
            testPauseBeforeExit();
#endif
            Exit::now(*killDeadline, 0, nullptr);
        });
    });
    return app->exec();
}

}  // namespace asb::app
