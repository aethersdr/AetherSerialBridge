#include "ServiceNativeSupport.h"

#include "platform/win/InstallBinary.h"
#include "platform/win/ServiceStatus.h"
#include "platform/win/UninstallEntry.h"
#include "platform/win/WindowsService.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QTcpSocket>
#include <QThread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace asb::test::svc {

namespace {
QString g_exe;

std::wstring W(const QString& s) { return s.toStdWString(); }

SC_HANDLE openService(DWORD access)
{
    SC_HANDLE m = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!m) return nullptr;
    SC_HANDLE s = OpenServiceW(m, L"AetherSerialBridge", access);
    CloseServiceHandle(m);
    return s;
}
}  // namespace

QString headlessExe()
{
    if (!g_exe.isEmpty()) return g_exe;
    return qEnvironmentVariable("ASB_HEADLESS_EXE");
}

void setHeadlessExe(const QString& p) { g_exe = p; }

std::unique_ptr<win::WindowsPlatform> platform(QString* error)
{
    const QString exe = headlessExe();
    QString err;
    const std::optional<QByteArray> image = win::readWholeFile(exe, &err);
    if (!image) {
        if (error) *error = QStringLiteral("cannot read %1: %2").arg(exe, err);
        return nullptr;
    }
    win::WindowsPlatform::Options o;
    o.expectedHeadlessSha256 = win::InstallBinary::sha256Hex(*image);
    o.image = [exe](QString* e) { return win::readWholeFile(exe, e); };
    return win::WindowsPlatform::create(std::move(o), error);
}

QStringList deployDevRuntime(const QString& lockDirectory, const QString& runtimeDir)
{
    QStringList copied;
    // The release executables are static (WS-13): the release workflow runs these rows against them with
    // ASB_HEADLESS_STATIC=1, and nothing may be copied beside the installed exe (the deployed-tree check).
    if (qEnvironmentVariableIntValue("ASB_HEADLESS_STATIC") == 1) return copied;
    CreateDirectoryW(W(lockDirectory).c_str(), nullptr);   // inherits the Program Files DACL; owner Administrators
    QStringList sources;
    if (runtimeDir.isEmpty()) {
        const QString bin = QLibraryInfo::path(QLibraryInfo::BinariesPath);
        for (const char* n : {"Qt6Core.dll", "Qt6Network.dll", "Qt6SerialPort.dll"}) sources << bin + QLatin1Char('/') + QLatin1String(n);
    } else {
        for (const QFileInfo& f : QDir(runtimeDir).entryInfoList({QStringLiteral("*.dll")}, QDir::Files))
            sources << f.absoluteFilePath();
    }
    for (const QString& src : sources) {
        const QString dst = lockDirectory + QLatin1Char('\\') + QFileInfo(src).fileName();
        if (CopyFileW(W(QDir::toNativeSeparators(src)).c_str(), W(dst).c_str(), FALSE)) copied << QFileInfo(src).fileName();
    }
    return copied;
}

QByteArray configJson(int tcpPort, const QString& allowed, const QString& com)
{
    return QStringLiteral(R"({
  "version": 1,
  "ports": [
    {
      "enabled": true,
      "name": "Native test",
      "comPort": "%1",
      "baudRate": 38400,
      "tcpPort": %2,
      "bindAddress": "0.0.0.0",
      "allowedClients": "%3"
    }
  ]
}
)")
        .arg(com)
        .arg(tcpPort)
        .arg(allowed)
        .toUtf8();
}

SaveRequest request(const QByteArray& config)
{
    SaveRequest r;
    r.config = config;
    r.source.kind = Source::New;
    r.supersedePending = true;
    return r;
}

Result install(win::WindowsPlatform& p, const QByteArray& config)
{
    return Applier(p.platform()).run(request(config), /*install=*/true);
}
Result apply(win::WindowsPlatform& p, const QByteArray& config) { return Applier(p.platform()).run(request(config), false); }
Result start(win::WindowsPlatform& p) { return Applier(p.platform()).start(); }
Result stop(win::WindowsPlatform& p) { return Applier(p.platform()).stop(); }
Result uninstall(win::WindowsPlatform& p) { return Applier(p.platform()).saveAndRemove(std::nullopt); }

