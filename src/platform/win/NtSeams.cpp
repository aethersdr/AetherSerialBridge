// The native NtSeams (Windows only): a thin pass-through to Win32/NT. Decisions live in the portable callers.
#include "platform/win/NativeSeams.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace asb::win {
namespace {

// --- NT declarations, resolved from ntdll at run time (no DDK headers) ------------------------------
using NTSTATUS_T = LONG;
struct UnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};
struct ObjectAttributes {
    ULONG Length;
    HANDLE RootDirectory;
    UnicodeString* ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
};
struct IoStatusBlock {
    union {
        NTSTATUS_T Status;
        PVOID Pointer;
    } u;
    ULONG_PTR Information;
};
struct RenameInformationEx {
    ULONG Flags;
    HANDLE RootDirectory;
    ULONG FileNameLength;
    WCHAR FileName[1];
};
struct DispositionInfoEx {
    DWORD Flags;
};

using NtCreateFileFn = NTSTATUS_T(NTAPI*)(PHANDLE, ACCESS_MASK, ObjectAttributes*, IoStatusBlock*, PLARGE_INTEGER, ULONG,
                                           ULONG, ULONG, ULONG, PVOID, ULONG);
using NtSetInformationFileFn = NTSTATUS_T(NTAPI*)(HANDLE, IoStatusBlock*, PVOID, ULONG, ULONG);
using RtlNtStatusToDosErrorFn = ULONG(NTAPI*)(NTSTATUS_T);

constexpr ULONG kObjCaseInsensitive = 0x40;
constexpr ULONG kFileSynchronousIoNonalert = 0x20;
constexpr ULONG kFileRenameInformationEx = 65;
constexpr ULONG kRenameReplaceIfExists = 0x1;
constexpr ULONG kRenamePosixSemantics = 0x2;
constexpr DWORD kDispositionDelete = 0x1;
constexpr DWORD kDispositionPosix = 0x2;

struct Ntdll {
    NtCreateFileFn createFile = nullptr;
    NtSetInformationFileFn setInformation = nullptr;
    RtlNtStatusToDosErrorFn toDosError = nullptr;
    Ntdll()
    {
        HMODULE m = GetModuleHandleW(L"ntdll.dll");
        createFile = reinterpret_cast<NtCreateFileFn>(reinterpret_cast<void*>(GetProcAddress(m, "NtCreateFile")));
        setInformation =
            reinterpret_cast<NtSetInformationFileFn>(reinterpret_cast<void*>(GetProcAddress(m, "NtSetInformationFile")));
        toDosError = reinterpret_cast<RtlNtStatusToDosErrorFn>(reinterpret_cast<void*>(GetProcAddress(m, "RtlNtStatusToDosError")));
    }
};
const Ntdll& ntdll()
{
    static const Ntdll n;
    return n;
}

QString win32Text(DWORD e)
{
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, e, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    QString s = n ? QString::fromWCharArray(buf, int(n)).trimmed() : QString();
    if (buf) LocalFree(buf);
    return s;
}

Status fromWin32(DWORD e)
{
    Status::Kind k = Status::Kind::Other;
    switch (e) {
    case ERROR_SUCCESS: return Status::success();
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: k = Status::Kind::NotFound; break;
    case ERROR_DELETE_PENDING: k = Status::Kind::DeletePending; break;
    case ERROR_ACCESS_DENIED: k = Status::Kind::AccessDenied; break;
    case ERROR_SHARING_VIOLATION: k = Status::Kind::SharingViolation; break;
    case ERROR_LOCK_VIOLATION: k = Status::Kind::LockViolation; break;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS: k = Status::Kind::Exists; break;
    case ERROR_DIRECTORY_NOT_SUPPORTED: k = Status::Kind::IsDirectory; break;
    default: break;
    }
    return Status::fail(k, e, win32Text(e));
}

Status fromNt(NTSTATUS_T s)
{
    if (s >= 0) return Status::success();
    const quint32 u = quint32(s);
    Status::Kind k = Status::Kind::Other;
    switch (u) {
    case 0xC0000034u:   // STATUS_OBJECT_NAME_NOT_FOUND
    case 0xC000003Au:   // STATUS_OBJECT_PATH_NOT_FOUND
    case 0xC000000Fu:   // STATUS_NO_SUCH_FILE
        k = Status::Kind::NotFound;
        break;
    case 0xC0000056u: k = Status::Kind::DeletePending; break;
    case 0xC0000022u: k = Status::Kind::AccessDenied; break;
    case 0xC0000043u: k = Status::Kind::SharingViolation; break;
    case 0xC0000054u:   // STATUS_FILE_LOCK_CONFLICT
    case 0xC0000055u:   // STATUS_LOCK_NOT_GRANTED
        k = Status::Kind::LockViolation;
        break;
    case 0xC0000035u: k = Status::Kind::Exists; break;
    case 0xC00000BAu: k = Status::Kind::IsDirectory; break;
    default: break;
    }
    const ULONG dos = ntdll().toDosError ? ntdll().toDosError(s) : 0;
    return Status::fail(k, u, dos ? win32Text(dos) : QString());
}

