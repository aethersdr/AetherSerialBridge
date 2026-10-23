#pragma once

// Native test support for the WS-6 rows on windows-latest (admin). Test harness only: it enables privileges,
// creates local accounts and impersonates them; production code never does any of this.
//
//  - scratch folders under %TEMP% (NTFS on C:), removed without following links;
//  - descriptors from SDDL, and SDDL of a path, for readable assertions;
//  - junctions and symbolic links;
//  - two local accounts, created with NetUserAdd under fresh asbtest-<hex> names and logged on in-process
//    (LogonUserW, network logon, no process creation): a standard user and a second administrator (master
//    §8.1). An account that already exists is never used, changed or deleted;
//  - impersonation and a token whose default owner is the user itself (the object-creator owner policy).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <lm.h>
#include <sddl.h>
#include <winioctl.h>

#include "WinTestNames.h"   // generated from ci/win-test-names.psd1

#include <QByteArray>
#include <QDir>
#include <QRandomGenerator>
#include <QString>
#include <QTest>

#include <optional>
#include <vector>

namespace asb::test::native {

inline const wchar_t* W(const QString& s)
{
    return reinterpret_cast<const wchar_t*>(s.utf16());
}

inline QString nativePath(QString p)
{
    p.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return p;
}

// A fresh empty folder under %TEMP%.
inline QString scratch(const QString& tag)
{
    wchar_t tmp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH, tmp);
    wchar_t longTmp[1024] = {};
    GetLongPathNameW(tmp, longTmp, 1023);
    QString base = QString::fromWCharArray(longTmp[0] ? longTmp : tmp);
    if (!base.endsWith(QLatin1Char('\\'))) base += QLatin1Char('\\');
    const QString p = base + QString::fromLatin1(names::kScratchPrefix) + QStringLiteral("%1-%2").arg(tag).arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
    CreateDirectoryW(W(p), nullptr);
    return p;
}

// Deletes a tree without following junctions or symbolic links (they are removed as links).
inline void removeTree(const QString& path)
{
    const DWORD attr = GetFileAttributesW(W(path));
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW(W(path + QStringLiteral("\\*")), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                const QString n = QString::fromWCharArray(fd.cFileName);
                if (n != QLatin1String(".") && n != QLatin1String("..")) removeTree(path + QLatin1Char('\\') + n);
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
    }
    SetFileAttributesW(W(path), FILE_ATTRIBUTE_NORMAL);
    if (attr & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(W(path));
    else DeleteFileW(W(path));
}

inline QByteArray sddl(const QString& s)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    ULONG n = 0;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(W(s), SDDL_REVISION_1, &sd, &n)) return {};
    QByteArray out(static_cast<const char*>(sd), int(n));
    LocalFree(sd);
    return out;
}

inline QString toSddl(const QByteArray& sd, SECURITY_INFORMATION si = OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION)
{
    QByteArray copy = sd;
    LPWSTR s = nullptr;
    if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(copy.data(), SDDL_REVISION_1, si, &s, nullptr)) return {};
    const QString out = QString::fromWCharArray(s);
    LocalFree(s);
    return out;
}

// Owner and DACL of a path (not following a final link), as SDDL.
inline QString sddlOf(const QString& path, SECURITY_INFORMATION si = OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION)
{
    HANDLE h = CreateFileW(W(path), READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return QStringLiteral("<cannot open: %1>").arg(GetLastError());
    PSECURITY_DESCRIPTOR psd = nullptr;
    QString out;
    if (GetSecurityInfo(h, SE_FILE_OBJECT, si, nullptr, nullptr, nullptr, nullptr, &psd) == ERROR_SUCCESS) {
        LPWSTR s = nullptr;
        if (ConvertSecurityDescriptorToStringSecurityDescriptorW(psd, SDDL_REVISION_1, si, &s, nullptr)) {
            out = QString::fromWCharArray(s);
            LocalFree(s);
        }
        LocalFree(psd);
    }
    CloseHandle(h);
    return out;
}

// The owner of a path as a full SID string (SDDL would print aliases such as "LA" or "BA").
inline QString ownerSidOf(const QString& path)
{
    HANDLE h = CreateFileW(W(path), READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return QStringLiteral("<cannot open: %1>").arg(GetLastError());
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR psd = nullptr;
    QString out;
    if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &psd) == ERROR_SUCCESS) {
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW(owner, &s)) {
            out = QString::fromWCharArray(s);
            LocalFree(s);
        }
        LocalFree(psd);
    }
    CloseHandle(h);
    return out;
}

