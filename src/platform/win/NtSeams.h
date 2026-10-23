#pragma once

// The Windows native seam (WS-6). Every Win32/NT call the data-folder adapter, the two locks, WinFileIo and
// the log file make goes through this interface, with the Win32/NT parameter values passed through
// unchanged, so the decisions — which access, which share mode, which disposition, which security
// descriptor, which identity is compared — are made in portable code and asserted on a recording fake on
// every OS (tests/support/FakeNtSeams). NtSeams.cpp (Windows only) is a thin pass-through to the OS.
//
// Rules of the native implementation: every handle is non-inheritable (bInheritHandle = FALSE,
// no OBJ_INHERIT); nothing creates a process; security descriptors travel as self-relative bytes
// (SecurityDescriptor.h); setSecurity is handle-based (SetKernelObjectSecurity) and never propagates to
// children by name.

#include "core/FileIo.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace asb::win {

// Win32 / NT constant values (winnt.h, winbase.h, ntifs.h), spelled out so this header needs no Windows SDK.
namespace nt {
// Access rights.
inline constexpr quint32 kReadData = 0x0001;            // FILE_READ_DATA / FILE_LIST_DIRECTORY
inline constexpr quint32 kListDirectory = 0x0001;
inline constexpr quint32 kWriteData = 0x0002;           // FILE_WRITE_DATA / FILE_ADD_FILE
inline constexpr quint32 kAddFile = 0x0002;
inline constexpr quint32 kAppendData = 0x0004;
inline constexpr quint32 kTraverse = 0x0020;
inline constexpr quint32 kReadAttributes = 0x0080;
inline constexpr quint32 kWriteAttributes = 0x0100;
inline constexpr quint32 kDelete = 0x00010000;
inline constexpr quint32 kReadControl = 0x00020000;
inline constexpr quint32 kWriteDac = 0x00040000;
inline constexpr quint32 kWriteOwner = 0x00080000;
inline constexpr quint32 kSynchronize = 0x00100000;
inline constexpr quint32 kGenericWrite = 0x40000000;
inline constexpr quint32 kGenericRead = 0x80000000;
// Share modes.
inline constexpr quint32 kShareRead = 0x1;
inline constexpr quint32 kShareWrite = 0x2;
inline constexpr quint32 kShareDelete = 0x4;
inline constexpr quint32 kShareAll = kShareRead | kShareWrite | kShareDelete;
// CreateFileW dispositions and flags.
inline constexpr quint32 kCreateNew = 1;
inline constexpr quint32 kOpenExisting = 3;
inline constexpr quint32 kOpenAlways = 4;
inline constexpr quint32 kFlagBackupSemantics = 0x02000000;
inline constexpr quint32 kFlagOpenReparsePoint = 0x00200000;
// NtCreateFile dispositions and options.
inline constexpr quint32 kFileOpen = 1;
inline constexpr quint32 kFileCreate = 2;
inline constexpr quint32 kFileOpenIf = 3;
inline constexpr quint32 kDirectoryFile = 0x00000001;
inline constexpr quint32 kNonDirectoryFile = 0x00000040;
inline constexpr quint32 kOpenReparsePoint = 0x00200000;
// File attributes.
inline constexpr quint32 kAttrDirectory = 0x10;
inline constexpr quint32 kAttrReparsePoint = 0x400;
// SECURITY_INFORMATION.
inline constexpr quint32 kOwnerInfo = 0x1;
inline constexpr quint32 kDaclInfo = 0x4;
}  // namespace nt

using Handle = quintptr;   // a native HANDLE value; 0 = none

// What a call returned, classified for the adapters. `code` is the NTSTATUS or Win32 error.
struct Status {
    enum class Kind {
        Ok,
        NotFound,           // file/path not found (ERROR_FILE_NOT_FOUND/PATH_NOT_FOUND, STATUS_OBJECT_NAME/PATH_NOT_FOUND)
        DeletePending,      // STATUS_DELETE_PENDING: going away; not evidence either way
        AccessDenied,
        SharingViolation,   // another handle's share mode refuses this open (a held lock)
        LockViolation,      // LockFileEx: another handle holds the range
        Exists,             // STATUS_OBJECT_NAME_COLLISION / ERROR_ALREADY_EXISTS / ERROR_FILE_EXISTS
        IsDirectory,        // STATUS_FILE_IS_A_DIRECTORY
        Other,
    };
    Kind kind = Kind::Ok;
    quint32 code = 0;
    QString text;

    bool ok() const { return kind == Kind::Ok; }
    bool gone() const { return kind == Kind::NotFound || kind == Kind::DeletePending; }
    static Status success() { return {}; }
    static Status fail(Kind k, quint32 code = 0, const QString& text = {}) { return {k, code, text}; }
    QString describe() const   // "<text> (0x<code>)"
    {
        const QString hex = QStringLiteral("0x%1").arg(code, 8, 16, QLatin1Char('0'));
        return text.isEmpty() ? QStringLiteral("error %1").arg(hex) : QStringLiteral("%1 (%2)").arg(text, hex);
    }
};

