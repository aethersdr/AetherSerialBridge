#pragma once

// %ProgramData%\AetherSerialBridge on NTFS/ReFS: the Windows IDataFolder (WS-6; master §4.4; INV-16, 17, 19,
// 40, 62, 63, 74, 75, 116). Portable logic over NtSeams; the decision itself is DataDirectoryTrust::decide.
//
//  inspect      the folder is opened by path WITHOUT following a reparse point (READ_CONTROL |
//               FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY, share read/write/delete); a junction, symlink or
//               file at the path is Refuse (INV-17); an unsupported or remote volume is Refuse (INV-116).
//               Owner and DACL come from that handle and are decoded by SecurityDescriptor (Unknown ACE kinds,
//               NULL DACL: untrusted, INV-19). Children are listed on the handle and each is opened RELATIVE to
//               it (NtCreateFile, FILE_OPEN_REPARSE_POINT, READ_CONTROL | FILE_READ_ATTRIBUTES only: share
//               modes never apply, so a busy log cannot make the folder look untrusted). A child that is not
//               found or delete-pending is "gone", not evidence; any other failure is untrusted (INV-40).
//  readFile     relative to the inspected handle (identity-checked), FILE_READ_DATA added, regular file only;
//               owner and DACL re-checked on that same handle (config, pending and token: owner
//               Administrators; start.token: its DACL must be exactly inherited from the folder), <= cap bytes.
//  createLockedDown   CreateDirectoryW with owner Administrators and the protected DACL in the one call (INV-16).
//  normalise    mutation handles (READ_CONTROL | WRITE_DAC | WRITE_OWNER | ...) opened separately; their
//               FILE_ID_INFO must equal what the inspection saw, or nothing is changed (INV-62). Folder: owner
//               Administrators + protected DACL. Each regular child: owner kept, DACL reset to the entries it
//               inherits from the folder. Set through SetKernelObjectSecurity on the handle: no name-based
//               propagation.
//  renameAside  Repair only: the folder handle (DELETE), identity-checked, renamed by handle to the sibling
//               "<name>.untrusted-<UTC>-<rand>" (no replace); nothing inside is opened or deleted (INV-74).
//
// The trusted owners are SYSTEM and Administrators in every mode but Scratch, which also trusts the user
// running the tool (master §4.4). The running administrator's own SID is NOT trusted in the other
// modes: under the object-creator owner policy that would let an apply trust a file the service (exit 3)
// and other administrators refuse (INV-75).

#include "core/DataDirectoryTrust.h"
#include "core/Interfaces.h"
#include "platform/win/NtSeams.h"
#include "platform/win/WinFileIo.h"

#include <QMap>

#include <functional>

namespace asb::win {

class WindowsDataFolder final : public IDataFolder {
public:
    // `path` with '/' or '\' separators, absolute (from serviceLocations() / AppLocations, or a test root).
    WindowsDataFolder(NtSeams& seams, QString path, std::function<QString()> asideSuffix = {});

    Inspection inspect(TrustMode) override;
    std::optional<QByteArray> readFile(const Inspection&, const QString& name, qint64 cap, QString* err) override;
    void createLockedDown() override;
    QStringList normalise(const Inspection&) override;
    QString renameAside(const Inspection&) override;
    const DirHandle& handle() const override { return m_dir; }
    // WS-16 (INV-125): config.json, config.pending.json, start.token, status.json, bridge.log, bridge.log.1.
    // removeIfEmpty: the folder opened with DELETE (reparse points not followed), its identity checked against the
    // inspection, then FileDispositionInfoEx (POSIX): a folder with anything in it is refused (not empty), never emptied.
    // asideFolders: the "<name>.untrusted-*" siblings renameAside made.
    QStringList knownFiles() const override;
    bool removeIfEmpty(const Inspection&, QString* err) override;
    QStringList asideFolders() override;
    void closeIdle() override { m_dir = WinDirHandle(); }   // v1.1 INV-128

    QString path() const { return m_path; }
    // "AetherSerialBridge.untrusted-20261014T123000Z-1a2b3c4d" (the default suffix generator).
    static QString defaultAsideSuffix();

private:
    struct Child {
        QString name;
        FileId id;
        FsEntry::Kind kind = FsEntry::Kind::Other;
    };
    FsEntry entryFor(Handle h, const QString& name, FsEntry::Kind kind);
    bool sameFolder(const Inspection&) const;
    OwnedHandle openFolderForMutation(quint32 access, const Inspection&);

    NtSeams& m_s;
    QString m_path;      // native separators
    QString m_parent;    // native separators
    QString m_name;
    std::function<QString()> m_asideSuffix;
    WinDirHandle m_dir;
    TrustPolicy m_policy;
    bool m_scratch = false;
    QMap<QString, Child> m_children;    // the last inspection's view, keyed by lower-case name
};

}  // namespace asb::win
