// The job-object process guard and the production paths (Windows only). See NativeSeams.h.
#include "platform/win/NativeSeams.h"

#include "core/BridgeRunner.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

namespace asb::win {

namespace detail {

JobCalls nativeJobCalls()
{
    JobCalls c;
    // Unnamed: nothing else can open it. Not inheritable (default SECURITY_ATTRIBUTES).
    c.create = []() -> void* { return CreateJobObjectW(nullptr, nullptr); };
    c.limit = [](void* job) -> bool {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        // ACTIVE_PROCESS = 1: this process is the one. No JOB_OBJECT_LIMIT_BREAKAWAY_OK /
        // SILENT_BREAKAWAY_OK, and no KILL_ON_JOB_CLOSE (the job lives as long as this process).
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        return SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) != FALSE;
    };
    c.assign = [](void* job) -> bool { return AssignProcessToJobObject(job, GetCurrentProcess()) != FALSE; };
    c.close = [](void* job) { CloseHandle(job); };
    return c;
}

bool installProcessGuard(QString* error, const JobCalls& calls)
{
    // `code` is the failing call's own GetLastError(), read before CloseHandle can overwrite it.
    auto fail = [error](const char* what, unsigned long code) {
        if (error) *error = QStringLiteral("%1 failed (error %2)").arg(QLatin1String(what)).arg(code);
        return false;
    };
    void* job = calls.create();
    if (!job) return fail("CreateJobObjectW", GetLastError());
    if (!calls.limit(job)) {
        const DWORD code = GetLastError();
        calls.close(job);
        return fail("SetInformationJobObject", code);
    }
    if (!calls.assign(job)) {
        const DWORD code = GetLastError();
        calls.close(job);
        return fail("AssignProcessToJobObject", code);
    }
    // The handle is kept for the life of the process (closing it would not free the process either).
    return true;
}

}  // namespace detail

bool installProcessGuard(QString* error)
{
    return detail::installProcessGuard(error, detail::nativeJobCalls());
}

namespace {

bool limitStandardUserJob(void* job, unsigned limit)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    // No breakaway, and NO kill-on-job-close (v1.1 station fix, 2026-10-18): the elevated window that Restart as
    // administrator starts is created by AppInfo with this window as its parent process, so it lands in this job; the
    // window then closes, its job handle goes, and kill-on-close ended the new window within milliseconds (it never
    // appeared). v1.1 starts no child of its own; v1.2's session child must end with the window some other way.
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = limit;
    return SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) != FALSE;
}

}  // namespace

bool installStandardUserJob(unsigned activeProcessLimit, QString* error)
{
    detail::JobCalls c = detail::nativeJobCalls();
    c.limit = activeProcessLimit == 1 ? +[](void* job) { return limitStandardUserJob(job, 1); }
                                      : +[](void* job) { return limitStandardUserJob(job, 2); };
    return detail::installProcessGuard(error, c);
}

JobLimits currentJobLimits()
{
    JobLimits out;
    BOOL in = FALSE;
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &in) || !in) return out;
    out.inJob = true;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (!QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &limits, sizeof limits, nullptr)) return out;
    if (limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS)
        out.activeProcessLimit = limits.BasicLimitInformation.ActiveProcessLimit;
    out.killOnClose = (limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0;
    return out;
}

std::optional<QString> programFilesLockDirectory()
{
    PWSTR p = nullptr;
    QString root;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &p)) && p)
        root = QString::fromWCharArray(p);
    CoTaskMemFree(p);
    return programFilesLockDirectoryFrom(root);
}

std::optional<QString> programFilesLockDirectoryFrom(QString root)
{
    root.replace(QLatin1Char('/'), QLatin1Char('\\'));
    const bool drive = root.size() >= 3 && root[0].isLetter() && root[1] == QLatin1Char(':') && root[2] == QLatin1Char('\\');
    const bool unc = root.size() > 2 && root.startsWith(QLatin1String("\\\\")) && root[2] != QLatin1Char('\\');
    if (!drive && !unc) return std::nullopt;
    if (!root.endsWith(QLatin1Char('\\'))) root += QLatin1Char('\\');
    return root + QStringLiteral("AetherSerialBridge");
}

std::optional<WindowsPaths> windowsPaths(QString* error)
{
    const ServiceLocations loc = serviceLocations();
    if (!loc.locations) {
        if (error) *error = loc.error;
        return std::nullopt;
    }
    return windowsPathsFrom(loc, programFilesLockDirectory(), error);
}

std::optional<WindowsPaths> windowsPathsFrom(const ServiceLocations& loc, const std::optional<QString>& lockDir, QString* error)
{
    if (!loc.locations) {
        if (error) *error = loc.error;
        return std::nullopt;
    }
    if (!lockDir) {
        if (error) *error = QStringLiteral("Cannot determine the Windows Program Files folder (SHGetKnownFolderPath failed); refusing to start.");
        return std::nullopt;
    }
    QString data = loc.locations->dataDirectory();
    data.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return WindowsPaths{data, *lockDir};
}

}  // namespace asb::win