Status lastError()
{
    return fromWin32(GetLastError());
}

HANDLE H(Handle h)
{
    return reinterpret_cast<HANDLE>(h);
}

const wchar_t* W(const QString& s)
{
    return reinterpret_cast<const wchar_t*>(s.utf16());
}

class NativeSeams final : public NtSeams {
public:
    Status createFile(const QString& path, quint32 access, quint32 share, quint32 disposition, quint32 flags,
                      const QByteArray& sd, Handle* out) override
    {
        QByteArray copy = sd;
        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), copy.isEmpty() ? nullptr : copy.data(), FALSE};
        const HANDLE h = CreateFileW(W(path), access, share, &sa, disposition, flags, nullptr);
        if (h == INVALID_HANDLE_VALUE) return lastError();
        *out = reinterpret_cast<Handle>(h);
        return Status::success();
    }

    Status openRelative(Handle dir, const QString& name, quint32 access, quint32 share, quint32 disposition, quint32 options,
                        const QByteArray& sd, Handle* out) override
    {
        if (!ntdll().createFile) return Status::fail(Status::Kind::Other, 0, QStringLiteral("NtCreateFile is unavailable"));
        std::wstring n = name.toStdWString();
        UnicodeString us{USHORT(n.size() * sizeof(wchar_t)), USHORT(n.size() * sizeof(wchar_t)), n.data()};
        QByteArray copy = sd;
        ObjectAttributes oa{sizeof(ObjectAttributes), H(dir), &us, kObjCaseInsensitive,
                            copy.isEmpty() ? nullptr : copy.data(), nullptr};   // no OBJ_INHERIT: never inheritable
        IoStatusBlock io{};
        HANDLE h = nullptr;
        const NTSTATUS_T st = ntdll().createFile(&h, access | SYNCHRONIZE, &oa, &io, nullptr, FILE_ATTRIBUTE_NORMAL, share,
                                                 disposition, options | kFileSynchronousIoNonalert, nullptr, 0);
        if (st < 0) return fromNt(st);
        *out = reinterpret_cast<Handle>(h);
        return Status::success();
    }

    Status createDirectory(const QString& path, const QByteArray& sd) override
    {
        QByteArray copy = sd;
        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), copy.isEmpty() ? nullptr : copy.data(), FALSE};
        return CreateDirectoryW(W(path), &sa) ? Status::success() : lastError();
    }

    void close(Handle h) override { CloseHandle(H(h)); }

    Status facts(Handle h, Facts* f) override
    {
        // Attributes, size and the 64-bit index work on every file system (FAT included); the 128-bit id and
        // the reparse tag are taken when the file system has them.
        BY_HANDLE_FILE_INFORMATION bi{};
        if (!GetFileInformationByHandle(H(h), &bi)) return lastError();
        f->attributes = bi.dwFileAttributes;
        f->size = qint64((quint64(bi.nFileSizeHigh) << 32) | bi.nFileSizeLow);
        FILE_ID_INFO id{};
        if (GetFileInformationByHandleEx(H(h), FileIdInfo, &id, sizeof id)) {
            f->id.volume = id.VolumeSerialNumber;
            static_assert(sizeof(id.FileId) == 16);
            memcpy(f->id.id.data(), &id.FileId, 16);
        } else {
            f->id = FileId::fromPosix(bi.dwVolumeSerialNumber, (quint64(bi.nFileIndexHigh) << 32) | bi.nFileIndexLow);
        }
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (GetFileInformationByHandleEx(H(h), FileAttributeTagInfo, &tag, sizeof tag)) {
            f->attributes = tag.FileAttributes;
            f->reparseTag = tag.ReparseTag;
        }
        return Status::success();
    }

    Status getSecurity(Handle h, quint32 info, QByteArray* out) override
    {
        PSECURITY_DESCRIPTOR psd = nullptr;
        const DWORD e = GetSecurityInfo(H(h), SE_FILE_OBJECT, info, nullptr, nullptr, nullptr, nullptr, &psd);
        if (e != ERROR_SUCCESS) return fromWin32(e);
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD rev = 0;
        GetSecurityDescriptorControl(psd, &control, &rev);
        Status st;
        if (control & SE_SELF_RELATIVE) {
            const DWORD n = GetSecurityDescriptorLength(psd);
            *out = QByteArray(static_cast<const char*>(psd), int(n));
        } else {
            DWORD n = 0;
            MakeSelfRelativeSD(psd, nullptr, &n);
            QByteArray buf(int(n), '\0');
            if (MakeSelfRelativeSD(psd, buf.data(), &n)) *out = buf;
            else st = lastError();
        }
        LocalFree(psd);
        return st;
    }

    Status setSecurity(Handle h, quint32 info, const QByteArray& sd) override
    {
        QByteArray copy = sd;
        return SetKernelObjectSecurity(H(h), info, copy.data()) ? Status::success() : lastError();
    }

    Status list(Handle dir, QStringList* names) override
    {
        std::vector<char> buf(64 * 1024);
        FILE_INFO_BY_HANDLE_CLASS cls = FileIdBothDirectoryRestartInfo;
        for (;;) {
            if (!GetFileInformationByHandleEx(H(dir), cls, buf.data(), DWORD(buf.size()))) {
                const DWORD e = GetLastError();
                if (e == ERROR_NO_MORE_FILES) return Status::success();
                return fromWin32(e);
            }
            cls = FileIdBothDirectoryInfo;
            const char* p = buf.data();
            for (;;) {
                const auto* e = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(p);
                const QString n = QString::fromWCharArray(e->FileName, int(e->FileNameLength / sizeof(wchar_t)));
                if (n != QLatin1String(".") && n != QLatin1String("..")) names->append(n);
                if (!e->NextEntryOffset) break;
                p += e->NextEntryOffset;
            }
        }
    }

    Status read(Handle h, qint64 offset, qint64 max, QByteArray* out) override
    {
        out->clear();
        while (out->size() < max) {
            const DWORD want = DWORD(std::min<qint64>(max - out->size(), 1 << 20));
            QByteArray chunk(int(want), '\0');
            OVERLAPPED ov{};
            const quint64 at = quint64(offset + out->size());
            ov.Offset = DWORD(at & 0xFFFFFFFFu);
            ov.OffsetHigh = DWORD(at >> 32);
            DWORD got = 0;
            if (!ReadFile(H(h), chunk.data(), want, &got, &ov)) {
                const DWORD e = GetLastError();
                if (e == ERROR_HANDLE_EOF) break;
                return fromWin32(e);
            }
            if (got == 0) break;
            out->append(chunk.constData(), int(got));
        }
        return Status::success();
    }

    Status write(Handle h, qint64 offset, const QByteArray& bytes) override
    {
        qint64 done = 0;
        while (done < bytes.size()) {
            OVERLAPPED ov{};
            if (offset < 0) {
                ov.Offset = 0xFFFFFFFFu;   // append
                ov.OffsetHigh = 0xFFFFFFFFu;
            } else {
                const quint64 at = quint64(offset + done);
                ov.Offset = DWORD(at & 0xFFFFFFFFu);
                ov.OffsetHigh = DWORD(at >> 32);
            }
            DWORD wrote = 0;
            if (!WriteFile(H(h), bytes.constData() + done, DWORD(bytes.size() - done), &wrote, &ov)) return lastError();
            if (wrote == 0) return Status::fail(Status::Kind::Other, 0, QStringLiteral("short write"));
            done += wrote;
        }
        return Status::success();
    }

    Status setEndOfFile(Handle h, qint64 size) override
    {
        FILE_END_OF_FILE_INFO e{};
        e.EndOfFile.QuadPart = size;
        return SetFileInformationByHandle(H(h), FileEndOfFileInfo, &e, sizeof e) ? Status::success() : lastError();
    }

    Status flush(Handle h) override { return FlushFileBuffers(H(h)) ? Status::success() : lastError(); }

    Status rename(Handle file, Handle dir, const QString& name, bool replace) override
    {
        if (!ntdll().setInformation) return Status::fail(Status::Kind::Other, 0, QStringLiteral("NtSetInformationFile is unavailable"));
        const std::wstring n = name.toStdWString();
        const size_t size = offsetof(RenameInformationEx, FileName) + n.size() * sizeof(wchar_t);
        std::vector<char> buf(size + sizeof(wchar_t), 0);
        auto* ri = reinterpret_cast<RenameInformationEx*>(buf.data());
        ri->Flags = kRenamePosixSemantics | (replace ? kRenameReplaceIfExists : 0);
        ri->RootDirectory = H(dir);
        ri->FileNameLength = ULONG(n.size() * sizeof(wchar_t));
        memcpy(ri->FileName, n.data(), n.size() * sizeof(wchar_t));
        IoStatusBlock io{};
        return fromNt(ntdll().setInformation(H(file), &io, ri, ULONG(size), kFileRenameInformationEx));
    }

    Status dispose(Handle h) override
    {
        DispositionInfoEx d{kDispositionDelete | kDispositionPosix};
        return SetFileInformationByHandle(H(h), static_cast<FILE_INFO_BY_HANDLE_CLASS>(21) /*FileDispositionInfoEx*/, &d, sizeof d)
                   ? Status::success()
                   : lastError();
    }

    Status setDeleteOnClose(Handle h, bool del, bool posix) override
    {
#ifdef ASB_TEST_SEAMS
        if (del && qEnvironmentVariable("ASB_TEST_FAIL_LOCK_DISPOSITION") == QLatin1String("1"))
            return posix ? Status::fail(Status::Kind::Other, ERROR_NOT_SUPPORTED,
                                        QStringLiteral("not supported (ASB_TEST_FAIL_LOCK_DISPOSITION)"))
                         : Status::fail(Status::Kind::AccessDenied, ERROR_ACCESS_DENIED,
                                        QStringLiteral("access denied (ASB_TEST_FAIL_LOCK_DISPOSITION)"));
#endif
        if (posix) {
            DispositionInfoEx d{del ? (kDispositionDelete | kDispositionPosix) : 0u};
            return SetFileInformationByHandle(H(h), static_cast<FILE_INFO_BY_HANDLE_CLASS>(21) /*FileDispositionInfoEx*/, &d,
                                              sizeof d)
                       ? Status::success()
                       : lastError();
        }
        FILE_DISPOSITION_INFO d{};
        d.DeleteFile = del ? BOOLEAN(TRUE) : BOOLEAN(FALSE);   // no narrowing from int (MSVC C2397)
        return SetFileInformationByHandle(H(h), FileDispositionInfo, &d, sizeof d) ? Status::success() : lastError();
    }

    Status lockExclusive(Handle h) override
    {
        OVERLAPPED ov{};
        return LockFileEx(H(h), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &ov)
                   ? Status::success()
                   : lastError();
    }

    Status volume(Handle h, Volume* v) override
    {
        wchar_t fs[MAX_PATH + 1] = {};
        if (!GetVolumeInformationByHandleW(H(h), nullptr, 0, nullptr, nullptr, nullptr, fs, MAX_PATH)) return lastError();
        v->fileSystem = QString::fromWCharArray(fs);
        FILE_REMOTE_PROTOCOL_INFO rp{};
        rp.StructureVersion = 2;
        rp.StructureSize = USHORT(sizeof rp);
        v->remote = GetFileInformationByHandleEx(H(h), FileRemoteProtocolInfo, &rp, sizeof rp) != 0;
        if (!v->remote) {
            // Class audit (review round 1): a path that cannot be read is "unknown", never "local" (the caller
            // refuses an unknown volume); a long path gets the buffer it needs.
            std::vector<wchar_t> path(1024);
            DWORD n = GetFinalPathNameByHandleW(H(h), path.data(), DWORD(path.size()), VOLUME_NAME_NT);
            if (n >= path.size()) {
                path.resize(size_t(n) + 1);
                n = GetFinalPathNameByHandleW(H(h), path.data(), DWORD(path.size()), VOLUME_NAME_NT);
            }
            if (n == 0) return lastError();
            if (n >= path.size()) return fromWin32(ERROR_INSUFFICIENT_BUFFER);
            const QString p = QString::fromWCharArray(path.data(), int(n));
            v->remote = p.startsWith(QLatin1String("\\Device\\Mup\\"), Qt::CaseInsensitive) ||
                        p.startsWith(QLatin1String("\\Device\\LanmanRedirector\\"), Qt::CaseInsensitive);
        }
        return Status::success();
    }

    bool inheritable(Handle h) override
    {
        DWORD flags = 0;
        return GetHandleInformation(H(h), &flags) && (flags & HANDLE_FLAG_INHERIT);
    }

    QString currentUserSid() override
    {
        HANDLE tok = nullptr;
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &tok) && !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
            return {};
        DWORD len = 0;
        GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
        std::vector<char> buf(len ? len : 1);
        QString out;
        if (GetTokenInformation(tok, TokenUser, buf.data(), len, &len)) {
            LPWSTR s = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s)) {
                out = QString::fromWCharArray(s);
                LocalFree(s);
            }
        }
        CloseHandle(tok);
        return out;
    }
};

}  // namespace

NtSeams& nativeSeams()
{
    static NativeSeams s;
    return s;
}

}  // namespace asb::win