// Replaces owner and/or DACL of a path from SDDL (handle-based, no propagation).
inline bool setSddl(const QString& path, const QString& s, SECURITY_INFORMATION si)
{
    HANDLE h = CreateFileW(W(path), READ_CONTROL | WRITE_DAC | WRITE_OWNER, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    QByteArray sd = sddl(s);
    const BOOL ok = !sd.isEmpty() && SetKernelObjectSecurity(h, si, sd.data());
    CloseHandle(h);
    return ok;
}

inline bool writeFile(const QString& path, const QByteArray& bytes, const QString& sddlString = {})
{
    QByteArray sd = sddlString.isEmpty() ? QByteArray() : sddl(sddlString);
    SECURITY_ATTRIBUTES sa{sizeof sa, sd.isEmpty() ? nullptr : sd.data(), FALSE};
    HANDLE h = CreateFileW(W(path), GENERIC_WRITE, 0, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const BOOL ok = WriteFile(h, bytes.constData(), DWORD(bytes.size()), &w, nullptr);
    CloseHandle(h);
    return ok && w == DWORD(bytes.size());
}

inline std::optional<QByteArray> readFile(const QString& path)
{
    HANDLE h = CreateFileW(W(path), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    QByteArray out(1 << 16, '\0');
    DWORD got = 0;
    const BOOL ok = ReadFile(h, out.data(), DWORD(out.size()), &got, nullptr);
    CloseHandle(h);
    if (!ok) return std::nullopt;
    out.resize(int(got));
    return out;
}

inline bool isReparse(const QString& path)
{
    const DWORD a = GetFileAttributesW(W(path));
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

// A junction (mount point) at `link` (created as an empty folder) to the folder `target`.
inline bool makeJunction(const QString& link, const QString& target)
{
    if (!CreateDirectoryW(W(link), nullptr)) return false;
    HANDLE h = CreateFileW(W(link), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const std::wstring sub = L"\\??\\" + nativePath(target).toStdWString();
    const std::wstring print = nativePath(target).toStdWString();
    std::vector<BYTE> buf(16 + (sub.size() + print.size() + 2) * sizeof(wchar_t), 0);
    auto* hdr = reinterpret_cast<DWORD*>(buf.data());
    hdr[0] = IO_REPARSE_TAG_MOUNT_POINT;
    auto* u = reinterpret_cast<USHORT*>(buf.data() + 8);
    const USHORT sl = USHORT(sub.size() * sizeof(wchar_t)), pl = USHORT(print.size() * sizeof(wchar_t));
    u[0] = 0;
    u[1] = sl;
    u[2] = USHORT(sl + sizeof(wchar_t));
    u[3] = pl;
    memcpy(buf.data() + 16, sub.c_str(), sl);
    memcpy(buf.data() + 16 + sl + sizeof(wchar_t), print.c_str(), pl);
    const USHORT dataLength = USHORT(8 + sl + sizeof(wchar_t) + pl + sizeof(wchar_t));
    reinterpret_cast<USHORT*>(buf.data())[2] = dataLength;
    DWORD br = 0;
    const BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), DWORD(8 + dataLength), nullptr, 0, &br, nullptr);
    CloseHandle(h);
    return ok;
}

inline bool makeSymlink(const QString& link, const QString& target, bool directory)
{
    return CreateSymbolicLinkW(W(link), W(target), directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
}

// Enables a privilege on the process token for this scope (the harness plants owners, master §8.1).
class Privilege {
public:
    explicit Privilege(const wchar_t* name)
    {
        HANDLE t = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) return;
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid);
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        m_ok = AdjustTokenPrivileges(t, FALSE, &tp, sizeof m_prev, &m_prev, &m_prevLen) && GetLastError() == ERROR_SUCCESS;
        CloseHandle(t);
    }
    ~Privilege()
    {
        HANDLE t = nullptr;
        if (m_ok && OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &t)) {
            AdjustTokenPrivileges(t, FALSE, &m_prev, 0, nullptr, nullptr);
            CloseHandle(t);
        }
    }
    bool ok() const { return m_ok; }

private:
    bool m_ok = false;
    TOKEN_PRIVILEGES m_prev{};
    DWORD m_prevLen = 0;
};

inline QString sidOfToken(HANDLE token)
{
    DWORD len = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    std::vector<char> buf(len ? len : 1);
    QString out;
    if (GetTokenInformation(token, TokenUser, buf.data(), len, &len)) {
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s)) {
            out = QString::fromWCharArray(s);
            LocalFree(s);
        }
    }
    return out;
}

// "asbtest-<8 hex>": 16 characters (a SAM account name allows 20), random per call, clearly a test account.
inline QString freshAccountName()
{
    return QString::fromLatin1(names::kAccountPrefix) + QStringLiteral("%1").arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
}

