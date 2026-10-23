#include "platform/win/WindowsPlatform.h"

#include "core/StartToken.h"
#include "platform/win/CimFirewall.h"
#include "platform/win/DataFolderStatusReader.h"
#include "platform/win/EngineLock.h"
#include "platform/win/FileApplyLock.h"
#include "platform/win/InstallBinary.h"
#include "platform/win/NativeSeams.h"
#include "platform/win/WbemSession.h"
#include "platform/win/WinFileIo.h"
#include "platform/win/WindowsDataFolder.h"
#include "platform/win/WindowsService.h"
#include "platform/win/UninstallEntry.h"
#include "core/BuildInfo.h"

#include <QFile>
#include <QRandomGenerator>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <string>

namespace asb::win {
namespace {

// WS-16: the Apps & features entry's registry calls (HKLM, 64-bit view), as thin pass-throughs.
class NativeRegistry final : public IRegistry {
public:
    quint32 writeKey(const QString& subkey, const QList<Value>& values) override
    {
        HKEY k = nullptr;
        const std::wstring sk = subkey.toStdWString();
        // RegFlushKey needs KEY_QUERY_VALUE on the handle; without it the flush is refused and a
        // written entry is reported as not written.
        LONG e = RegCreateKeyExW(HKEY_LOCAL_MACHINE, sk.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                 KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_WOW64_64KEY, nullptr, &k, nullptr);
        if (e != ERROR_SUCCESS) return quint32(e);
        for (const Value& v : values) {
            const std::wstring name = v.name.toStdWString();
            if (v.type == Value::Type::Sz) {
                const std::wstring s = v.sz.toStdWString();
                e = RegSetValueExW(k, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(s.c_str()),
                                   DWORD((s.size() + 1) * sizeof(wchar_t)));
            } else {
                const DWORD d = v.dword;
                e = RegSetValueExW(k, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&d), sizeof d);
            }
            if (e != ERROR_SUCCESS) break;
        }
        if (e == ERROR_SUCCESS) e = RegFlushKey(k);
        RegCloseKey(k);
        return quint32(e);
    }
    bool keyExists(const QString& subkey) override
    {
        HKEY k = nullptr;
        const std::wstring sk = subkey.toStdWString();
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sk.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return false;
        RegCloseKey(k);
        return true;
    }
    quint32 deleteKey(const QString& subkey) override
    {
        const std::wstring sk = subkey.toStdWString();
        const LONG e = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, sk.c_str(), KEY_WOW64_64KEY, 0);
        if (e == ERROR_SUCCESS) RegFlushKey(HKEY_LOCAL_MACHINE);
        return quint32(e);
    }
};

QString lastErrorText(DWORD code)
{
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return fileNotFoundError();
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    QString text = n ? QString::fromWCharArray(buf, int(n)).trimmed() : QString();
    if (buf) LocalFree(buf);
    return QStringLiteral("%1 (%2)").arg(text.isEmpty() ? QStringLiteral("Windows error") : text).arg(code);
}

std::wstring wide(const QString& s)
{
    return s.toStdWString();
}

// The file identity (volume serial + 128-bit file id) of a path, without following a final reparse point.
bool fileIdentity(const std::wstring& path, FILE_ID_INFO* id)
{
    const HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = GetFileInformationByHandleEx(h, FileIdInfo, id, sizeof *id) != 0;
    CloseHandle(h);
    return ok;
}

