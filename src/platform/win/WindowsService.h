#pragma once

// IServiceControl on Windows (WS-7; master §4.5 "SCM configuration", §4.6 installation states; INV-41,
// INV-77). Portable logic over ScmSeams (the native pass-through is ScmSeams.cpp).
//
//  detect        QueryServiceConfigW of "AetherSerialBridge" -> InstallationEvidence::WindowsEvidence: the binPath,
//                the installed Qt exe's digest against this build's, and the lock files -> classifyWindows.
//                "service does not exist" is NotInstalled; any other query failure throws.
//  query         QueryServiceStatusEx; the pid SCM reports, else the pid of any session-0 process still running the
//                installed exe (one that outlived its STOPPED report): pid 0 promises that no engine process exists.
//  inhibitStarts SERVICE_DISABLED, then RegFlushKey on the service key (ServicePlan::blockStarts); a no-op when
//                the service does not exist.
//  allowStarts   SERVICE_AUTO_START, then RegFlushKey (ServicePlan::allowStarts).
//  requestStop   ControlService(STOP), not awaited; "not running" and "does not exist" are not errors. A service
//                still in START_PENDING cannot take the control (it accepts none; SCM answers 1052 or 1061): the STOP is retried until it
//                is taken, the service stops on its own, or the deadline passes (then the barrier's lock wait decides).
//  installOrConfigure  the install copy (InstallBinary, into the lock directory) then CreateServiceW / ChangeServiceConfigW
//                to ServicePlan::windowsConfig with starts DISABLED, the description, the failure actions (restart
//                5 s / 5 s / 30 s, reset 86400 s) and the failure flag, then RegFlushKey.
//  start         StartServiceW; "already running" is not an error.
//  uninstall     DeleteService; "does not exist" is not an error. Nothing else (WS-16, D69): the program goes in
//                removeProgram(), which the Applier calls only after the held lock files were marked for deletion.
//  removeProgram the installed exe removed from the lock directory through its FileIo; when it is this process's own
//                image (the console exe run from %ProgramFiles%, as Apps & features does), it is renamed aside in the
//                same folder ("aether-serial-bridge.exe.removed-<16 hex>") and registered for deletion at the next
//                restart (counts as removed). A failure (in use by another process) keeps it under its own name.
//  removeProgramFolder  after the locks were released: RemoveDirectoryW when the folder is empty; when only the scheduled
//                image is left, the folder is scheduled for deletion at restart too; anything else keeps it, named.
//  writeAppsEntry / removeAppsEntry  the Apps & features entry (UninstallEntry.h): written after installOrConfigure put
//                this build's program in place, removed last by uninstall.
//  installOrConfigure also removes the "*.removed-*" images an earlier uninstall left (when it can; one it cannot is
//                left for the restart).

#include "core/Interfaces.h"
#include "core/InstallationEvidence.h"
#include "platform/win/ScmSeams.h"
#include "platform/win/UninstallEntry.h"

#include <QDate>

#include <functional>
#include <optional>

namespace asb::win {

class WindowsService final : public IServiceControl {
public:
    // Where things are and what this build is. `image` supplies the bytes to install (nullopt + error when they
    // cannot be read); `expectedSha256` is this build's headless exe digest (both from the host: the GUI's
    // HeadlessDigest.h, the CLI's own image).
    struct Install {
        QString lockDirectory;                 // %ProgramFiles%\AetherSerialBridge (native separators)
        QByteArray expectedSha256;             // lower-case hex
        std::function<std::optional<QByteArray>(QString* error)> image;
        FileIo* io = nullptr;                  // the lock directory's FileIo (EngineLock::io())
        std::function<const DirHandle&()> dir; // the lock directory's handle (EngineLock::dir())
    };
    // WS-16: what uninstall and the Apps & features entry need beyond the install source (setLifecycle).
    struct Lifecycle {
        std::function<QStringList()> children;  // the names in the lock directory (EngineLock::children())
        IUninstallEntry* entry = nullptr;       // the Apps & features entry (D63); none: not written
        QString version;                        // this build's version, for the entry
        // Native pass-throughs (WindowsPlatform.cpp); a test passes fakes.
        std::function<bool(const QString& path)> isOwnImage;   // the file this process runs from (file identity)
        std::function<bool(const QString& from, const QString& to, QString* err)> rename;   // MoveFileExW(from, to)
        std::function<bool(const QString& path, QString* err)> deleteAtRestart;  // MOVEFILE_DELAY_UNTIL_REBOOT
        std::function<bool(const QString& path, QString* err)> removeDirectory;  // RemoveDirectoryW
        std::function<QString()> injectedRemovalFailure;   // test builds: ASB_TEST_FAIL_PROGRAM_REMOVAL=1
        std::function<QString()> randomSuffix;             // 16 hex digits
        std::function<QDate()> today;
    };

    WindowsService(ScmSeams& seams, Install install, IClock& clock = systemClock());
    void setLifecycle(Lifecycle l) { m_life = std::move(l); }

    Installation detect(Deadline) override;
    ServiceQuery query(Deadline) override;
    void inhibitStarts(Deadline) override;
    void allowStarts(Deadline) override;
    void requestStop(Deadline) override;
    void installOrConfigure(InstallState, Deadline) override;
    void start(Deadline) override;
    void uninstall(Deadline) override;
    void reconcileOutstandingJobs(Deadline) override {}
    bool separateProgramRemoval() const override { return true; }
    QString programFolder() const override { return m_install.lockDirectory; }
    ProgramRemoval removeProgram(Deadline) override;
    QStringList removeProgramFolder() override;
    QString writeAppsEntry() override;
    void removeAppsEntry(Deadline) override;
    static constexpr char kRemovedMarker[] = ".removed-";   // "aether-serial-bridge.exe.removed-<16 hex>"

    QString installedExe() const;
    // What uninstall did to the installed exe ("removed <path>", or "kept <path> (<why>)"); cleared by the call.
    QStringList takeChanges();
    // The program path of a binPath ("\"C:\\x y\\a.exe\" --service" -> C:\x y\a.exe).
    static QString programOf(const QString& binPath);
    // The evidence detect() classifies (exposed for the qualification harness and tests).
    InstallationEvidence::WindowsEvidence evidence();
    // Review round 3: what of the configuration installOrConfigure writes is missing or different on the open
    // service (empty: all of it). Throws when the recovery settings cannot be read.
    QString scmConfigProblem(ScmSeams::Handle, const ScmServiceConfig&);

private:
    std::optional<ScmSeams::Handle> open(quint32 access, quint32* err);
    void applyStartType(bool automatic, Deadline);
    // Review round 1 (class audit): a blocking call that returned after the deadline is not followed by a forward
    // step's change (installOrConfigure, allowStarts, start, uninstall). The fail-closed calls (inhibitStarts,
    // requestStop) complete whatever the time: they are what failClosed runs.
    void onTime(Deadline, const char* before) const;

    void removeLeftoverImages();

    ScmSeams& m_s;
    Install m_install;
    Lifecycle m_life;
    IClock& m_clock;
    QStringList m_changes;
    qint64 m_installedBytes = 0;   // the size of the copy installOrConfigure put in place (the entry's EstimatedSize)
    QString m_scheduledImage;      // removeProgram renamed this process's image aside to this name (scheduled)
};

QString scmErrorText(quint32 code);

}  // namespace asb::win
