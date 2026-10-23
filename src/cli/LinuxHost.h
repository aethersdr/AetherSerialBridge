#pragma once

// The Linux Host (WS-9): the managed locations from serviceLocations(), the seccomp guard (WS-8 ProcessGuard) at
// mode entry, geteuid() for elevation, and the Applier's Platform = posix::LinuxPlatform (systemd over D-Bus, the
// POSIX data folder and locks; WS-8), built on first use. What the adapters report outside Result (apply-lock mode
// resets, the unit/binary/policy files install and uninstall write or remove) is collected for the printed result.

#include "cli/Commands.h"
#include "platform/posix/LinuxPlatform.h"

#include <memory>
#include <mutex>

namespace asb::cli {

// The SHA-256 (lower-case hex) of this process's own program, /proc/self/exe; empty when it cannot be read.
QByteArray ownImageSha256();

class LinuxHost : public Host {
public:
    // `programSha256`: this build's program digest (lower-case SHA-256 hex; v1.1 D61, INV-120), which detect()
    // compares the installed program with. The window passes the digest compiled into it (the program its AppImage
    // embeds); empty (the headless program: the CLI, the TUI, the pkexec helper) = the SHA-256 of /proc/self/exe,
    // hashed once, at the first platform() or thisBuild() call. The installed helper's image is the installed file,
    // so it classifies its own installation as Current.
    LinuxHost(int argc, char** argv, QByteArray programSha256 = QByteArray());
    ~LinuxHost() override;

    bool windows() const override { return false; }
    QString managedConfigFile() const override { return m_configFile; }
    QString dataFolder() const override { return m_dataFolder; }

    void enterGuardedMode() override;
    void blockStopSignals() override;
    bool elevated() override;
    Platform* platform(QString* error, int* code) override;
    QStringList takeReports() override;
    QString engineLockHolder() override;

    ScratchFolder scratchFolder(const QString& directory) override;
    QString ownExecutable() override;
    QString installedExecutable() override;

    // Without an engine: refused (the GUI executable). The headless executable overrides both.
    int runScratch(const QString& configFile, const BridgeConfig& loopbackConfig, Console&) override;
    int runService() override;
    int listPorts(Console&) override;
    std::optional<bool> confirm(const QString& question) override;
    // "<version>, program sha256 <12 hex>", as WindowsHost names its build (just the version when the digest is
    // unknown).
    QString thisBuild() override;

    // This build's program digest (see the constructor); empty when /proc/self/exe could not be read (then nothing
    // is ever Current: fail closed).
    QByteArray programSha256();

    posix::LinuxPlatform* linuxPlatform() { return m_lp.get(); }

protected:
    // Test builds: the headless executable's suite seams (a pausing PosixSys, step pauses, a failing firewall).
    virtual void customise(posix::LinuxPlatform::Options&) {}
    // A firewall to use in place of the platform's (nullptr: the platform's own).
    virtual IFirewall* wrapFirewall(IFirewall&) { return nullptr; }

    int m_argc;
    char** m_argv;

private:
    QString m_configFile, m_dataFolder;
    QByteArray m_programSha256;
    bool m_programHashed = false;
    std::mutex m_reportsMutex;   // LockReport runs on the operation worker
    QStringList m_reports;
    std::unique_ptr<posix::LinuxPlatform> m_lp;
    std::unique_ptr<Platform> m_platform;
};

}  // namespace asb::cli
