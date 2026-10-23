#include "app/ScratchEngine.h"

#include "core/ConfigJson.h"
#include "core/LogSinks.h"
#include "core/SideChannel.h"
#include "engine/BridgeHost.h"
#include "engine/QtSerialDevice.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QTimer>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <QSocketNotifier>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>
#endif

#include <atomic>

namespace asb::app {

ScratchEngine::ScratchEngine(ISideChannel& side)
    // The serial device factory: registered only by --service and here (INV-1).
    : m_side(side), m_engine(std::make_unique<engine::BridgeHost>(&engine::makeQtSerialDevice, side)),
      m_instanceId(StatusStore::newInstanceId())
{
}

ScratchEngine::~ScratchEngine() = default;

void ScratchEngine::start(const BridgeConfig& config)
{
    m_startedAt = QDateTime::currentDateTimeUtc();
    m_configSha256 = sha256Hex(toJson(config));
    m_engine->start(config);
}

void ScratchEngine::stop(std::function<void()> done)
{
    m_engine->stop(std::move(done));
}

QList<ScratchEngine::Endpoint> ScratchEngine::endpoints() const
{
    QList<Endpoint> out;
    for (const auto& b : m_engine->bridges()) {
        Endpoint e;
        e.name = b->mapping().displayName();
        e.device = b->mapping().comPort;
        e.listening = b->server().isListening();
        e.address = b->server().serverAddress();
        e.port = b->server().serverPort();
        e.error = b->snapshot().lastError.value_or(QString());
        out << e;
    }
    return out;
}

StatusReport ScratchEngine::report() const
{
    StatusReport r;
    r.updated = QDateTime::currentDateTimeUtc();
    r.startedAt = m_startedAt;
    r.pid = QCoreApplication::applicationPid();
    r.instanceId = m_instanceId;
    r.configSha256 = m_configSha256;
    r.ports = m_engine->snapshot();
    return r;
}

namespace {

#ifdef _WIN32
std::atomic<bool> g_stopRequested{false};
BOOL WINAPI consoleControl(DWORD)
{
    if (!g_stopRequested.exchange(true)) {
        if (QCoreApplication* app = QCoreApplication::instance())
            QMetaObject::invokeMethod(app, &QCoreApplication::quit, Qt::QueuedConnection);
    }
    return TRUE;   // handled: the process ends through the orderly stop below
}
#endif

}  // namespace

int runScratch(cli::Host& host, const QString& configFile, const BridgeConfig& config, cli::Console& c)
{
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) {
        c.err(QStringLiteral("error: no application object."));
        return 1;
    }
#ifndef _WIN32
    // The stop signals were blocked before the QCoreApplication (Host::blockStopSignals at the run's mode entry), so
    // every thread has them masked; blocking them again here is harmless. They are read through a signalfd on the
    // event loop.
    sigset_t stopSignals;
    sigemptyset(&stopSignals);
    sigaddset(&stopSignals, SIGTERM);
    sigaddset(&stopSignals, SIGINT);
    sigaddset(&stopSignals, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &stopSignals, nullptr);
    const int sigfd = ::signalfd(-1, &stopSignals, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sigfd < 0) {
        c.err(QStringLiteral("error: cannot watch for Ctrl+C (signalfd failed)."));
        return 1;
    }
    QSocketNotifier notifier(sigfd, QSocketNotifier::Read);
    QObject::connect(&notifier, &QSocketNotifier::activated, app, [&] {
        signalfd_siginfo si{};
        while (::read(sigfd, &si, sizeof si) > 0) {
        }
        notifier.setEnabled(false);
        QCoreApplication::quit();
    });
#else
    SetConsoleCtrlHandler(&consoleControl, TRUE);
#endif

    // The status beside the scratch config, through the folder the CLI judged (Scratch trust), when it is trusted.
    const QString dir = QFileInfo(configFile).absolutePath();
    auto scratch = std::make_shared<cli::ScratchFolder>(host.scratchFolder(dir));
    StatusWriter writer;
    if (scratch->folder->inspect(TrustMode::Scratch).action == DirAction::Trust) {
        writer = statusFileWriter(std::shared_ptr<FileIo>(scratch, scratch->io.get()),
                                  std::shared_ptr<const DirHandle>(scratch, &scratch->folder->handle()));
    }
    // Never destroyed before the process ends: an abandoned side-channel worker keeps what it uses.
    auto* side = new SideChannel(std::make_unique<ConsoleLogSink>(stderr), writer);
    ScratchEngine engine(*side);
    engine.start(config);

    c.out(QStringLiteral("Running %1 in this terminal (diagnostic, not the service); Ctrl+C stops it.")
              .arg(QDir::toNativeSeparators(configFile)));
    for (const ScratchEngine::Endpoint& e : engine.endpoints()) {
        if (e.listening) {
            const QString where = e.address.protocol() == QAbstractSocket::IPv6Protocol
                                      ? QStringLiteral("[%1]").arg(e.address.toString())
                                      : e.address.toString();
            c.out(QStringLiteral("  %1: %2 on %3:%4").arg(e.name, e.device, where).arg(e.port));
        } else {
            c.err(QStringLiteral("  %1: %2 is not listening: %3").arg(e.name, e.device, e.error));
        }
    }
    if (engine.endpoints().isEmpty()) c.err(QStringLiteral("Note: the config has no enabled ports; nothing listens."));
    if (writer) c.out(QStringLiteral("Status: %1 (--status --config %2)")
                          .arg(QDir::toNativeSeparators(QDir(dir).filePath(QString::fromLatin1(StatusStore::kFileName))),
                               QDir::toNativeSeparators(configFile)));

    QTimer status;
    QObject::connect(&status, &QTimer::timeout, app, [&] { side->publishStatus(engine.report()); });
    side->publishStatus(engine.report());
    status.start(1000);
    app->exec();
    status.stop();

    c.out(QStringLiteral("Stopping..."));
    bool stopped = false;
    QEventLoop loop;
    engine.stop([&] {
        stopped = true;
        loop.quit();
    });
    if (!stopped) loop.exec();
    side->publishStatus(engine.report());
    side->shutdown(SideChannel::kShutdownBudget);
    return 0;
}

}  // namespace asb::app