QString describe(const Result& r)
{
    return QStringLiteral("%1 [%2] step=%3: %4 | %5 | files: %6")
        .arg(enumName(r.outcome), enumName(r.persisted), r.step, r.message, r.postcondition, r.fileChanges.join(QStringLiteral("; ")));
}

bool setServiceEnvironment(const QStringList& kv)
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\AetherSerialBridge", 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return false;
    LONG rc;
    if (kv.isEmpty()) {
        rc = RegDeleteValueW(k, L"Environment");
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    } else {
        std::wstring block;
        for (const QString& s : kv) {
            block += W(s);
            block.push_back(L'\0');
        }
        block.push_back(L'\0');
        rc = RegSetValueExW(k, L"Environment", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(block.data()),
                            DWORD(block.size() * sizeof(wchar_t)));
    }
    RegFlushKey(k);
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

ScmState query()
{
    ScmState s;
    SC_HANDLE h = openService(SERVICE_QUERY_STATUS);
    if (!h) return s;
    s.exists = true;
    SERVICE_STATUS_PROCESS sp{};
    DWORD n = 0;
    if (QueryServiceStatusEx(h, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&sp), sizeof sp, &n)) {
        s.state = sp.dwCurrentState;
        s.pid = sp.dwProcessId;
        s.win32ExitCode = sp.dwWin32ExitCode;
        s.specificExitCode = sp.dwServiceSpecificExitCode;
        s.checkPoint = sp.dwCheckPoint;
    }
    CloseServiceHandle(h);
    return s;
}

bool waitState(quint32 state, int ms, ScmState* last)
{
    QElapsedTimer t;
    t.start();
    ScmState s;
    do {
        s = query();
        if (s.state == state && (state != SERVICE_RUNNING || s.pid != 0)) break;
        Sleep(50);
    } while (t.elapsed() < ms);
    if (last) *last = s;
    return s.state == state;
}

bool waitDeleted(int ms)
{
    QElapsedTimer t;
    t.start();
    while (query().exists) {
        if (t.elapsed() >= ms) return false;
        Sleep(100);
    }
    return true;
}

quint32 systemLogMark()
{
    HANDLE log = OpenEventLogW(nullptr, L"System");
    if (!log) return 0;
    DWORD oldest = 0, count = 0;
    const bool ok = GetOldestEventLogRecord(log, &oldest) && GetNumberOfEventLogRecords(log, &count);
    CloseEventLog(log);
    return ok && count ? oldest + count - 1 : 0;
}

QStringList scmEventsAfter(quint32 mark, const QList<quint32>& ids, const QString& displayName, bool* ok)
{
    QStringList out;
    if (ok) *ok = false;
    HANDLE log = OpenEventLogW(nullptr, L"System");
    if (!log) return out;
    std::vector<BYTE> buf(64 * 1024);
    bool done = false;
    while (!done) {
        DWORD got = 0, need = 0;
        // Newest first; stop at the first record at or before the mark.
        if (!ReadEventLogW(log, EVENTLOG_SEQUENTIAL_READ | EVENTLOG_BACKWARDS_READ, 0, buf.data(), DWORD(buf.size()), &got, &need)) {
            const DWORD e = GetLastError();
            if (e == ERROR_INSUFFICIENT_BUFFER) {
                buf.resize(need);
                continue;
            }
            if (e == ERROR_HANDLE_EOF) break;
            CloseEventLog(log);
            return out;   // unreadable: *ok stays false
        }
        for (DWORD at = 0; at < got;) {
            const auto* rec = reinterpret_cast<const EVENTLOGRECORD*>(buf.data() + at);
            at += rec->Length;
            if (rec->RecordNumber <= mark) {
                done = true;
                break;
            }
            const auto* source = reinterpret_cast<const wchar_t*>(reinterpret_cast<const BYTE*>(rec) + sizeof(EVENTLOGRECORD));
            if (QString::fromWCharArray(source) != QLatin1String("Service Control Manager")) continue;
            const quint32 id = rec->EventID & 0xFFFF;
            if (!ids.contains(id)) continue;
            QStringList strings;
            const auto* p = reinterpret_cast<const wchar_t*>(reinterpret_cast<const BYTE*>(rec) + rec->StringOffset);
            for (WORD i = 0; i < rec->NumStrings; ++i) {
                const QString one = QString::fromWCharArray(p);
                strings << one;
                p += one.size() + 1;
            }
            if (strings.isEmpty() || strings.front() != displayName) continue;
            out << QStringLiteral("%1 (record %2): %3").arg(id).arg(rec->RecordNumber).arg(strings.join(QStringLiteral(" | ")));
        }
    }
    CloseEventLog(log);
    if (ok) *ok = true;
    return out;
}

