#include "platform/posix/LinuxPlatform.h"

namespace asb::posix {

std::optional<StatusReport> PosixStatusReader::read()
{
    const QByteArray p = m_path.toLocal8Bit();
    Fd fd(m_sys, m_sys.open(p.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (!fd) return std::nullopt;
    struct stat st {};
    if (m_sys.fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) return std::nullopt;
    QString err;
    const std::optional<QByteArray> b = preadAll(m_sys, fd.get(), StatusStore::kStatusMaxBytes, &err);
    if (!b) return std::nullopt;
    return StatusStore::fromJson(*b);
}

LinuxPaths LinuxPaths::from(const AppLocations& l)
{
    LinuxPaths p;
    p.dataFolder = l.dataDirectory();
    p.statusFile = l.statusFile;
    p.runtimeDirectory = l.statusFile.left(l.statusFile.lastIndexOf(QLatin1Char('/')));
    return p;
}

namespace {
SystemdService::Options serviceOptions(SystemdService::Options o, const LinuxPaths& p)
{
    o.lockDirectory = p.lockDirectory;
    return o;
}
EngineLock::Options lockOptions(const LinuxPaths& p)
{
    EngineLock::Options o;
    o.directory = p.lockDirectory;
    return o;
}
FileApplyLock::Options applyOptions(const LockReport& report)
{
    FileApplyLock::Options o;
    o.report = report;
    return o;
}
}  // namespace

LinuxPlatform::LinuxPlatform(Options o)
    : m_opt(std::move(o)),
      m_sys(m_opt.sys ? *m_opt.sys : realPosixSys()),
      m_io(m_sys, PosixOwner::root(), 0644),
      m_folder(m_opt.paths.dataFolder, PosixDataFolder::Options{}, m_sys),
      m_service(m_bus, serviceOptions(m_opt.service, m_opt.paths), systemClock(), m_sys),
      m_applyLock(applyOptions(m_opt.report), m_sys),
      m_engineLock(lockOptions(m_opt.paths), systemClock(), m_sys),
      m_token(m_folder, m_io),
      m_status(m_opt.paths.statusFile, m_sys),
      m_platform{m_folder, m_service, m_firewall, m_applyLock, m_engineLock, m_token, m_status, m_io}
{
}

}  // namespace asb::posix
