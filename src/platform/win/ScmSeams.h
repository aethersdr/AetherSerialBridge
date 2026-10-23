#pragma once

// The SCM seam of WindowsService (WS-7). Every service-control-manager, registry-flush and read-only file call
// WindowsService makes goes through ScmSeams, with the Win32 values passed through unchanged and the Win32 error
// code returned (0 = success), so the decisions are asserted on a recording fake on every OS (tests/support/
// FakeScm). The native implementation (ScmSeams.cpp, Windows only) is a thin pass-through:
// OpenSCManagerW / OpenServiceW / CreateServiceW / ChangeServiceConfigW / ChangeServiceConfig2W /
// QueryServiceConfigW / QueryServiceStatusEx / ControlService / StartServiceW / DeleteService, RegFlushKey on
// HKLM\SYSTEM\CurrentControlSet\Services\<name>, and CreateFileW(GENERIC_READ, share read|write|delete,
// OPEN_EXISTING) for the read-only evidence reads (the program named by a binPath is opened, never executed).

#include "core/ServicePlan.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace asb::win {

namespace scm {
// winsvc.h / winerror.h values.
inline constexpr quint32 kAutoStart = 2;          // SERVICE_AUTO_START
inline constexpr quint32 kDemandStart = 3;
inline constexpr quint32 kDisabled = 4;           // SERVICE_DISABLED
inline constexpr quint32 kQueryConfig = 0x0001;   // SERVICE_QUERY_CONFIG
inline constexpr quint32 kChangeConfig = 0x0002;
inline constexpr quint32 kQueryStatus = 0x0004;
inline constexpr quint32 kStart = 0x0010;
inline constexpr quint32 kStop = 0x0020;
inline constexpr quint32 kDelete = 0x00010000;    // DELETE
inline constexpr quint32 kAllAccess = 0x000F01FF; // SERVICE_ALL_ACCESS

inline constexpr quint32 kOk = 0;
inline constexpr quint32 kErrorAccessDenied = 5;
inline constexpr quint32 kErrorServiceAlreadyRunning = 1056;
inline constexpr quint32 kErrorServiceDisabled = 1058;
inline constexpr quint32 kErrorServiceDoesNotExist = 1060;
inline constexpr quint32 kErrorInvalidServiceControl = 1052;   // what a START_PENDING service answers a STOP (measured)
inline constexpr quint32 kErrorCannotAcceptControl = 1061;
inline constexpr quint32 kErrorServiceNotActive = 1062;
inline constexpr quint32 kErrorServiceExists = 1073;
inline constexpr quint32 kErrorServiceSpecificError = 1066;   // the service reported its own exit code
inline constexpr quint32 kErrorProcessAborted = 1067;         // the process ended without reporting STOPPED
inline constexpr quint32 kErrorServiceMarkedForDelete = 1072;
inline constexpr quint32 kErrorFileNotFound = 2;
inline constexpr quint32 kErrorPathNotFound = 3;
}  // namespace scm

struct ScmStatus {
    quint32 state = 0;            // SERVICE_STOPPED (1) ... SERVICE_RUNNING (4)
    quint32 processId = 0;        // 0 when no process (SCM's own view)
    quint32 win32ExitCode = 0;
    quint32 serviceSpecificExitCode = 0;
};

struct ScmServiceConfig {
    QString binaryPath;
    quint32 startType = 0;
    QString account;
    QString displayName;
};

class ScmSeams {
public:
    using Handle = quintptr;
    virtual ~ScmSeams() = default;

    virtual quint32 openService(const QString& name, quint32 access, Handle* out) = 0;
    // CreateServiceW(own process, error normal, account = `account` or LocalSystem when empty, no password).
    virtual quint32 createService(const QString& name, const QString& displayName, const QString& binaryPath,
                                  quint32 startType, const QString& account, Handle* out) = 0;
    // ChangeServiceConfigW(type own process, startType, error normal, binaryPath, display name, account).
    virtual quint32 changeConfig(Handle, quint32 startType, const QString& binaryPath, const QString& displayName,
                                 const QString& account) = 0;
    // ChangeServiceConfigW with only the start type (everything else SERVICE_NO_CHANGE / null).
    virtual quint32 changeStartType(Handle, quint32 startType) = 0;
    virtual quint32 setDescription(Handle, const QString&) = 0;
    // SERVICE_CONFIG_FAILURE_ACTIONS (restart delays) and SERVICE_CONFIG_FAILURE_ACTIONS_FLAG.
    virtual quint32 setFailureActions(Handle, const QList<ServicePlan::FailureAction>&, int resetSeconds) = 0;
    virtual quint32 setFailureFlag(Handle, bool) = 0;
    virtual quint32 queryConfig(Handle, ScmServiceConfig*) = 0;
    virtual quint32 queryFailureActions(Handle, QList<ServicePlan::FailureAction>*, int* resetSeconds, bool* flag) = 0;
    virtual quint32 queryStatus(Handle, ScmStatus*) = 0;
    virtual quint32 control(Handle, quint32 control) = 0;   // ControlService
    virtual quint32 start(Handle) = 0;
    virtual quint32 remove(Handle) = 0;                     // DeleteService
    virtual void close(Handle) = 0;

    // RegFlushKey(HKLM\SYSTEM\CurrentControlSet\Services\<name>) (INV-77).
    virtual quint32 flushServiceKey(const QString& name) = 0;
    // Read-only: the first `max` bytes of `path` and its size. Never executes anything.
    virtual quint32 readFileHead(const QString& path, qint64 max, QByteArray* head, qint64* size) = 0;
    virtual bool fileExists(const QString& path) = 0;
    // The live processes in session 0 (where services run) whose image is `path`, this process excluded
    // (a process snapshot, then ProcessIdToSessionId, QueryFullProcessImageNameW and GetExitCodeProcess on each
    // process whose image file NAME is path's; read-only). WindowsService::query uses it so that a pid of 0 is a
    // promise that no engine process exists, even one that outlived its STOPPED report. So it fails CLOSED
    // (review round 1): the snapshot failing, or a candidate that cannot be opened (other than one that
    // is already gone: ERROR_INVALID_PARAMETER) or whose session, image or exit code cannot be read, returns that
    // Win32 error and an incomplete `out`, never a shorter list as if complete. 0 = `out` is complete.
    virtual quint32 processesOfImage(const QString& path, QList<quint32>* out) = 0;
};

// The process-wide native ScmSeams (ScmSeams.cpp; Windows builds only).
ScmSeams& nativeScmSeams();

}  // namespace asb::win
