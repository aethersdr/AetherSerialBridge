#include "HandleProbe.h"

#include <QDir>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace asb::test::handles {
namespace {

using Status = LONG;
constexpr ULONG kSystemExtendedHandleInformation = 64;
constexpr ULONG kObjectNameInformation = 1;
constexpr ULONG kObjectTypeInformation = 2;
constexpr Status kInfoLengthMismatch = static_cast<Status>(0xC0000004L);

// SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX and SYSTEM_HANDLE_INFORMATION_EX (not in the SDK headers).
struct HandleEntryEx {
    PVOID object;
    ULONG_PTR uniqueProcessId;
    ULONG_PTR handleValue;
    ULONG grantedAccess;
    USHORT creatorBackTraceIndex;
    USHORT objectTypeIndex;
    ULONG handleAttributes;
    ULONG reserved;
};
struct HandleInfoEx {
    ULONG_PTR numberOfHandles;
    ULONG_PTR reserved;
    HandleEntryEx handles[1];
};
// OBJECT_NAME_INFORMATION is a UNICODE_STRING; OBJECT_TYPE_INFORMATION starts with one (the type's name).
struct CountedString {
    USHORT length;
    USHORT maximumLength;
    PWSTR buffer;
};

using QuerySystemInformationFn = Status(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using QueryObjectFn = Status(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

QuerySystemInformationFn querySystemInformation()
{
    return reinterpret_cast<QuerySystemInformationFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
}

QueryObjectFn queryObject()
{
    return reinterpret_cast<QueryObjectFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject")));
}

QString counted(const CountedString& s)
{
    if (!s.buffer || s.length == 0) return {};
    return QString::fromWCharArray(s.buffer, int(s.length / sizeof(wchar_t)));
}

QString typeName(QueryObjectFn q, HANDLE h)
{
    std::vector<char> buf(4096);
    ULONG len = 0;
    if (q(h, kObjectTypeInformation, buf.data(), ULONG(buf.size()), &len) < 0) return {};
    return counted(*reinterpret_cast<const CountedString*>(buf.data()));
}

// The handle's path: the DOS form ("C:\..."), else the NT form ("\Device\...").
QString nameOf(QueryObjectFn q, HANDLE h)
{
    std::vector<wchar_t> dos(32768);
    const DWORD n = GetFinalPathNameByHandleW(h, dos.data(), DWORD(dos.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n > 0 && n < dos.size()) {
        QString p = QString::fromWCharArray(dos.data(), int(n));
        if (p.startsWith(QLatin1String("\\\\?\\UNC\\"))) p = QStringLiteral("\\\\") + p.mid(8);
        else if (p.startsWith(QLatin1String("\\\\?\\"))) p = p.mid(4);
        return p;
    }
    std::vector<char> buf(65536);
    ULONG len = 0;
    if (q(h, kObjectNameInformation, buf.data(), ULONG(buf.size()), &len) < 0) return {};
    return counted(*reinterpret_cast<const CountedString*>(buf.data()));
}

// "C:\Program Files\X" -> "\Device\HarddiskVolumeN\Program Files\X" (empty when the drive has no device name).
QString ntForm(const QString& dos)
{
    if (dos.size() < 2 || dos.at(1) != QLatin1Char(':')) return {};
    const std::wstring drive = dos.left(2).toStdWString();
    wchar_t target[1024] = {};
    if (!QueryDosDeviceW(drive.c_str(), target, 1023)) return {};
    return QString::fromWCharArray(target) + dos.mid(2);
}

bool isUnder(const QString& path, const QString& root)
{
    if (root.isEmpty() || path.isEmpty()) return false;
    return path.compare(root, Qt::CaseInsensitive) == 0 || path.startsWith(root + QLatin1Char('\\'), Qt::CaseInsensitive);
}

// What the helper thread names; shared with it, so a hung query can be left behind safely.
struct Naming {
    std::vector<HANDLE> handles;
    std::vector<QString> names;
    std::atomic<size_t> done{0};
    QueryObjectFn q = nullptr;
};

}  // namespace

QString Probe::describe() const
{
    if (under.isEmpty()) return QStringLiteral("none");
    QStringList l;
    for (const Held& h : under) l << QStringLiteral("handle 0x%1 access 0x%2 %3").arg(h.handle, 0, 16).arg(h.access, 0, 16).arg(h.path);
    return l.join(QLatin1Char('\n'));
}

QString Probe::describeAll() const
{
    QStringList l;
    for (const Held& h : files) l << QStringLiteral("handle 0x%1 access 0x%2 %3").arg(h.handle, 0, 16).arg(h.access, 0, 16).arg(h.path);
    return l.isEmpty() ? QStringLiteral("(no disk-file handle)") : l.join(QLatin1Char('\n'));
}

Probe probe(quint32 pid, const QStringList& roots, int perHandleMs)
{
    Probe r;
    const QuerySystemInformationFn qsi = querySystemInformation();
    const QueryObjectFn qo = queryObject();
    if (!qsi || !qo) {
        r.error = QStringLiteral("ntdll has no NtQuerySystemInformation / NtQueryObject");
        return r;
    }
    // Every handle on the machine; the buffer grows until it fits.
    std::vector<char> buf(size_t(8) << 20);
    Status st = 0;
    for (int i = 0; i < 8; ++i) {
        ULONG need = 0;
        st = qsi(kSystemExtendedHandleInformation, buf.data(), ULONG(buf.size()), &need);
        if (st != kInfoLengthMismatch) break;
        buf.resize((std::max<size_t>)(buf.size() * 2, size_t(need) + (size_t(1) << 20)));
    }
    if (st < 0) {
        r.error = QStringLiteral("NtQuerySystemInformation(SystemExtendedHandleInformation) 0x%1").arg(quint32(st), 8, 16, QLatin1Char('0'));
        return r;
    }
    HANDLE process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
    if (!process) {
        r.error = QStringLiteral("OpenProcess(PROCESS_DUP_HANDLE, %1) %2").arg(pid).arg(GetLastError());
        return r;
    }
    const auto* info = reinterpret_cast<const HandleInfoEx*>(buf.data());
    auto naming = std::make_shared<Naming>();
    naming->q = qo;
    std::vector<Held> held;
    int notDuplicated = 0;
    for (ULONG_PTR i = 0; i < info->numberOfHandles; ++i) {
        const HandleEntryEx& e = info->handles[i];
        if (e.uniqueProcessId != ULONG_PTR(pid)) continue;
        HANDLE dup = nullptr;
        if (!DuplicateHandle(process, reinterpret_cast<HANDLE>(e.handleValue), GetCurrentProcess(), &dup, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            ++notDuplicated;
            continue;
        }
        // Files only, and of those only disk files: a name query on a pipe or a device can block.
        if (typeName(qo, dup) != QLatin1String("File") || GetFileType(dup) != FILE_TYPE_DISK) {
            CloseHandle(dup);
            continue;
        }
        Held h;
        h.handle = quint64(e.handleValue);
        h.access = quint32(e.grantedAccess);
        held.push_back(h);
        naming->handles.push_back(dup);
    }
    CloseHandle(process);
    naming->names.resize(naming->handles.size());

    // The names, on a helper thread: a query that does not return within perHandleMs fails the probe.
    std::thread worker([naming] {
        for (size_t i = 0; i < naming->handles.size(); ++i) {
            naming->names[i] = nameOf(naming->q, naming->handles[i]);
            naming->done = i + 1;
        }
    });
    size_t seen = 0;
    auto lastProgress = std::chrono::steady_clock::now();
    while (naming->done < naming->handles.size()) {
        const size_t now = naming->done;
        if (now != seen) {
            seen = now;
            lastProgress = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - lastProgress > std::chrono::milliseconds(perHandleMs)) {
            r.error = QStringLiteral("the name query of handle 0x%1 did not return within %2 ms")
                          .arg(held[now].handle, 0, 16).arg(perHandleMs);
            worker.detach();   // it still uses its handles: they are left open with it
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    worker.join();
    for (HANDLE h : naming->handles) CloseHandle(h);

    QStringList dosRoots, ntRoots;
    for (const QString& root : roots) {
        QString d = QDir::toNativeSeparators(root);
        while (d.endsWith(QLatin1Char('\\'))) d.chop(1);
        dosRoots << d;
        ntRoots << ntForm(d);
    }
    for (size_t i = 0; i < held.size(); ++i) {
        held[i].path = naming->names[i].isEmpty() ? QStringLiteral("(no name)") : naming->names[i];
        r.files << held[i];
        for (int k = 0; k < dosRoots.size(); ++k) {
            if (isUnder(naming->names[i], dosRoots[k]) || isUnder(naming->names[i], ntRoots[k])) {
                r.under << held[i];
                break;
            }
        }
    }
    if (notDuplicated > 0)
        r.files << Held{0, 0, QStringLiteral("(%1 handle(s) of the process could not be duplicated)").arg(notDuplicated)};
    r.ok = true;
    return r;
}

}  // namespace asb::test::handles
