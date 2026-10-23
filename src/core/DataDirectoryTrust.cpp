#include "core/DataDirectoryTrust.h"

#include "core/AtomicJson.h"
#include "core/ConfigFiles.h"

namespace asb::DataDirectoryTrust {

bool isKnownFileName(const QString& name)
{
    static const QStringList known = {
        QString::fromLatin1(ConfigFiles::kConfig),     QString::fromLatin1(ConfigFiles::kPending),
        QString::fromLatin1(ConfigFiles::kStartToken), QString::fromLatin1(ConfigFiles::kStatus),
        QString::fromLatin1(ConfigFiles::kLog),        QString::fromLatin1(ConfigFiles::kLog) + QStringLiteral(".1"),
    };
    // Case-insensitive: on Windows "Config.JSON" is the same file.
    for (const QString& k : known)
        if (name.compare(k, Qt::CaseInsensitive) == 0) return true;
    return isTempName(name.toLower());
}

Decision decide(const std::optional<FsEntry>& directory, const QList<FsEntry>& children, const TrustPolicy& policy)
{
    if (!directory) return {DirAction::Create, {}};
    const FsEntry& dir = *directory;
    if (dir.kind != FsEntry::Kind::Directory)
        return {DirAction::Refuse, QStringLiteral("it is a file, junction or symbolic link, not a plain folder")};
    if (!policy.isTrustedOwner(dir.owner))
        return {DirAction::Recreate, QStringLiteral("the folder is owned by an account that is not trusted")};
    if (AceJudge::anyUntrusted(dir.acl, policy))
        return {DirAction::Recreate, QStringLiteral("the folder can be changed by an account that is not trusted")};
    for (const FsEntry& c : children) {
        if (c.vanished) continue;   // renamed away or rolled over between the listing and the read
        if (c.kind == FsEntry::Kind::Link)
            return {DirAction::Recreate, QStringLiteral("%1 is a link or junction").arg(c.name)};
        if (isKnownFileName(c.name) && c.kind != FsEntry::Kind::File)
            return {DirAction::Recreate, QStringLiteral("%1 is not a regular file").arg(c.name)};
        if (!policy.isTrustedOwner(c.owner))
            return {DirAction::Recreate, QStringLiteral("%1 is owned by an account that is not trusted").arg(c.name)};
        if (AceJudge::anyUntrusted(c.acl, policy))
            return {DirAction::Recreate, QStringLiteral("%1 can be changed by an account that is not trusted").arg(c.name)};
    }
    return {DirAction::Trust, {}};
}

}  // namespace asb::DataDirectoryTrust
