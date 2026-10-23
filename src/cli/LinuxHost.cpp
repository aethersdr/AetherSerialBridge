#include "cli/LinuxHost.h"

#include "cli/Elevation.h"
#include "cli/PolkitPolicy.h"
#include "core/BridgeRunner.h"
#include "platform/posix/PosixDataFolder.h"
#include "platform/posix/PosixFileIo.h"
#include "platform/posix/ProcessGuard.h"
#include "platform/posix/SerialPortList.h"
#include "platform/posix/SystemdUnit.h"

#include <QCryptographicHash>
#include <QFile>
#include <QTextStream>

#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>

namespace asb::cli {

LinuxHost::LinuxHost(int argc, char** argv, QByteArray programSha256)
    : m_argc(argc), m_argv(argv), m_programSha256(programSha256.toLower()), m_programHashed(!programSha256.isEmpty())
{
    // Cannot fail on Linux (the Windows-only failure is kWindowsDataFolderError); checked like every host.
    const ServiceLocations loc = serviceLocations();
    if (loc.locations) {
        m_configFile = loc.locations->configFile;
        m_dataFolder = loc.locations->dataDirectory();
    }
}

LinuxHost::~LinuxHost() = default;

void LinuxHost::enterGuardedMode()
{
    posix::ProcessGuard::installOrExit();   // exit 5 on failure, before anything else exists
}

void LinuxHost::blockStopSignals()
{
    sigset_t stop;
    sigemptyset(&stop);
    sigaddset(&stop, SIGTERM);
    sigaddset(&stop, SIGINT);
    sigaddset(&stop, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &stop, nullptr);
}

bool LinuxHost::elevated()
{
    return Elevation::isElevated();
}

Platform* LinuxHost::platform(QString* error, int* code)
{
    if (m_platform) return m_platform.get();
    const ServiceLocations loc = serviceLocations();
    if (!loc.locations) {
        if (error) *error = QStringLiteral("cannot determine the data folder");
        if (code) *code = 3;
        return nullptr;
    }
    posix::LinuxPlatform::Options o;
    o.paths = posix::LinuxPaths::from(*loc.locations);
    o.report = [this](const QString& line) {
        std::lock_guard<std::mutex> g(m_reportsMutex);
        m_reports << line;
    };
    // Build identity (v1.1 D61, INV-120): the installed program is this build's only when its SHA-256 is this one.
    o.service.expectedSha256 = programSha256();
    // Install copies the running program: the binary that is installed is the one that was run.
    o.service.sourceBinary = QStringLiteral("/proc/self/exe");
    // The pkexec helper's polkit action (WS-9b): written by install, removed only by uninstall (INV-51).
    o.service.policy = QByteArray(kPolkitPolicy);
    customise(o);
    m_lp = std::make_unique<posix::LinuxPlatform>(o);
    Platform& p = m_lp->platform();
    IFirewall* firewall = wrapFirewall(p.firewall);
    m_platform = std::make_unique<Platform>(Platform{p.folder, p.service, firewall ? *firewall : p.firewall, p.applyLock,
                                                     p.engineLock, p.token, p.status, p.io});
    return m_platform.get();
}

QByteArray ownImageSha256()
{
    QFile self(QStringLiteral("/proc/self/exe"));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (self.open(QIODevice::ReadOnly) && hash.addData(&self)) return hash.result().toHex();
    return {};
}

QByteArray LinuxHost::programSha256()
{
    if (!m_programHashed) {
        m_programHashed = true;
        m_programSha256 = ownImageSha256();
    }
    return m_programSha256;
}

QString LinuxHost::thisBuild()
{
    const QString version = Host::thisBuild();
    const QByteArray digest = programSha256();
    if (digest.isEmpty()) return version;
    return QStringLiteral("%1, program sha256 %2").arg(version, QString::fromLatin1(digest.left(12)));
}

QStringList LinuxHost::takeReports()
{
    QStringList r;
    {
        std::lock_guard<std::mutex> g(m_reportsMutex);
        r.swap(m_reports);
    }
    if (m_lp) r << m_lp->service().takeChanges();
    return r;
}

QString LinuxHost::engineLockHolder()
{
    return m_lp ? m_lp->engineLock().holderDescription() : QString();
}

ScratchFolder LinuxHost::scratchFolder(const QString& directory)
{
    // Scratch trust (master §4.4): root and the invoking user are the trusted owners, so the Config owner stays root
    // (the default) and the invoking user is added (a root-owned scratch config is trusted). What the CLI
    // writes beside the scratch config is the invoking user's own.
    const posix::PosixOwner me{::geteuid(), ::getegid()};
    posix::PosixDataFolder::Options o;
    o.scratchUser = ::geteuid();
    ScratchFolder s;
    s.folder = std::make_unique<posix::PosixDataFolder>(directory, o);
    s.io = std::make_unique<posix::PosixFileIo>(posix::realPosixSys(), me, 0644);
    return s;
}

QString LinuxHost::ownExecutable()
{
    return QStringLiteral("/proc/self/exe");   // stat follows it to the running program's file
}

QString LinuxHost::installedExecutable()
{
    return QString::fromLatin1(posix::SystemdUnit::kInstalledBinary);
}

int LinuxHost::runScratch(const QString&, const BridgeConfig&, Console& c)
{
    c.err(QStringLiteral("error: --run runs the bridge, which only the console executable has: aether-serial-bridge --run "
                         "--config <file>."));
    return 2;
}

int LinuxHost::runService()
{
    std::fputs("error: --service runs only in the console executable, started by systemd.\n", stderr);
    return 2;
}

int LinuxHost::listPorts(Console& c)
{
    QString text;
    {
        QTextStream out(&text);
        posix::SerialPortList::print(out);
    }
    QStringList lines = text.split(QLatin1Char('\n'));
    // print() ends with a newline: the last, empty piece is not a line of its own.
    if (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    for (const QString& line : lines) c.out(line);
    return 0;
}

std::optional<bool> LinuxHost::confirm(const QString& question)
{
    if (!::isatty(0)) return std::nullopt;
    std::fputs(question.toLocal8Bit().constData(), stderr);
    std::fflush(stderr);
    char line[64] = {};
    if (!std::fgets(line, sizeof line, stdin)) return false;
    const QString a = QString::fromLocal8Bit(line).trimmed().toLower();
    return a == QLatin1String("y") || a == QLatin1String("yes");
}

}  // namespace asb::cli