WindowsService::Lifecycle nativeLifecycle()
{
    WindowsService::Lifecycle f;
    f.isOwnImage = [](const QString& path) {
        std::wstring self(32768, L'\0');
        const DWORD n = GetModuleFileNameW(nullptr, self.data(), DWORD(self.size()));
        if (n == 0 || n >= self.size()) return false;
        self.resize(n);
        FILE_ID_INFO a{}, b{};
        return fileIdentity(self, &a) && fileIdentity(wide(path), &b) && a.VolumeSerialNumber == b.VolumeSerialNumber &&
               std::memcmp(&a.FileId, &b.FileId, sizeof a.FileId) == 0;
    };
    f.rename = [](const QString& from, const QString& to, QString* err) {
        if (MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_WRITE_THROUGH)) return true;
        *err = lastErrorText(GetLastError());
        return false;
    };
    f.deleteAtRestart = [](const QString& path, QString* err) {
        if (MoveFileExW(wide(path).c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) return true;
        *err = lastErrorText(GetLastError());
        return false;
    };
    f.removeDirectory = [](const QString& path, QString* err) {
        if (RemoveDirectoryW(wide(path).c_str())) return true;
        *err = lastErrorText(GetLastError());
        return false;
    };
#ifdef ASB_TEST_SEAMS
    f.injectedRemovalFailure = [] {
        return qEnvironmentVariable("ASB_TEST_FAIL_PROGRAM_REMOVAL") == QLatin1String("1")
                   ? QStringLiteral("in use (ASB_TEST_FAIL_PROGRAM_REMOVAL)")
                   : QString();
    };
#endif
    f.randomSuffix = [] {
        return QStringLiteral("%1%2")
            .arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'))
            .arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
    };
    f.today = [] { return QDate::currentDate(); };
    return f;
}

}  // namespace

struct WindowsPlatform::Impl {
    Impl(const WindowsPaths& p, Options o)
        : paths(p), nt(nativeSeams()), folder(nt, p.dataFolder), io(nt, CreateSecurity::dataFolder()),
          applyLock(nt, p.lockDirectory, systemClock(), o.lockReport),
          engineLock(nt, p.lockDirectory, LockRole::Applier, systemClock(), o.lockReport), token(folder, io),
          statusFolder(nt, p.dataFolder), status(statusFolder), entry(registry),
          service(nativeScmSeams(),
                  WindowsService::Install{p.lockDirectory, o.expectedHeadlessSha256, o.image, &engineLock.io(),
                                          [this]() -> const DirHandle& { return engineLock.dir(); }}),
          firewall(cim, service.installedExe()),
          platform{folder, service, firewall, applyLock, engineLock, token, status, io}
    {
        WindowsService::Lifecycle l = nativeLifecycle();
        l.children = [this] { return engineLock.children(); };
        l.entry = &entry;
        l.version = QString::fromLatin1(core::version().data(), qsizetype(core::version().size()));
        service.setLifecycle(std::move(l));
    }
    WindowsPaths paths;
    NtSeams& nt;
    WindowsDataFolder folder;
    WinFileIo io;
    FileApplyLock applyLock;
    EngineLock engineLock;
    FileStartToken token;
    WindowsDataFolder statusFolder;
    DataFolderStatusReader status;
    NativeRegistry registry;
    UninstallEntry entry;
    WindowsService service;
    WbemSession cim;
    CimFirewall firewall;
    Platform platform;
};

std::unique_ptr<WindowsPlatform> WindowsPlatform::create(Options o, QString* error)
{
    const std::optional<WindowsPaths> p = windowsPaths(error);
    if (!p) return nullptr;
    return std::unique_ptr<WindowsPlatform>(new WindowsPlatform(std::make_unique<Impl>(*p, std::move(o))));
}

WindowsPlatform::WindowsPlatform(std::unique_ptr<Impl> impl) : d(std::move(impl)) {}
WindowsPlatform::~WindowsPlatform() = default;

Platform& WindowsPlatform::platform() { return d->platform; }
WindowsService& WindowsPlatform::service() { return d->service; }
QString WindowsPlatform::dataFolder() const { return d->paths.dataFolder; }
QString WindowsPlatform::lockDirectory() const { return d->paths.lockDirectory; }
QString WindowsPlatform::installedExe() const { return d->service.installedExe(); }

std::optional<QByteArray> readWholeFile(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return std::nullopt;
    }
    if (f.size() > InstallBinary::kMaxImageBytes) {
        if (error) *error = QStringLiteral("%1 is too large").arg(path);
        return std::nullopt;
    }
    QByteArray b = f.readAll();
    if (b.size() != f.size()) {
        if (error) *error = QStringLiteral("could not read all of %1").arg(path);
        return std::nullopt;
    }
    return b;
}

}  // namespace asb::win
