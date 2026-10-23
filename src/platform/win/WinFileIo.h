#pragma once

// FileIo on Windows (WS-6; the WS-3 seam's native implementation): INV-15, INV-75, INV-77, INV-96.
//
//  createTemp   NtCreateFile(RootDirectory = the folder handle, FILE_CREATE, FILE_OPEN_REPARSE_POINT,
//               FILE_NON_DIRECTORY_FILE) with the policy's security descriptor in the same call: nothing
//               existing at the generated name is ever opened, followed or truncated (INV-15), and the
//               file never exists with another owner (INV-75, INV-96).
//  commitAs     the temp is renamed BY ITS HANDLE (FileRenameInformationEx, POSIX semantics, replace, target
//               relative to the folder handle), so exactly the bytes written and read back are published, and
//               a reader that shares delete does not block it (WinCI reader_does_not_block_rename); then the
//               directory flush (FlushFileBuffers on a FILE_ADD_FILE reopen of the folder) = durable.
//  remove / openExisting   by handle, relative to the folder, never through a link or reparse point.
//
// The security a create receives depends on where the file goes (CreateSecurity): in the data folder the
// owner only (Config: Administrators, Runtime: SYSTEM) and the folder's protected DACL is inherited; in the
// lock directory (engine.lock, engine.lock.id) owner Administrators and the protected SYSTEM+Administrators
// DACL. The Runtime owner SYSTEM is settable only by a process running as SYSTEM (the service) or holding
// SeRestorePrivilege; this class never enables a privilege.

#include "core/FileIo.h"
#include "platform/win/NtSeams.h"

namespace asb::win {

// A directory opened by WindowsDataFolder or EngineLock: the native handle plus the identity read from it.
class WinDirHandle final : public DirHandle {
public:
    WinDirHandle() = default;
    WinDirHandle(OwnedHandle h, FileId id) : m_h(std::move(h)), m_id(id) {}
    FileId id() const override { return m_id; }
    Handle native() const { return m_h.get(); }
    bool valid() const { return bool(m_h); }

private:
    OwnedHandle m_h;
    FileId m_id;
};

struct CreateSecurity {
    QByteArray config;    // OwnerPolicy::Config
    QByteArray runtime;   // OwnerPolicy::Runtime
    static CreateSecurity dataFolder();      // owner Administrators / SYSTEM; DACL inherited from the folder
    static CreateSecurity lockDirectory();   // lockFileDescriptor() for both
};

class WinFileIo final : public FileIo {
public:
    WinFileIo(NtSeams& seams, CreateSecurity security) : m_s(seams), m_sec(std::move(security)) {}

    std::unique_ptr<TempFile> createTemp(const DirHandle& dir, OwnerPolicy, const NameGen& gen, QString* err) override;
    bool remove(const DirHandle& dir, const QString& name, QString* err) override;
    bool syncDir(const DirHandle& dir, QString* err) override;
    std::unique_ptr<ExistingFile> openExisting(const DirHandle& dir, const QString& name, QString* err) override;

    // The native handle behind a DirHandle this adapter family issued; 0 for any other DirHandle.
    static Handle nativeOf(const DirHandle&);
    // A single path component: no separator, no ':' (alternate data stream), not "." or "..".
    static bool isPlainName(const QString&);

private:
    NtSeams& m_s;
    CreateSecurity m_sec;
};

}  // namespace asb::win
