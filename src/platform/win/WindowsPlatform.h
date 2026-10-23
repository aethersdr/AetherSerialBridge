#pragma once

// The Windows Platform an Applier runs on (WS-7 for WS-9; Windows only): every seam of Interfaces.h over the
// real system — WindowsDataFolder + WinFileIo (WS-6), FileApplyLock + EngineLock(Applier) (WS-6), FileStartToken,
// WindowsService over the SCM, CimFirewall over WMI, and the status reader. The paths come only from
// windowsPaths() (serviceLocations() + %ProgramFiles%; no fallback folder). The hosts (the CLI, the GUI, the
// qualification harness) give the install source: the bytes of the headless exe to install and this build's
// digest of it.

#include "core/Interfaces.h"
#include "platform/win/LockFiles.h"

#include <QByteArray>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

namespace asb::win {

class WindowsService;

class WindowsPlatform {
public:
    struct Options {
        QByteArray expectedHeadlessSha256;                                   // lower-case hex
        std::function<std::optional<QByteArray>(QString* error)> image;      // the exe to install
        LockReport lockReport;                                               // WS-6 LockReport (folded into fileChanges by WS-9)
    };
    // nullptr + *error when the Windows folders cannot be determined (kWindowsDataFolderError, or Program Files).
    static std::unique_ptr<WindowsPlatform> create(Options, QString* error);
    ~WindowsPlatform();

    Platform& platform();
    WindowsService& service();
    QString dataFolder() const;
    QString lockDirectory() const;
    QString installedExe() const;

    struct Impl;

private:
    explicit WindowsPlatform(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> d;
};

// The bytes of a file (the CLI's own image, or the exe beside the GUI), read in one go; nullopt + *error.
std::optional<QByteArray> readWholeFile(const QString& path, QString* error);

}  // namespace asb::win