quint32 startRaw()
{
    SC_HANDLE h = openService(SERVICE_START);
    if (!h) return GetLastError();
    const quint32 e = StartServiceW(h, 0, nullptr) ? 0 : GetLastError();
    CloseServiceHandle(h);
    return e;
}

quint32 stopSync()
{
    SC_HANDLE h = openService(SERVICE_STOP);
    if (!h) return GetLastError();
    SERVICE_STATUS st{};
    const quint32 e = ControlService(h, SERVICE_CONTROL_STOP, &st) ? 0 : GetLastError();
    CloseServiceHandle(h);
    return e;
}

void stopAsync()
{
    std::thread([] { stopSync(); }).detach();
}

void* openProcess(quint32 pid)
{
    if (!pid) return nullptr;
    return OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
}

bool waitExit(void* process, int ms, std::chrono::milliseconds* elapsed, quint32* exitCode)
{
    if (!process) return true;
    QElapsedTimer t;
    t.start();
    const DWORD w = WaitForSingleObject(process, DWORD(ms));
    if (elapsed) *elapsed = std::chrono::milliseconds(t.elapsed());
    if (exitCode) {
        DWORD c = 0;
        GetExitCodeProcess(process, &c);
        *exitCode = c;
    }
    return w == WAIT_OBJECT_0;
}

void closeHandle(void* h)
{
    if (h) CloseHandle(h);
}

void killInstalledProcesses(const QString& installedExe)
{
    QList<quint32> pids;
    win::nativeScmSeams().processesOfImage(installedExe, &pids);   // test cleanup: whatever could be listed
    for (quint32 pid : pids) {
        HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (!p) continue;
        TerminateProcess(p, 99);
        WaitForSingleObject(p, 5000);
        CloseHandle(p);
    }
}

bool removeServiceHard(const QString& installedExe)
{
    SC_HANDLE h = openService(SERVICE_ALL_ACCESS);
    if (h) {
        ChangeServiceConfigW(h, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr,
                             nullptr, nullptr, nullptr);
        SERVICE_STATUS st{};
        ControlService(h, SERVICE_CONTROL_STOP, &st);
        killInstalledProcesses(installedExe);
        DeleteService(h);
        CloseServiceHandle(h);
    }
    killInstalledProcesses(installedExe);
    removeAppsEntry();
    return waitDeleted(60000);
}

namespace {

std::wstring appsKey()
{
    return QString::fromLatin1(win::UninstallEntry::kKey).toStdWString();
}

constexpr wchar_t kSessionManager[] = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";
constexpr wchar_t kPendingValue[] = L"PendingFileRenameOperations";

// The strings of PendingFileRenameOperations, empty ones (an empty target = delete) included; {} when absent,
// nullopt when unreadable.
std::optional<QStringList> pendingStrings()
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSessionManager, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
        return std::nullopt;
    DWORD type = 0, bytes = 0;
    LONG e = RegQueryValueExW(k, kPendingValue, nullptr, &type, nullptr, &bytes);
    if (e == ERROR_FILE_NOT_FOUND) {
        RegCloseKey(k);
        return QStringList();
    }
    std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, L'\0');
    if (e == ERROR_SUCCESS && type == REG_MULTI_SZ) {
        bytes = DWORD(buf.size() * sizeof(wchar_t));
        e = RegQueryValueExW(k, kPendingValue, nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &bytes);
    }
    RegCloseKey(k);
    if (e != ERROR_SUCCESS || type != REG_MULTI_SZ) return std::nullopt;
    QString s = QString::fromWCharArray(buf.data(), qsizetype(bytes / sizeof(wchar_t)));
    if (s.endsWith(QChar(0))) s.chop(1);   // the list's own terminator
    QStringList parts = s.split(QChar(0));
    if (!parts.isEmpty() && parts.last().isEmpty()) parts.removeLast();   // what follows the last string's terminator
    return parts;
}

