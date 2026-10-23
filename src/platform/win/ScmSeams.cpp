// The native ScmSeams (WS-7, Windows only): a thin pass-through to the SCM, the registry and read-only file opens.

#include "platform/win/ScmSeams.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace asb::win {

namespace {

std::wstring W(const QString& s) { return s.toStdWString(); }
const wchar_t* Wp(const std::wstring& s) { return s.empty() ? nullptr : s.c_str(); }

using ActionType = ServicePlan::FailureAction::Type;
SC_ACTION_TYPE scActionOf(ActionType t)
{
    switch (t) {
    case ActionType::None: return SC_ACTION_NONE;
    case ActionType::Reboot: return SC_ACTION_REBOOT;
    case ActionType::RunCommand: return SC_ACTION_RUN_COMMAND;
    case ActionType::Restart:
    case ActionType::Other: break;
    }
    return SC_ACTION_RESTART;
}
ActionType typeOf(SC_ACTION_TYPE t)
{
    switch (t) {
    case SC_ACTION_NONE: return ActionType::None;
    case SC_ACTION_RESTART: return ActionType::Restart;
    case SC_ACTION_REBOOT: return ActionType::Reboot;
    case SC_ACTION_RUN_COMMAND: return ActionType::RunCommand;
    default: return ActionType::Other;
    }
}

class NativeScm final : public ScmSeams {
public:
    quint32 openService(const QString& name, quint32 access, Handle* out) override
    {
        SC_HANDLE m = manager();
        if (!m) return GetLastError();
        SC_HANDLE s = OpenServiceW(m, W(name).c_str(), access);
        if (!s) return GetLastError();
        *out = reinterpret_cast<Handle>(s);
        return 0;
    }
    quint32 createService(const QString& name, const QString& displayName, const QString& binaryPath, quint32 startType,
                          const QString& account, Handle* out) override
    {
        SC_HANDLE m = manager();
        if (!m) return GetLastError();
        const std::wstring acct = account == QLatin1String("LocalSystem") ? std::wstring() : W(account);
        SC_HANDLE s = CreateServiceW(m, W(name).c_str(), W(displayName).c_str(), SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                     startType, SERVICE_ERROR_NORMAL, W(binaryPath).c_str(), nullptr, nullptr, nullptr,
                                     Wp(acct), nullptr);
        if (!s) return GetLastError();
        *out = reinterpret_cast<Handle>(s);
        return 0;
    }
    quint32 changeConfig(Handle h, quint32 startType, const QString& binaryPath, const QString& displayName,
                         const QString& account) override
    {
        // The account by name, LocalSystem included (a null name would mean "no change" here, unlike CreateServiceW).
        const std::wstring acct = W(account);
        return ChangeServiceConfigW(sc(h), SERVICE_WIN32_OWN_PROCESS, startType, SERVICE_ERROR_NORMAL,
                                    W(binaryPath).c_str(), nullptr, nullptr, nullptr, acct.c_str(), nullptr,
                                    W(displayName).c_str())
                   ? 0
                   : GetLastError();
    }
    quint32 changeStartType(Handle h, quint32 startType) override
    {
        return ChangeServiceConfigW(sc(h), SERVICE_NO_CHANGE, startType, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
                                    nullptr, nullptr, nullptr, nullptr)
                   ? 0
                   : GetLastError();
    }
    quint32 setDescription(Handle h, const QString& text) override
    {
        std::wstring t = W(text);
        SERVICE_DESCRIPTIONW d{t.data()};
        return ChangeServiceConfig2W(sc(h), SERVICE_CONFIG_DESCRIPTION, &d) ? 0 : GetLastError();
    }
    quint32 setFailureActions(Handle h, const QList<ServicePlan::FailureAction>& actions, int resetSeconds) override
    {
        std::vector<SC_ACTION> a;
        for (const ServicePlan::FailureAction& f : actions) a.push_back({scActionOf(f.type), static_cast<DWORD>(f.delayMs)});
        SERVICE_FAILURE_ACTIONSW fa{};
        fa.dwResetPeriod = static_cast<DWORD>(resetSeconds);
        fa.cActions = static_cast<DWORD>(a.size());
        fa.lpsaActions = a.data();
        return ChangeServiceConfig2W(sc(h), SERVICE_CONFIG_FAILURE_ACTIONS, &fa) ? 0 : GetLastError();
    }
    quint32 setFailureFlag(Handle h, bool flag) override
    {
        SERVICE_FAILURE_ACTIONS_FLAG f{flag ? TRUE : FALSE};
        return ChangeServiceConfig2W(sc(h), SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &f) ? 0 : GetLastError();
    }
    quint32 queryConfig(Handle h, ScmServiceConfig* out) override
    {
        DWORD need = 0;
        QueryServiceConfigW(sc(h), nullptr, 0, &need);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return GetLastError();
        std::vector<BYTE> buf(need);
        auto* c = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
        if (!QueryServiceConfigW(sc(h), c, need, &need)) return GetLastError();
        out->binaryPath = c->lpBinaryPathName ? QString::fromWCharArray(c->lpBinaryPathName) : QString();
        out->startType = c->dwStartType;
        out->account = c->lpServiceStartName ? QString::fromWCharArray(c->lpServiceStartName) : QString();
        out->displayName = c->lpDisplayName ? QString::fromWCharArray(c->lpDisplayName) : QString();
        return 0;
    }
    quint32 queryFailureActions(Handle h, QList<ServicePlan::FailureAction>* actions, int* resetSeconds, bool* flag) override
    {
        DWORD need = 0;
        QueryServiceConfig2W(sc(h), SERVICE_CONFIG_FAILURE_ACTIONS, nullptr, 0, &need);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return GetLastError();
        std::vector<BYTE> buf(need);
        auto* fa = reinterpret_cast<SERVICE_FAILURE_ACTIONSW*>(buf.data());
        if (!QueryServiceConfig2W(sc(h), SERVICE_CONFIG_FAILURE_ACTIONS, buf.data(), need, &need)) return GetLastError();
        actions->clear();
        // Every action, whatever its type: the recovery judgement compares the whole list with the plan.
        for (DWORD i = 0; i < fa->cActions; ++i)
            actions->append({typeOf(fa->lpsaActions[i].Type), static_cast<int>(fa->lpsaActions[i].Delay)});
        *resetSeconds = static_cast<int>(fa->dwResetPeriod);
        SERVICE_FAILURE_ACTIONS_FLAG f{};
        DWORD n = 0;
        if (!QueryServiceConfig2W(sc(h), SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, reinterpret_cast<BYTE*>(&f), sizeof f, &n))
            return GetLastError();
        *flag = f.fFailureActionsOnNonCrashFailures != FALSE;
        return 0;
    }
    quint32 queryStatus(Handle h, ScmStatus* out) override
    {
        SERVICE_STATUS_PROCESS sp{};
        DWORD n = 0;
        if (!QueryServiceStatusEx(sc(h), SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&sp), sizeof sp, &n))
            return GetLastError();
        out->state = sp.dwCurrentState;
        out->processId = sp.dwProcessId;
        out->win32ExitCode = sp.dwWin32ExitCode;
        out->serviceSpecificExitCode = sp.dwServiceSpecificExitCode;
        return 0;
    }
    quint32 control(Handle h, quint32 code) override
    {
        SERVICE_STATUS st{};
        return ControlService(sc(h), code, &st) ? 0 : GetLastError();
    }
    quint32 start(Handle h) override { return StartServiceW(sc(h), 0, nullptr) ? 0 : GetLastError(); }
    quint32 remove(Handle h) override { return DeleteService(sc(h)) ? 0 : GetLastError(); }
    void close(Handle h) override { CloseServiceHandle(sc(h)); }

