#include "platform/win/WindowsDataFolder.h"

#include "core/ConfigFiles.h"
#include "core/FilesystemPolicy.h"
#include "platform/win/SecurityDescriptor.h"

#include <QDateTime>
#include <QRandomGenerator>

#include <vector>

namespace asb::win {
namespace {

constexpr quint32 kInspectDir = nt::kReadControl | nt::kReadAttributes | nt::kListDirectory;
constexpr quint32 kInspectChild = nt::kReadControl | nt::kReadAttributes;   // no data right: share modes never apply

QString native(QString p)
{
    p.replace(QLatin1Char('/'), QLatin1Char('\\'));
    while (p.size() > 3 && p.endsWith(QLatin1Char('\\'))) p.chop(1);
    return p;
}

FsEntry::Kind kindOf(const Facts& f)
{
    if (f.isReparse()) return FsEntry::Kind::Link;
    return f.isDirectory() ? FsEntry::Kind::Directory : FsEntry::Kind::File;
}

bool isConfigPolicyFile(const QString& name)
{
    for (const char* n : {ConfigFiles::kConfig, ConfigFiles::kPending, ConfigFiles::kStartToken})
        if (name.compare(QLatin1String(n), Qt::CaseInsensitive) == 0) return true;
    return false;
}

[[noreturn]] void throwPlatform(const QString& what)
{
    throw PlatformError(what.toLocal8Bit().toStdString());
}

// A failure once changes may have been made: they travel with it, so the caller still reports them.
[[noreturn]] void throwAfterChanges(const QString& what, const QStringList& changes)
{
    throw PlatformErrorAfterChanges(what.toLocal8Bit().toStdString(), changes);
}

}  // namespace

WindowsDataFolder::WindowsDataFolder(NtSeams& seams, QString path, std::function<QString()> asideSuffix)
    : m_s(seams), m_path(native(std::move(path))), m_asideSuffix(std::move(asideSuffix))
{
    const qsizetype cut = m_path.lastIndexOf(QLatin1Char('\\'));
    m_parent = cut > 0 ? m_path.left(cut) : QString();
    if (m_parent.size() == 2 && m_parent.endsWith(QLatin1Char(':'))) m_parent += QLatin1Char('\\');
    m_name = m_path.mid(cut + 1);
    if (!m_asideSuffix) m_asideSuffix = &WindowsDataFolder::defaultAsideSuffix;
}

QString WindowsDataFolder::defaultAsideSuffix()
{
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")) + QLatin1Char('-') +
           QString::number(QRandomGenerator::system()->generate(), 16).rightJustified(8, QLatin1Char('0'));
}

FsEntry WindowsDataFolder::entryFor(Handle h, const QString& name, FsEntry::Kind kind)
{
    FsEntry e;
    e.name = name;
    e.kind = kind;
    QByteArray bytes;
    const Status st = m_s.getSecurity(h, nt::kOwnerInfo | nt::kDaclInfo, &bytes);
    if (!st.ok()) {
        if (st.gone()) return FsEntry::gone(name);
        return e;   // unreadable security: owner nullopt = untrusted (INV-40)
    }
    const std::optional<sd::Descriptor> d = sd::parse(bytes);
    if (!d) {
        e.acl = {AceInfo{{}, 0, AceKind::Unknown}};   // undecodable: untrusted (INV-19)
        return e;
    }
    e.owner = d->owner;
    e.acl = sd::toAceInfos(*d);
    return e;
}

Inspection WindowsDataFolder::inspect(TrustMode mode)
{
    m_dir = WinDirHandle();
    m_children.clear();
    m_scratch = mode == TrustMode::Scratch;
    m_policy = TrustPolicy::windows(m_scratch ? m_s.currentUserSid() : QString());

    Inspection out;
    Handle h = 0;
    Status st = m_s.createFile(m_path, kInspectDir, nt::kShareAll, nt::kOpenExisting,
                               nt::kFlagBackupSemantics | nt::kFlagOpenReparsePoint, {}, &h);
    if (!st.ok()) {
        if (st.kind == Status::Kind::NotFound) {
            // INV-116 before anything creates it: the volume the folder would be created on. A parent that
            // cannot be opened leaves the volume unknown, and an unknown volume is refused (fail closed).
            Handle p = 0;
            const Status ps = m_s.createFile(m_parent, nt::kReadAttributes, nt::kShareAll, nt::kOpenExisting,
                                             nt::kFlagBackupSemantics, {}, &p);
            if (!ps.ok()) {
                out.action = DirAction::Refuse;
                out.reason = QStringLiteral("its file system cannot be determined (the parent folder cannot be opened: %1)")
                                 .arg(m_parent.isEmpty() ? QStringLiteral("the path has no parent") : ps.describe());
                return out;
            }
            OwnedHandle parent(m_s, p);
            Volume v;
            const Status vs = m_s.volume(p, &v);
            if (!vs.ok() || !FilesystemPolicy::windowsVolumeSupported(FilesystemPolicy::fromWindowsName(v.fileSystem), v.remote)) {
                out.action = DirAction::Refuse;
                out.reason = !vs.ok()   ? QStringLiteral("its file system cannot be determined (%1)").arg(vs.describe())
                             : v.remote ? QStringLiteral("it would be on a network volume")
                                        : QStringLiteral("it would be on an unsupported file system (%1; NTFS or ReFS is required)")
                                              .arg(v.fileSystem);
                return out;
            }
            const DataDirectoryTrust::Decision d = DataDirectoryTrust::decide(std::nullopt, {}, m_policy);
            out.action = d.action;
            out.reason = d.reason;
            return out;
        }
        out.action = DirAction::Recreate;
        out.reason = QStringLiteral("the folder cannot be opened (%1)").arg(st.describe());
        return out;
    }
    OwnedHandle dir(m_s, h);
    // INV-116 first: NTFS or ReFS, and not a network volume; nothing else is looked at on another volume.
    Volume v;
    st = m_s.volume(h, &v);
    if (!st.ok() || !FilesystemPolicy::windowsVolumeSupported(FilesystemPolicy::fromWindowsName(v.fileSystem), v.remote)) {
        out.action = DirAction::Refuse;
        out.reason = !st.ok()      ? QStringLiteral("its file system cannot be determined (%1)").arg(st.describe())
                     : v.remote    ? QStringLiteral("it is on a network volume")
                                   : QStringLiteral("it is on an unsupported file system (%1; NTFS or ReFS is required)")
                                      .arg(v.fileSystem);
        return out;
    }

    Facts f;
    if (st = m_s.facts(h, &f); !st.ok()) {
        out.action = DirAction::Recreate;
        out.reason = QStringLiteral("the folder cannot be read (%1)").arg(st.describe());
        return out;
    }
    out.folderId = f.id;
    const FsEntry::Kind kind = kindOf(f);
    if (kind != FsEntry::Kind::Directory) {
        FsEntry e;
        e.name = m_name;
        e.kind = kind;
        const DataDirectoryTrust::Decision d = DataDirectoryTrust::decide(e, {}, m_policy);
        out.action = d.action;
        out.reason = d.reason;
        return out;   // touch nothing behind a link or a file (INV-17)
    }
    const FsEntry folder = entryFor(h, m_name, FsEntry::Kind::Directory);
    QList<FsEntry> children;
    QStringList names;
    if (st = m_s.list(h, &names); !st.ok()) {
        FsEntry e;
        e.name = QStringLiteral("(the folder's contents cannot be listed: %1)").arg(st.describe());
        children.append(e);   // cannot see inside: not trusted
    }
    for (const QString& name : names) {
        out.children << name;
        Handle c = 0;
        st = m_s.openRelative(h, name, kInspectChild, nt::kShareAll, nt::kFileOpen, nt::kOpenReparsePoint, {}, &c);
        if (!st.ok()) {
            if (st.gone()) {
                children.append(FsEntry::gone(name));   // renamed away or rolled over: not evidence (INV-40)
            } else {
                FsEntry e;   // unreadable: untrusted (INV-40)
                e.name = name;
                children.append(e);
            }
            continue;
        }
        OwnedHandle child(m_s, c);
        Facts cf;
        if (st = m_s.facts(c, &cf); !st.ok()) {
            FsEntry e = st.gone() ? FsEntry::gone(name) : FsEntry{};
            e.name = name;
            children.append(e);
            continue;
        }
        const FsEntry e = entryFor(c, name, kindOf(cf));
        children.append(e);
        if (!e.vanished) m_children.insert(name.toLower(), Child{name, cf.id, e.kind});
    }
    const DataDirectoryTrust::Decision d = DataDirectoryTrust::decide(folder, children, m_policy);
    out.action = d.action;
    out.reason = d.reason;
    m_dir = WinDirHandle(std::move(dir), f.id);
    return out;
}

bool WindowsDataFolder::sameFolder(const Inspection& insp) const
{
    return m_dir.valid() && insp.folderId == m_dir.id();
}

std::optional<QByteArray> WindowsDataFolder::readFile(const Inspection& insp, const QString& name, qint64 cap, QString* err)
{
    auto fail = [err](const QString& e) -> std::optional<QByteArray> {
        if (err) *err = e;
        return std::nullopt;
    };
    if (insp.action != DirAction::Trust || !sameFolder(insp))
        return fail(QStringLiteral("the data folder was not inspected and trusted"));
    if (!WinFileIo::isPlainName(name)) return fail(QStringLiteral("invalid file name"));
    Handle h = 0;
    Status st = m_s.openRelative(m_dir.native(), name, kInspectChild | nt::kReadData, nt::kShareAll, nt::kFileOpen,
                                 nt::kNonDirectoryFile | nt::kOpenReparsePoint, {}, &h);
    if (!st.ok()) {
        if (st.kind == Status::Kind::NotFound) return fail(fileNotFoundError());
        return fail(QStringLiteral("%1 cannot be opened (%2)").arg(name, st.describe()));
    }
    OwnedHandle file(m_s, h);
    // B2: every check below is on the handle the bytes are read from.
    Facts f;
    if (st = m_s.facts(h, &f); !st.ok()) return fail(QStringLiteral("%1 cannot be read (%2)").arg(name, st.describe()));
    if (f.isReparse() || f.isDirectory()) return fail(QStringLiteral("%1 is not a regular file").arg(name));
    QByteArray sec;
    if (st = m_s.getSecurity(h, nt::kOwnerInfo | nt::kDaclInfo, &sec); !st.ok())
        return fail(QStringLiteral("the permissions of %1 cannot be read (%2)").arg(name, st.describe()));
    const std::optional<sd::Descriptor> d = sd::parse(sec);
    if (!d) return fail(QStringLiteral("the permissions of %1 cannot be decoded").arg(name));
    const bool configFile = isConfigPolicyFile(name);
    const bool ownerOk = configFile && !m_scratch ? d->owner == QLatin1String(sd::kAdministrators)
                                                  : m_policy.isTrustedOwner(d->owner);
    if (!ownerOk) return fail(QStringLiteral("%1 is owned by an account that is not trusted").arg(name));
    if (AceJudge::anyUntrusted(sd::toAceInfos(*d), m_policy))
        return fail(QStringLiteral("%1 can be changed by an account that is not trusted").arg(name));
    if (!m_scratch && name.compare(QLatin1String(ConfigFiles::kStartToken), Qt::CaseInsensitive) == 0) {
        bool inheritedOnly = !d->daclProtected;
        for (const sd::Ace& a : d->dacl) inheritedOnly = inheritedOnly && (a.flags & sd::kInherited);
        if (!inheritedOnly) return fail(QStringLiteral("start.token has permissions of its own, not the folder's"));
    }
    QByteArray bytes;
    if (st = m_s.read(h, 0, cap + 1, &bytes); !st.ok())
        return fail(QStringLiteral("%1 cannot be read (%2)").arg(name, st.describe()));
    if (bytes.size() > cap) return fail(QStringLiteral("%1 is larger than %2 bytes").arg(name).arg(cap));
    return bytes;
}

void WindowsDataFolder::createLockedDown()
{
    // INV-16: owner and protected DACL in the create call itself; there is no moment with default security.
    const Status st = m_s.createDirectory(m_path, sd::dataFolderDescriptor());
    if (st.kind == Status::Kind::Exists)
        throwPlatform(QStringLiteral("%1 was created by someone else while it was being secured. Remove it as an "
                             "administrator and try again.")
                  .arg(m_path));
    if (!st.ok()) throwPlatform(QStringLiteral("could not create %1: %2").arg(m_path, st.describe()));
}

OwnedHandle WindowsDataFolder::openFolderForMutation(quint32 access, const Inspection& insp)
{
    if (!sameFolder(insp)) throwPlatform(QStringLiteral("the data folder changed since it was inspected; nothing was changed"));
    Handle h = 0;
    Status st = m_s.createFile(m_path, access, nt::kShareAll, nt::kOpenExisting,
                               nt::kFlagBackupSemantics | nt::kFlagOpenReparsePoint, {}, &h);
    if (!st.ok()) throwPlatform(QStringLiteral("could not open %1 to change it: %2").arg(m_path, st.describe()));
    OwnedHandle m(m_s, h);
    Facts f;
    st = m_s.facts(h, &f);
    // INV-62: the object about to be changed must be the object that was inspected.
    if (!st.ok() || f.id != insp.folderId || f.isReparse() || !f.isDirectory())
        throwPlatform(QStringLiteral("the data folder changed since it was inspected; nothing was changed"));
    return m;
}

QStringList WindowsDataFolder::normalise(const Inspection& insp)
{
    if (insp.action != DirAction::Trust) throwPlatform(QStringLiteral("only a trusted data folder can be normalised"));
    OwnedHandle folder = openFolderForMutation(nt::kReadControl | nt::kWriteDac | nt::kWriteOwner | nt::kReadAttributes, insp);

    // Open and identity-check every mutation handle first, so a swapped object aborts before any change.
    struct Target {
        QString name;
        OwnedHandle h;
    };
    std::vector<Target> files;
    for (auto it = m_children.cbegin(); it != m_children.cend(); ++it) {
        if (it->kind != FsEntry::Kind::File) continue;
        Handle c = 0;
        const Status st = m_s.openRelative(m_dir.native(), it->name, nt::kReadControl | nt::kWriteDac | nt::kReadAttributes,
                                           nt::kShareAll, nt::kFileOpen, nt::kNonDirectoryFile | nt::kOpenReparsePoint, {}, &c);
        if (st.gone()) continue;   // rolled over or renamed away since the inspection
        if (!st.ok()) throwPlatform(QStringLiteral("could not open %1 to secure it: %2; nothing was changed").arg(it->name, st.describe()));
        OwnedHandle owned(m_s, c);
        Facts f;
        const Status fs = m_s.facts(c, &f);
        if (!fs.ok() || f.id != it->id || f.isReparse() || f.isDirectory())
            throwPlatform(QStringLiteral("%1 changed since the folder was inspected; nothing was changed").arg(it->name));
        files.push_back({it->name, std::move(owned)});
    }

    // Each change is recorded the moment it is made, before the next step; a later failure carries the list
    // (PlatformErrorAfterChanges), so a change that stays on disk is always reported (master §4.6 P4).
    QStringList changes;
    const std::optional<sd::Descriptor> want = sd::parse(sd::dataFolderDescriptor());
    QByteArray cur;
    const Status gs = m_s.getSecurity(folder.get(), nt::kOwnerInfo | nt::kDaclInfo, &cur);
    const std::optional<sd::Descriptor> have = gs.ok() ? sd::parse(cur) : std::nullopt;
    if (!have || have->owner != want->owner || !have->daclPresent || have->daclNull || !have->daclProtected ||
        !sd::sameEntries(have->dacl, want->dacl)) {
        const Status st = m_s.setSecurity(folder.get(), nt::kOwnerInfo | nt::kDaclInfo, sd::dataFolderDescriptor());
        if (!st.ok()) throwAfterChanges(QStringLiteral("could not secure %1: %2").arg(m_path, st.describe()), changes);
        changes << QStringLiteral("secured the data folder (owner Administrators, protected permissions)");
    }
    for (Target& t : files) {
        QByteArray bytes;
        if (const Status st = m_s.getSecurity(t.h.get(), nt::kOwnerInfo | nt::kDaclInfo, &bytes); !st.ok())
            throwAfterChanges(QStringLiteral("could not read the permissions of %1: %2").arg(t.name, st.describe()), changes);
        const std::optional<sd::Descriptor> d = sd::parse(bytes);
        const QList<sd::Ace> expected = sd::inheritedForFile(want->dacl, d && d->owner ? *d->owner : QString());
        if (d && d->daclPresent && !d->daclNull && !d->daclProtected && sd::sameEntries(d->dacl, expected)) continue;
        sd::Descriptor nd;
        nd.daclPresent = true;
        nd.daclAutoInherited = true;
        nd.dacl = expected;
        const QByteArray set = sd::build(QString(), nd);
        const Status st = set.isEmpty() ? Status::fail(Status::Kind::Other, 0, QStringLiteral("invalid descriptor"))
                                        : m_s.setSecurity(t.h.get(), nt::kDaclInfo, set);
        if (!st.ok()) throwAfterChanges(QStringLiteral("could not reset the permissions of %1: %2").arg(t.name, st.describe()), changes);
        changes << QStringLiteral("reset the permissions of %1 to the folder's").arg(t.name);
    }
    return changes;
}

QStringList WindowsDataFolder::knownFiles() const
{
    return {QString::fromLatin1(ConfigFiles::kConfig), QString::fromLatin1(ConfigFiles::kPending),
            QString::fromLatin1(ConfigFiles::kStartToken), QString::fromLatin1(ConfigFiles::kStatus),
            QString::fromLatin1(ConfigFiles::kLog), QString::fromLatin1(ConfigFiles::kLog) + QStringLiteral(".1")};
}

bool WindowsDataFolder::removeIfEmpty(const Inspection& insp, QString* err)
{
    Handle h = 0;
    Status st = m_s.createFile(m_path, nt::kDelete | nt::kReadAttributes, nt::kShareAll, nt::kOpenExisting,
                               nt::kFlagBackupSemantics | nt::kFlagOpenReparsePoint, {}, &h);
    if (!st.ok()) {
        *err = st.describe();
        return false;
    }
    OwnedHandle folder(m_s, h);
    Facts f;
    st = m_s.facts(h, &f);
    if (!st.ok() || insp.folderId == FileId{} || f.id != insp.folderId || f.isReparse() || !f.isDirectory()) {
        *err = QStringLiteral("%1 changed since it was inspected").arg(m_path);
        return false;
    }
    st = m_s.dispose(h);   // a directory that is not empty is refused (ERROR_DIR_NOT_EMPTY)
    if (!st.ok()) {
        *err = st.describe();
        return false;
    }
    m_dir = WinDirHandle();
    m_children.clear();
    return true;
}

QStringList WindowsDataFolder::asideFolders()
{
    QStringList out;
    if (m_parent.isEmpty()) return out;
    Handle p = 0;
    if (!m_s.createFile(m_parent, nt::kListDirectory | nt::kReadAttributes, nt::kShareAll, nt::kOpenExisting,
                        nt::kFlagBackupSemantics, {}, &p)
             .ok())
        return out;
    OwnedHandle parent(m_s, p);
    QStringList names;
    if (!m_s.list(p, &names).ok()) return out;
    const QString prefix = m_name + QStringLiteral(".untrusted-");
    QString base = m_parent;
    if (!base.endsWith(QLatin1Char('\\'))) base += QLatin1Char('\\');
    for (const QString& n : names)
        if (n.startsWith(prefix, Qt::CaseInsensitive)) out << base + n;
    out.sort();
    return out;
}

QString WindowsDataFolder::renameAside(const Inspection& insp)
{
    const QString instructions =
        QStringLiteral(" Nothing was changed. As an administrator, run  takeown /f \"%1\" /r /d y  and  "
                       "icacls \"%1\" /reset /t /c  and then try Repair again.")
            .arg(m_path);
    if (!sameFolder(insp)) throwPlatform(QStringLiteral("The data folder changed since it was inspected.") + instructions);
    Handle h = 0;
    Status st = m_s.createFile(m_path, nt::kDelete | nt::kReadAttributes, nt::kShareAll, nt::kOpenExisting,
                               nt::kFlagBackupSemantics | nt::kFlagOpenReparsePoint, {}, &h);
    if (!st.ok()) throwPlatform(QStringLiteral("Could not open %1 to move it aside (%2).").arg(m_path, st.describe()) + instructions);
    OwnedHandle folder(m_s, h);
    Facts f;
    st = m_s.facts(h, &f);
    if (!st.ok() || f.id != insp.folderId || f.isReparse() || !f.isDirectory())
        throwPlatform(QStringLiteral("The data folder changed since it was inspected.") + instructions);
    Handle p = 0;
    st = m_s.createFile(m_parent, nt::kReadAttributes | nt::kTraverse, nt::kShareAll, nt::kOpenExisting,
                        nt::kFlagBackupSemantics, {}, &p);
    if (!st.ok()) throwPlatform(QStringLiteral("Could not open %1 (%2).").arg(m_parent, st.describe()) + instructions);
    OwnedHandle parent(m_s, p);
    const QString aside = m_name + QStringLiteral(".untrusted-") + m_asideSuffix();
    // By handle, no replace: the folder moves with everything in it; nothing inside is opened (INV-74).
    st = m_s.rename(h, p, aside, /*replace*/ false);
    if (!st.ok()) throwPlatform(QStringLiteral("Could not move %1 aside (%2).").arg(m_path, st.describe()) + instructions);
    m_dir = WinDirHandle();
    m_children.clear();
    QString out = m_parent;
    if (!out.endsWith(QLatin1Char('\\'))) out += QLatin1Char('\\');
    return out + aside;
}

}  // namespace asb::win