// The folder, or a path under it, after any prefix that holds no drive ("\\??\\", "!\\??\\", ...): the sweep's rule.
bool isUnder(const QString& s, const QString& folder)
{
    const QString root = QDir::toNativeSeparators(folder);
    const qsizetype at = s.indexOf(root, 0, Qt::CaseInsensitive);
    if (at < 0 || s.left(at).contains(QLatin1Char(':'))) return false;
    const qsizetype end = at + root.size();
    return end == s.size() || s.at(end) == QLatin1Char('\\');
}

}  // namespace

bool appsEntryExists()
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, appsKey().c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
        return false;
    RegCloseKey(k);
    return true;
}

QString appsEntryValue(const char* name)
{
    const std::wstring key = appsKey();
    const std::wstring value = QString::fromLatin1(name).toStdWString();
    DWORD type = 0, bytes = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), value.c_str(), RRF_RT_REG_SZ | RRF_RT_REG_DWORD, &type, nullptr,
                     &bytes) != ERROR_SUCCESS)
        return {};
    std::vector<BYTE> buf(bytes + sizeof(wchar_t), 0);
    bytes = DWORD(buf.size());
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), value.c_str(), RRF_RT_REG_SZ | RRF_RT_REG_DWORD, &type, buf.data(),
                     &bytes) != ERROR_SUCCESS)
        return {};
    if (type == REG_DWORD) {
        DWORD d = 0;
        std::memcpy(&d, buf.data(), sizeof d);
        return QString::number(d);
    }
    return QString::fromWCharArray(reinterpret_cast<const wchar_t*>(buf.data()));
}

bool removeAppsEntry()
{
    const LONG e = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, appsKey().c_str(), KEY_WOW64_64KEY, 0);
    return e == ERROR_SUCCESS || e == ERROR_FILE_NOT_FOUND;
}

QStringList restartDeletionsUnder(const QString& folder)
{
    QStringList r;
    if (const std::optional<QStringList> ops = pendingStrings())
        for (const QString& s : *ops)
            if (isUnder(s, folder)) r << s;
    return r;
}

bool dropRestartDeletionsUnder(const QString& folder)
{
    const std::optional<QStringList> ops = pendingStrings();
    if (!ops || ops->size() % 2 != 0) return false;
    QStringList keep;
    for (qsizetype i = 0; i + 1 < ops->size(); i += 2)
        if (!isUnder(ops->at(i), folder) && !isUnder(ops->at(i + 1), folder)) keep << ops->at(i) << ops->at(i + 1);
    if (keep.size() == ops->size()) return true;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSessionManager, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
        return false;
    LONG e = ERROR_SUCCESS;
    if (keep.isEmpty()) {
        e = RegDeleteValueW(k, kPendingValue);
    } else {
        std::wstring data;
        for (const QString& s : keep) {
            data += s.toStdWString();
            data.push_back(L'\0');
        }
        data.push_back(L'\0');
        e = RegSetValueExW(k, kPendingValue, 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(data.data()),
                           DWORD(data.size() * sizeof(wchar_t)));
    }
    RegCloseKey(k);
    return e == ERROR_SUCCESS;
}

bool echoRoundTrip(int port, const QByteArray& bytes, int ms, QString* why)
{
    QTcpSocket s;
    s.connectToHost(QStringLiteral("127.0.0.1"), quint16(port));
    if (!s.waitForConnected(ms)) {
        if (why) *why = QStringLiteral("connect: %1").arg(s.errorString());
        return false;
    }
    s.write(bytes);
    s.waitForBytesWritten(ms);
    QByteArray got;
    QElapsedTimer t;
    t.start();
    while (got.size() < bytes.size() && t.elapsed() < ms) {
        if (s.waitForReadyRead(200)) got += s.readAll();
    }
    if (got != bytes && why) *why = QStringLiteral("echo: got '%1'").arg(QString::fromLatin1(got.toHex()));
    s.disconnectFromHost();
    return got == bytes;
}

bool canConnect(int port, int ms)
{
    QTcpSocket s;
    s.connectToHost(QStringLiteral("127.0.0.1"), quint16(port));
    const bool ok = s.waitForConnected(ms);
    s.abort();
    return ok;
}

}  // namespace asb::test::svc
