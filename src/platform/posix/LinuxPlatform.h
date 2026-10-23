#pragma once

// The Linux Platform (WS-8): every seam the Applier touches, built from the service locations and owned
// here, for the processes that run the Applier as root (the CLI and the pkexec helper, WS-9; the systemd
// suite's test operations). Construct it after the QCoreApplication (QtDBus needs one) and after the
// process guard (INV-110).
//
//   folder      PosixDataFolder   /etc/aether-serial-bridge (Config owner root)
//   service     SystemdService    over QtSystemdBus (the system bus)
//   firewall    NullFirewall      (no managed firewall on Linux, D7)
//   applyLock   FileApplyLock     /run/aether-serial-bridge.apply.lock
//   engineLock  EngineLock        /var/lib/aether-serial-bridge/engine.lock (+ engine.lock.id)
//   token       FileStartToken    over the folder and its FileIo
//   status      PosixStatusReader /run/aether-serial-bridge/status.json (the ACK, read only)
//   io          PosixFileIo       Config owner root, mode 0644

#include "core/AppLocations.h"
#include "core/Interfaces.h"
#include "core/StartToken.h"
#include "platform/posix/LinuxLocks.h"
#include "platform/posix/NullFirewall.h"
#include "platform/posix/PosixDataFolder.h"
#include "platform/posix/PosixFileIo.h"
#include "platform/posix/QtSystemdBus.h"
#include "platform/posix/SystemdService.h"

#include <memory>

namespace asb::posix {

// status.json read without following links, regular files only, at most StatusStore::kStatusMaxBytes.
class PosixStatusReader final : public IStatusReader {
public:
    explicit PosixStatusReader(QString path, PosixSys& sys = realPosixSys()) : m_path(std::move(path)), m_sys(sys) {}
    std::optional<StatusReport> read() override;

private:
    QString m_path;
    PosixSys& m_sys;
};

struct LinuxPaths {
    QString dataFolder = QStringLiteral("/etc/aether-serial-bridge");
    QString statusFile = QString::fromLatin1(kLinuxStatusFile);
    QString runtimeDirectory = QStringLiteral("/run/aether-serial-bridge");
    QString lockDirectory = QString::fromLatin1(kEngineLockDirectory);

    static LinuxPaths from(const AppLocations&);
};

class LinuxPlatform {
public:
    struct Options {
        LinuxPaths paths;
        SystemdService::Options service;   // its lockDirectory follows paths.lockDirectory
        LockReport report;                 // apply-lock mode resets (WS-9: into Result::fileChanges)
        PosixSys* sys = nullptr;           // nullptr: realPosixSys()
    };
    explicit LinuxPlatform(Options);
    Platform& platform() { return m_platform; }

    PosixDataFolder& folder() { return m_folder; }
    SystemdService& service() { return m_service; }
    EngineLock& engineLock() { return m_engineLock; }
    FileApplyLock& applyLock() { return m_applyLock; }
    PosixFileIo& io() { return m_io; }

private:
    Options m_opt;
    PosixSys& m_sys;
    PosixFileIo m_io;
    PosixDataFolder m_folder;
    QtSystemdBus m_bus;
    SystemdService m_service;
    NullFirewall m_firewall;
    FileApplyLock m_applyLock;
    EngineLock m_engineLock;
    FileStartToken m_token;
    PosixStatusReader m_status;
    Platform m_platform;
};

}  // namespace asb::posix