// Identity, attributes and size of an open handle (FILE_ID_INFO + FILE_ATTRIBUTE_TAG_INFO + end of file).
struct Facts {
    FileId id;
    quint32 attributes = 0;
    quint32 reparseTag = 0;
    qint64 size = 0;
    bool isDirectory() const { return (attributes & nt::kAttrDirectory) != 0; }
    bool isReparse() const { return (attributes & nt::kAttrReparsePoint) != 0; }
};

struct Volume {
    QString fileSystem;   // GetVolumeInformationByHandleW: "NTFS", "ReFS", "FAT32", ...
    bool remote = false;  // FileRemoteProtocolInfo answered, or the NT path is under \Device\Mup
};

class NtSeams {
public:
    virtual ~NtSeams() = default;

    // CreateFileW(path, access, share, &sa{sd, bInheritHandle = FALSE}, disposition, flags). `sd` empty =
    // no descriptor (default security).
    virtual Status createFile(const QString& path, quint32 access, quint32 share, quint32 disposition, quint32 flags,
                              const QByteArray& sd, Handle* out) = 0;
    // NtCreateFile(RootDirectory = dir, ObjectName = name, OBJ_CASE_INSENSITIVE, access | SYNCHRONIZE, share,
    // disposition, options | FILE_SYNCHRONOUS_IO_NONALERT, SecurityDescriptor = sd). An empty name reopens
    // `dir` itself.
    virtual Status openRelative(Handle dir, const QString& name, quint32 access, quint32 share, quint32 disposition,
                                quint32 options, const QByteArray& sd, Handle* out) = 0;
    // CreateDirectoryW(path, &sa{sd}): the folder exists with exactly this owner and DACL from the start.
    virtual Status createDirectory(const QString& path, const QByteArray& sd) = 0;
    virtual void close(Handle) = 0;

    virtual Status facts(Handle, Facts*) = 0;
    // GetSecurityInfo(handle, SE_FILE_OBJECT, info) as a self-relative descriptor.
    virtual Status getSecurity(Handle, quint32 info, QByteArray* sd) = 0;
    // SetKernelObjectSecurity(handle, info, sd): this object only; no propagation to children.
    virtual Status setSecurity(Handle, quint32 info, const QByteArray& sd) = 0;
    // The names in a directory (FileIdBothDirectoryInfo on the handle), without "." and "..".
    virtual Status list(Handle dir, QStringList* names) = 0;

    // ReadFile at `offset`, at most `max` bytes (fewer only at end of file).
    virtual Status read(Handle, qint64 offset, qint64 max, QByteArray* out) = 0;
    // WriteFile of the whole buffer at `offset` (-1 = at the end), retrying short writes.
    virtual Status write(Handle, qint64 offset, const QByteArray&) = 0;
    virtual Status setEndOfFile(Handle, qint64 size) = 0;
    virtual Status flush(Handle) = 0;   // FlushFileBuffers (a directory needs FILE_ADD_FILE access)
    // NtSetInformationFile(FileRenameInformationEx, RootDirectory = dir, name, POSIX_SEMANTICS
    // [| REPLACE_IF_EXISTS]): renames the open file; a replaced target may still be open by readers that
    // share delete.
    virtual Status rename(Handle file, Handle dir, const QString& name, bool replace) = 0;
    // SetFileInformationByHandle(FileDispositionInfoEx, DELETE | POSIX_SEMANTICS).
    virtual Status dispose(Handle) = 0;
    // WS-16 (D69): the open file is deleted when this handle closes (`del`), or no longer (`!del`). `posix`:
    // FileDispositionInfoEx (FILE_DISPOSITION_FLAG_DELETE | POSIX_SEMANTICS, or FILE_DISPOSITION_FLAG_DO_NOT_DELETE);
    // otherwise the classic FileDispositionInfo{DeleteFile}. The handle needs DELETE access. Test builds:
    // ASB_TEST_FAIL_LOCK_DISPOSITION=1 makes both forms of `del` fail (the POSIX one as not supported).
    virtual Status setDeleteOnClose(Handle, bool del, bool posix) = 0;
    // LockFileEx(LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, whole file).
    virtual Status lockExclusive(Handle) = 0;
    virtual Status volume(Handle, Volume*) = 0;
    // GetHandleInformation: HANDLE_FLAG_INHERIT.
    virtual bool inheritable(Handle) = 0;
    // The SID of the token this thread acts as (impersonation, else the process).
    virtual QString currentUserSid() = 0;
};

// A handle closed on destruction.
class OwnedHandle {
public:
    OwnedHandle() = default;
    OwnedHandle(NtSeams& s, Handle h) : m_s(&s), m_h(h) {}
    OwnedHandle(OwnedHandle&& o) noexcept : m_s(o.m_s), m_h(o.release()) {}
    OwnedHandle& operator=(OwnedHandle&& o) noexcept
    {
        if (this != &o) {
            reset();
            m_s = o.m_s;
            m_h = o.release();
        }
        return *this;
    }
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    ~OwnedHandle() { reset(); }

    Handle get() const { return m_h; }
    NtSeams* seams() const { return m_s; }
    explicit operator bool() const { return m_h != 0; }
    Handle release()
    {
        const Handle h = m_h;
        m_h = 0;
        return h;
    }
    void reset()
    {
        if (m_h && m_s) m_s->close(m_h);
        m_h = 0;
    }

private:
    NtSeams* m_s = nullptr;
    Handle m_h = 0;
};

}  // namespace asb::win
