#pragma once

// The real Windows implementation of NtSeams (NtSeams.cpp; Windows only), the job-object process guard and
// the production paths. Everything portable is in the other files of this directory.

#include "core/BridgeRunner.h"
#include "platform/win/NtSeams.h"

#include <QString>

#include <optional>

namespace asb::win {

// The process-wide native seams (stateless; thread-safe).
NtSeams& nativeSeams();

// S17, INV-104, INV-112 runtime guard: puts THIS process into a new unnamed job with
// JOB_OBJECT_LIMIT_ACTIVE_PROCESS = 1 and no breakaway, so every CreateProcess* from it fails
// (ERROR_NOT_ENOUGH_QUOTA; with CREATE_BREAKAWAY_FROM_JOB, access denied). Only direct children are
// contained: WMI provider hosts and COM servers are started by system brokers outside the job, so CIM keeps
// working. Irreversible for the life of the process; called at mode entry (WS-9) before any Applier or
// engine object exists. false with *error when the job cannot be created, limited or joined (fatal).
bool installProcessGuard(QString* error);

// v1.1 WS-17 (D65, INV-130): a standard user's window: a new unnamed job with JOB_OBJECT_LIMIT_ACTIVE_PROCESS =
// `activeProcessLimit` (2 in the product; 1 only through the test seam ASB_TEST_STANDARD_JOB_LIMIT for HT W-2 step 5)
// and no breakaway. The handle is kept for the life of the process. NOT kill-on-job-close (v1.1 station fix): the
// runas relaunch inherits this job, and kill-on-close ended it the moment this window closed.
bool installStandardUserJob(unsigned activeProcessLimit, QString* error);

// The job this process is in, as QueryInformationJobObject(NULL) reads it (test builds print it: tst_GuiNative).
struct JobLimits {
    bool inJob = false;
    unsigned activeProcessLimit = 0;   // 0 = no active-process limit
    bool killOnClose = false;
};
JobLimits currentJobLimits();

namespace detail {
// The job calls installProcessGuard makes, replaceable by a test. Each reports failure as the Win32 call
// does: false (create: nullptr) with the thread's last error set. close is CloseHandle.
struct JobCalls {
    void* (*create)() = nullptr;
    bool (*limit)(void* job) = nullptr;    // JOB_OBJECT_LIMIT_ACTIVE_PROCESS = 1, no breakaway
    bool (*assign)(void* job) = nullptr;   // this process
    void (*close)(void* job) = nullptr;
};
JobCalls nativeJobCalls();
// The guard over `calls`; *error names the failing call and ITS error code (read before the job handle is
// closed, which may overwrite the last error).
bool installProcessGuard(QString* error, const JobCalls& calls);
}  // namespace detail

// %ProgramFiles%\AetherSerialBridge from SHGetKnownFolderPath(FOLDERID_ProgramFiles), '\' separators.
// nullopt when the folder cannot be determined or is not a drive or UNC path: no fallback (as AppLocations).
std::optional<QString> programFilesLockDirectory();

// The two Windows roots the adapters need. The data folder comes ONLY from serviceLocations() (its nullopt is
// kWindowsDataFolderError, exit 3); the lock directory from programFilesLockDirectory(). nullopt + *error
// when either is unknown; the caller refuses to start (WS-7 service host, WS-9 CLI/GUI).
struct WindowsPaths {
    QString dataFolder;
    QString lockDirectory;
};
std::optional<WindowsPaths> windowsPaths(QString* error);

// The two lookups' fail-closed branches over given results (WS-7 row "the Windows path lookups fail closed"):
// `knownFolder` is what SHGetKnownFolderPath(FOLDERID_ProgramFiles) gave (empty when it failed); `loc` is
// serviceLocations()'s answer and `lockDirectory` programFilesLockDirectory()'s. Never a guessed folder.
std::optional<QString> programFilesLockDirectoryFrom(QString knownFolder);
std::optional<WindowsPaths> windowsPathsFrom(const ServiceLocations& loc, const std::optional<QString>& lockDirectory, QString* error);

}  // namespace asb::win
