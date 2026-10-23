#pragma once

// Decides whether the data folder can be trusted (with
// the regular-file rule for known names, INV-63). The service trusts config.json and an owner
// keeps WRITE_DAC whatever the DACL says, so only trusted owners may own the directory or anything
// directly in it. Pure logic: the adapters (WS-6 Windows, WS-8 POSIX) gather the entries on handles and
// call decide(); recreation is no longer an outcome of loading — only Repair renames aside (S2).

#include "core/AceJudge.h"
#include "core/Interfaces.h"

#include <QList>
#include <QString>

#include <optional>

namespace asb {

// What the adapter saw for one entry, on a handle opened without following links.
struct FsEntry {
    enum class Kind { Directory, File, Link, Other };   // Link = symlink, junction or any reparse point
    QString name;
    std::optional<QString> owner;   // nullopt when unreadable
    Kind kind = Kind::File;
    bool vanished = false;          // listed, then gone before it could be read (not evidence either way)
    QList<AceInfo> acl;

    static FsEntry gone(const QString& name)
    {
        FsEntry e;
        e.name = name;
        e.vanished = true;
        return e;
    }
};

namespace DataDirectoryTrust {

inline constexpr char kLocalSystemSid[] = "S-1-5-18";
inline constexpr char kAdministratorsSid[] = "S-1-5-32-544";

// The files the bridge itself keeps in the folder, plus our temps (`*.<hex>.asbtmp`): each must be a
// regular file (INV-63), or the folder is untrusted.
bool isKnownFileName(const QString& name);

struct Decision {
    DirAction action = DirAction::Create;
    QString reason;   // empty for Trust and Create
};

// `directory` is nullopt when nothing exists at the path.
Decision decide(const std::optional<FsEntry>& directory, const QList<FsEntry>& children, const TrustPolicy&);

}  // namespace DataDirectoryTrust
}  // namespace asb
