#pragma once

// Native support for the WS-7 service rows on windows-latest (admin) and for the qualification harness
// (asb_win_qual). Test code only: it drives the SCM directly (StartServiceW / ControlService / QueryServiceStatusEx),
// sets the service's Environment value (the service's test seams, ServiceMain.cpp), and runs Applier operations
// over the real Windows Platform (WindowsPlatform) — the same objects WS-9's CLI will build.
//
// A dynamic-Qt test build's service needs the Qt DLLs beside the installed exe (the shipped exe is static, D3):
// deployDevRuntime() copies them into %ProgramFiles%\AetherSerialBridge before the first start.

#include "core/Applier.h"
#include "platform/win/WindowsPlatform.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <chrono>
#include <memory>
#include <optional>

namespace asb::test::svc {

inline constexpr char kService[] = "AetherSerialBridge";

// The headless executable under test (ASB_HEADLESS_EXE, or the path given).
QString headlessExe();
void setHeadlessExe(const QString&);

// The Windows Platform over the real system, installing the headless exe (expected digest = its own).
std::unique_ptr<win::WindowsPlatform> platform(QString* error = nullptr);

// Copies the *.dll files of `runtimeDir` (default: the directory of the Qt this process runs on: Qt6Core,
// Qt6Network, Qt6SerialPort) into the lock directory, creating it if needed. Returns the names copied. Copies
// nothing when ASB_HEADLESS_STATIC=1 (the release workflow's runs against the static release executables).
QStringList deployDevRuntime(const QString& lockDirectory, const QString& runtimeDir = {});

// One enabled port on `tcpPort`, COM `com`, allow-list `allowed`.
QByteArray configJson(int tcpPort, const QString& allowed = {}, const QString& com = QStringLiteral("COM9"));
SaveRequest request(const QByteArray& config);

// Applier operations, synchronous on the calling thread.
Result install(win::WindowsPlatform&, const QByteArray& config);
Result apply(win::WindowsPlatform&, const QByteArray& config);
Result start(win::WindowsPlatform&);
Result stop(win::WindowsPlatform&);
Result uninstall(win::WindowsPlatform&);
QString describe(const Result&);

// The service's environment (REG_MULTI_SZ "Environment" on its key): the test seams. Empty list = removed.
bool setServiceEnvironment(const QStringList& keyValues);

struct ScmState {
    quint32 state = 0, pid = 0, win32ExitCode = 0, specificExitCode = 0, checkPoint = 0;
    bool exists = false;
};
ScmState query();
// Polls until the state is `state` (and, for Running, a pid exists) or `ms` pass.
bool waitState(quint32 state, int ms, ScmState* last = nullptr);
// After DeleteService, SCM removes the service only once every handle to it is closed (until then it is "marked
// for delete" and a new CreateService fails with 1072). Waits up to `ms` for it to be gone; true when it is.
bool waitDeleted(int ms);
quint32 startRaw();                         // StartServiceW; the Win32 error (0 = ok)
// ControlService(STOP) on a thread of its own (a blocked handler cannot stall the test); returns at once.
void stopAsync();
quint32 stopSync();                         // ControlService(STOP); the Win32 error
// A process handle (SYNCHRONIZE | QUERY_LIMITED_INFORMATION) for `pid`; nullptr if gone.
void* openProcess(quint32 pid);
// Waits for the process to end; true + elapsed when it did within `ms`.
bool waitExit(void* process, int ms, std::chrono::milliseconds* elapsed = nullptr, quint32* exitCode = nullptr);
void closeHandle(void*);
// Kills every running process of the installed exe (test cleanup only).
void killInstalledProcesses(const QString& installedExe);
// Removes the service whatever its state (cleanup): disable, stop, kill, delete; and the Apps & features entry the
// install wrote (WS-16). True when the service is gone.
bool removeServiceHard(const QString& installedExe);

// WS-16: the Apps & features entry (HKLM\<win::UninstallEntry::kKey>, the 64-bit view this x64 process reads).
bool appsEntryExists();
// A REG_SZ value as written, a REG_DWORD as decimal text; empty when the key or the value is absent.
QString appsEntryValue(const char* name);
bool removeAppsEntry();   // true when it is gone

// WS-16: the restart deletions (PendingFileRenameOperations, NT paths) whose source or target is under `folder`, and
// their removal: exactly those pairs go, every other pair stays as it is, in its order (other software and Windows
// Update write this value too). drop... is false when the value does not read as pairs or cannot be written.
QStringList restartDeletionsUnder(const QString& folder);
bool dropRestartDeletionsUnder(const QString& folder);

// v1.1 station fix: the System event log, read with the classic API (ReadEventLogW). systemLogMark() is the newest
// record number now (0 when the log is empty or cannot be read); scmEventsAfter() lists the Service Control Manager
// events with one of `ids` (7024 service-specific error, 7031/7034 terminated unexpectedly, ...) written after
// `mark` whose first insertion string is `displayName`, as "<id>: <strings>" lines. Record numbers, not times:
// TimeGenerated has a resolution of one second, and earlier rows in the same run log 7031s on purpose. `ok` is false
// when the log could not be read (then the list proves nothing).
quint32 systemLogMark();
QStringList scmEventsAfter(quint32 mark, const QList<quint32>& ids, const QString& displayName, bool* ok = nullptr);

// A TCP round trip on 127.0.0.1:port: connects, sends `bytes`, reads them back (the echo device seam).
bool echoRoundTrip(int port, const QByteArray& bytes, int ms, QString* why);
bool canConnect(int port, int ms);

}  // namespace asb::test::svc