    quint32 flushServiceKey(const QString& name) override
    {
        HKEY k = nullptr;
        const std::wstring path = L"SYSTEM\\CurrentControlSet\\Services\\" + W(name);
        LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &k);
        if (rc != ERROR_SUCCESS) return static_cast<quint32>(rc);
        rc = RegFlushKey(k);
        RegCloseKey(k);
        return static_cast<quint32>(rc);
    }
    quint32 readFileHead(const QString& path, qint64 max, QByteArray* head, qint64* size) override
    {
        // Read-only, sharing everything, no execution. Links are followed: what is read is the file SCM would run.
        HANDLE f = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return GetLastError();
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(f, &sz)) {
            const DWORD e = GetLastError();
            CloseHandle(f);
            return e;
        }
        *size = sz.QuadPart;
        const qint64 want = std::min<qint64>(max, sz.QuadPart);
        head->resize(want);
        qint64 got = 0;
        while (got < want) {
            DWORD n = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<qint64>(want - got, 1 << 20));
            if (!ReadFile(f, head->data() + got, chunk, &n, nullptr)) {
                const DWORD e = GetLastError();
                CloseHandle(f);
                return e;
            }
            if (n == 0) break;
            got += n;
        }
        head->resize(got);
        CloseHandle(f);
        return 0;
    }
    bool fileExists(const QString& path) override
    {
        return GetFileAttributesW(W(path).c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    quint32 processesOfImage(const QString& path, QList<quint32>* out) override
    {
        out->clear();
        const QString want = QString(path).replace(QLatin1Char('/'), QLatin1Char('\\'));
        const QString wantName = want.mid(want.lastIndexOf(QLatin1Char('\\')) + 1);
        const DWORD self = GetCurrentProcessId();
        // One snapshot of every process with its image file name: no fixed-size pid buffer to truncate, and only
        // the processes named like the installed exe are opened (an unrelated protected process that refuses
        // PROCESS_QUERY_LIMITED_INFORMATION cannot make every query fail).
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return GetLastError();
        struct CloseSnap {
            HANDLE h;
            ~CloseSnap() { CloseHandle(h); }
        } closeSnap{snap};
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof pe;
        if (!Process32FirstW(snap, &pe)) return GetLastError();   // there is always at least this process
        for (;;) {
            const DWORD pid = pe.th32ProcessID;
            if (pid != 0 && pid != self && QString::fromWCharArray(pe.szExeFile).compare(wantName, Qt::CaseInsensitive) == 0) {
                if (const quint32 e = checkCandidate(pid, want, out)) return e;
            }
            if (!Process32NextW(snap, &pe)) {
                const DWORD e = GetLastError();
                if (e == ERROR_NO_MORE_FILES) return 0;   // the end of the snapshot: the only success
                return e;
            }
        }
    }

private:
    // One process named like the installed exe: appended to `out` when it is in session 0, its full image path is
    // `want` and it is still running. 0, or the Win32 error that leaves the answer unknown.
    static quint32 checkCandidate(DWORD pid, const QString& want, QList<quint32>* out)
    {
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!p) {
            const DWORD e = GetLastError();
            return e == ERROR_INVALID_PARAMETER ? 0 : e;   // already gone: not a survivor
        }
        struct CloseProcess {
            HANDLE h;
            ~CloseProcess() { CloseHandle(h); }
        } closeProcess{p};
        DWORD session = 0;
        if (!ProcessIdToSessionId(pid, &session)) {
            const DWORD e = GetLastError();
            return e == ERROR_INVALID_PARAMETER ? 0 : e;
        }
        if (session != 0) return 0;   // not a service process
        std::vector<wchar_t> name(32768);
        DWORD len = DWORD(name.size() - 1);
        if (!QueryFullProcessImageNameW(p, 0, name.data(), &len)) return GetLastError();
        if (QString::fromWCharArray(name.data(), static_cast<qsizetype>(len)).compare(want, Qt::CaseInsensitive) != 0) return 0;
        DWORD code = 0;
        if (!GetExitCodeProcess(p, &code)) return GetLastError();
        if (code == STILL_ACTIVE) out->append(pid);
        return 0;
    }
    static SC_HANDLE sc(Handle h) { return reinterpret_cast<SC_HANDLE>(h); }
    SC_HANDLE manager()
    {
        std::lock_guard g(m_m);
        if (!m_manager) m_manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
        if (!m_manager) m_manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        return m_manager;
    }
    std::mutex m_m;
    SC_HANDLE m_manager = nullptr;
};

}  // namespace

ScmSeams& nativeScmSeams()
{
    static NativeScm* s = new NativeScm();   // never destroyed (process lifetime)
    return *s;
}

}  // namespace asb::win
