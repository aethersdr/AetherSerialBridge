#pragma once

// The Windows Host (WS-9; Windows only, thin): the managed locations from windowsPaths() (serviceLocations() +
// %ProgramFiles%, no fallback), the job-object guard (WS-6 installProcessGuard) at mode entry, TokenElevation, and
// the Applier's Platform = win::WindowsPlatform (WS-6/WS-7), built on first use with the program to install and
// its digest: the console executable installs its own image (expected = that image's digest); the GUI passes the
// console executable beside it and the digest it was built with (HeadlessDigest.h).

#include "cli/Commands.h"

#include <QByteArray>

#include <memory>
#include <mutex>

namespace asb::win {
class WindowsPlatform;
}

namespace asb::cli {

class WindowsHost : public Host {
public:
    struct Image {
        QString path;              // empty: this executable; a bare file name: that file beside this executable
        QByteArray expectedSha256; // empty: the digest of the image read from `path`
    };
    WindowsHost(int argc, char** argv, Image image = {});
    ~WindowsHost() override;

    bool windows() const override { return true; }
    QString managedConfigFile() const override { return m_configFile; }
    QString dataFolder() const override { return m_dataFolder; }

    void enterGuardedMode() override;
    void enterStandardUserJob() override;
    bool elevated() override;
    Platform* platform(QString* error, int* code) override;
    QStringList takeReports() override;
    QString thisBuild() override;

    ScratchFolder scratchFolder(const QString& directory) override;
    QString ownExecutable() override;
    QString installedExecutable() override;

    int runScratch(const QString& configFile, const BridgeConfig& loopbackConfig, Console&) override;
    int runService() override;
    int listPorts(Console&) override;
    std::optional<bool> confirm(const QString& question) override;
    bool messageBox(const QString& text, Box) override;
    void detachConsole() override;
    Relaunch relaunchElevated(const QStringList& arguments) override;

protected:
    int m_argc;
    char** m_argv;

private:
    Image m_image;
    QByteArray m_expectedSha256;   // the program this host installs (set when the Platform is built)
    QString m_configFile, m_dataFolder, m_lockDirectory;
    std::mutex m_reportsMutex;
    QStringList m_reports;
    std::unique_ptr<win::WindowsPlatform> m_wp;
};

}  // namespace asb::cli