// A local account THIS object created, for its lifetime, logged on in-process (network logon: a standard user
// gets a standard token; an administrator gets a full administrator token). These rows also run on real
// machines, so an account that already exists is never adopted: no password reset, no group
// change, no deletion. Only an account this object created is put in Administrators and deleted.
//  - LocalAccount(administrator): a fresh asbtest-<hex> name; a collision is retried with another name.
//  - LocalAccount(name, administrator): exactly that name; fails (error(), no token) if it exists.
class LocalAccount {
public:
    explicit LocalAccount(bool administrator)
    {
        for (int attempt = 0; attempt < 5 && !m_created; ++attempt) {
            m_name = freshAccountName();
            if (!create() && m_status != NERR_UserExists) return;
        }
        if (m_created) setUp(administrator);
    }
    LocalAccount(const QString& name, bool administrator) : m_name(name)
    {
        if (create()) setUp(administrator);
    }
    ~LocalAccount()
    {
        if (m_token) CloseHandle(m_token);
        if (m_created) NetUserDel(nullptr, W(m_name));   // ours only: never an account that was already there
    }
    LocalAccount(const LocalAccount&) = delete;
    LocalAccount& operator=(const LocalAccount&) = delete;
    HANDLE token() const { return m_token; }
    QString sid() const { return m_sid; }
    QString name() const { return m_name; }
    // For CreateProcessWithLogonW (tst_GuiNative::standardUserWindow): passed in memory, never on a command line.
    QString password() const { return m_password; }
    bool created() const { return m_created; }
    QString error() const { return m_error; }

private:
    // NetUserAdd, nothing else: success is the only way this object comes to own an account.
    bool create()
    {
        m_password = QStringLiteral("Asb-%1-x9!Q").arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
        USER_INFO_1 ui{};
        ui.usri1_name = const_cast<LPWSTR>(W(m_name));
        ui.usri1_password = const_cast<LPWSTR>(W(m_password));
        ui.usri1_priv = USER_PRIV_USER;
        ui.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
        DWORD perr = 0;
        m_status = NetUserAdd(nullptr, 1, reinterpret_cast<LPBYTE>(&ui), &perr);
        m_created = m_status == NERR_Success;
        if (!m_created)
            m_error = m_status == NERR_UserExists ? QStringLiteral("%1 already exists; it is not used or changed").arg(m_name)
                                                  : QStringLiteral("NetUserAdd %1").arg(m_status);
        return m_created;
    }
    void setUp(bool administrator)
    {
        m_error.clear();
        if (administrator) {
            LOCALGROUP_MEMBERS_INFO_3 gm{const_cast<LPWSTR>(W(m_name))};
            const NET_API_STATUS g = NetLocalGroupAddMembers(nullptr, L"Administrators", 3, reinterpret_cast<LPBYTE>(&gm), 1);
            if (g != NERR_Success) {
                m_error = QStringLiteral("NetLocalGroupAddMembers %1").arg(g);
                return;
            }
        }
        if (!LogonUserW(W(m_name), L".", W(m_password), LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, &m_token)) {
            m_error = QStringLiteral("LogonUserW %1").arg(GetLastError());
            m_token = nullptr;
            return;
        }
        m_sid = sidOfToken(m_token);
    }

    QString m_name;
    QString m_password;
    NET_API_STATUS m_status = 0;
    bool m_created = false;
    HANDLE m_token = nullptr;
    QString m_sid;
    QString m_error;
};

// Impersonates a token on this thread for the scope.
class Impersonate {
public:
    explicit Impersonate(HANDLE token) : m_ok(token && ImpersonateLoggedOnUser(token)) {}
    ~Impersonate()
    {
        if (m_ok) RevertToSelf();
    }
    bool ok() const { return m_ok; }

private:
    bool m_ok;
};

// The object-creator owner policy, exactly: a copy of `token` (the process token when null) whose default
// owner is the user itself instead of BUILTIN\Administrators. Files created without an explicit owner under it
// are owned by the user, which another administrator's (or the service's) trust check would not accept.
inline HANDLE objectCreatorToken(HANDLE token = nullptr)
{
    HANDLE base = token;
    if (!base && !OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &base)) return nullptr;
    HANDLE dup = nullptr;
    DuplicateTokenEx(base, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenImpersonation, &dup);
    if (!token) CloseHandle(base);
    if (!dup) return nullptr;
    DWORD len = 0;
    GetTokenInformation(dup, TokenUser, nullptr, 0, &len);
    std::vector<char> buf(len ? len : 1);
    if (!GetTokenInformation(dup, TokenUser, buf.data(), len, &len)) {
        CloseHandle(dup);
        return nullptr;
    }
    TOKEN_OWNER owner{reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid};
    if (!SetTokenInformation(dup, TokenOwner, &owner, sizeof owner)) {
        CloseHandle(dup);
        return nullptr;
    }
    return dup;
}

}  // namespace asb::test::native
