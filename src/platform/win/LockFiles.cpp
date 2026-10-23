#include "platform/win/LockFiles.h"

#include "core/AceJudge.h"
#include "core/FilesystemPolicy.h"
#include "platform/win/SecurityDescriptor.h"

namespace asb::win {
namespace {

QString native(QString p)
{
    p.replace(QLatin1Char('/'), QLatin1Char('\\'));
    while (p.size() > 3 && p.endsWith(QLatin1Char('\\'))) p.chop(1);
    return p;
}

TrustPolicy directoryPolicy()
{
    return TrustPolicy{{QString::fromLatin1(sd::kSystem), QString::fromLatin1(sd::kAdministrators),
                        QString::fromLatin1(sd::kTrustedInstaller)},
                       {}};
}

LockResult fail(QString* why, LockResult r, const QString& w)
{
    if (why) *why = w;
    return r;
}

}  // namespace

LockDirectory::LockDirectory(NtSeams& seams, QString path, LockRole role, LockReport report)
    : m_s(seams), m_path(native(std::move(path))), m_role(role), m_report(std::move(report))
{
}

bool LockDirectory::directoryDescriptorTrusted(const QByteArray& bytes, QString* why)
{
    const std::optional<sd::Descriptor> d = sd::parse(bytes);
    if (!d) {
        if (why) *why = QStringLiteral("its permissions cannot be decoded");
        return false;
    }
    const TrustPolicy policy = directoryPolicy();
    if (!policy.isTrustedOwner(d->owner)) {
        if (why) *why = QStringLiteral("it is owned by %1").arg(d->owner.value_or(QStringLiteral("nobody")));
        return false;
    }
    sd::Descriptor judged = *d;
    judged.dacl.clear();
    for (const sd::Ace& a : d->dacl) {
        const bool creatorOwnerInheritOnly = a.kind() == AceKind::Allow && a.sid == QLatin1String(sd::kCreatorOwner) &&
                                             (a.flags & sd::kInheritOnly);
        if (!creatorOwnerInheritOnly) judged.dacl.append(a);
    }
    if (AceJudge::anyUntrusted(sd::toAceInfos(judged), policy)) {
        if (why) *why = QStringLiteral("it can be changed by an account that is not trusted");
        return false;
    }
    return true;
}

LockResult LockDirectory::open(bool create, QString* why)
{
    m_dir = WinDirHandle();
    const quint32 access = nt::kReadControl | nt::kReadAttributes | nt::kListDirectory;
    const quint32 flags = nt::kFlagBackupSemantics | nt::kFlagOpenReparsePoint;
    Handle h = 0;
    Status st = m_s.createFile(m_path, access, nt::kShareAll, nt::kOpenExisting, flags, {}, &h);
    if (st.kind == Status::Kind::NotFound) {
        if (!create || m_role != LockRole::Applier) return fail(why, LockResult::Missing, QStringLiteral("%1 does not exist").arg(m_path));
        // INV-116 before any change: the volume the directory would be created on. A parent that cannot be
        // opened leaves the volume unknown, and an unknown volume is refused (fail closed).
        const qsizetype cut = m_path.lastIndexOf(QLatin1Char('\\'));
        QString parentPath = cut > 0 ? m_path.left(cut) : QString();
        if (parentPath.size() == 2 && parentPath.endsWith(QLatin1Char(':'))) parentPath += QLatin1Char('\\');
        Handle p = 0;
        Volume pv;
        st = m_s.createFile(parentPath, nt::kReadAttributes, nt::kShareAll, nt::kOpenExisting, nt::kFlagBackupSemantics, {}, &p);
        if (!st.ok())
            return fail(why, LockResult::Unsupported,
                        QStringLiteral("the file system of %1 cannot be determined (its parent folder cannot be opened: %2)")
                            .arg(m_path, parentPath.isEmpty() ? QStringLiteral("the path has no parent") : st.describe()));
        const Status vs = m_s.volume(p, &pv);
        m_s.close(p);
        if (!vs.ok() || !FilesystemPolicy::windowsVolumeSupported(FilesystemPolicy::fromWindowsName(pv.fileSystem), pv.remote))
            return fail(why, LockResult::Unsupported,
                        QStringLiteral("%1 is not on a local NTFS or ReFS volume (%2)")
                            .arg(parentPath, vs.ok() ? (pv.remote ? QStringLiteral("network") : pv.fileSystem) : vs.describe()));
        // Created with its final owner and protected DACL in the one call; only an elevated process can.
        st = m_s.createDirectory(m_path, sd::lockDirectoryDescriptor());
        if (!st.ok() && st.kind != Status::Kind::Exists)
            return fail(why, LockResult::Insecure, QStringLiteral("could not create %1: %2").arg(m_path, st.describe()));
        // Reported the moment it exists, before it is opened and judged: a later refusal leaves it on disk.
        if (st.ok() && m_report) m_report(QStringLiteral("created the lock directory %1").arg(m_path));
        st = m_s.createFile(m_path, access, nt::kShareAll, nt::kOpenExisting, flags, {}, &h);
    }
    if (!st.ok()) {
        if (st.kind == Status::Kind::NotFound) return fail(why, LockResult::Missing, QStringLiteral("%1 does not exist").arg(m_path));
        return fail(why, LockResult::Insecure, QStringLiteral("could not open %1: %2").arg(m_path, st.describe()));
    }
    OwnedHandle dir(m_s, h);
    Volume v;
    st = m_s.volume(h, &v);
    if (!st.ok() || !FilesystemPolicy::windowsVolumeSupported(FilesystemPolicy::fromWindowsName(v.fileSystem), v.remote))
        return fail(why, LockResult::Unsupported,
                    QStringLiteral("%1 is not on a local NTFS or ReFS volume (%2)")
                        .arg(m_path, st.ok() ? (v.remote ? QStringLiteral("network") : v.fileSystem) : st.describe()));
    Facts f;
    if (st = m_s.facts(h, &f); !st.ok())
        return fail(why, LockResult::Insecure, QStringLiteral("could not read %1: %2").arg(m_path, st.describe()));
    if (f.isReparse() || !f.isDirectory())
        return fail(why, LockResult::Insecure, QStringLiteral("%1 is a link, junction or file, not a plain folder").arg(m_path));
    QByteArray sec;
    if (st = m_s.getSecurity(h, nt::kOwnerInfo | nt::kDaclInfo, &sec); !st.ok())
        return fail(why, LockResult::Insecure, QStringLiteral("could not read the permissions of %1: %2").arg(m_path, st.describe()));
    QString w;
    if (!directoryDescriptorTrusted(sec, &w))
        return fail(why, LockResult::Insecure, QStringLiteral("%1 is not secured: %2").arg(m_path, w));
    m_dir = WinDirHandle(std::move(dir), f.id);
    return LockResult::Acquired;
}

bool LockDirectory::volumeSupported(QString* why)
{
    if (!m_dir.valid()) {
        QString w;
        open(/*create*/ false, &w);
        if (!m_dir.valid()) {
            if (why) *why = w;
            return false;
        }
    }
    Volume v;
    const Status st = m_s.volume(m_dir.native(), &v);
    if (!st.ok()) {
        if (why) *why = st.describe();
        return false;
    }
    const FilesystemPolicy::Kind k = FilesystemPolicy::fromWindowsName(v.fileSystem);
    if (FilesystemPolicy::windowsVolumeSupported(k, v.remote)) return true;
    if (why) *why = v.remote ? QStringLiteral("%1 is on a network volume").arg(m_path)
                             : QStringLiteral("%1 is on %2, not NTFS or ReFS").arg(m_path, v.fileSystem);
    return false;
}

LockResult LockDirectory::openLockFile(const QString& name, quint32 access, quint32 disposition, OwnedHandle* out, QString* why)
{
    if (!m_dir.valid()) return fail(why, LockResult::Insecure, QStringLiteral("the lock directory is not open"));
    const QString full = m_path + QLatin1Char('\\') + name;
    Handle h = 0;
    // Share mode 0: while this handle is open nobody else can open the file for reading, writing or deletion.
    Status st = m_s.openRelative(m_dir.native(), name, access | nt::kReadControl | nt::kReadAttributes, 0, disposition,
                                 nt::kNonDirectoryFile | nt::kOpenReparsePoint,
                                 disposition == nt::kFileOpen ? QByteArray() : sd::lockFileDescriptor(), &h);
    if (st.kind == Status::Kind::SharingViolation) return fail(why, LockResult::Busy, QStringLiteral("%1 is held").arg(full));
    if (st.kind == Status::Kind::NotFound) return fail(why, LockResult::Missing, QStringLiteral("%1 does not exist").arg(full));
    if (!st.ok()) return fail(why, LockResult::Insecure, QStringLiteral("could not open %1: %2").arg(full, st.describe()));
    OwnedHandle lock(m_s, h);
    Facts f;
    if (st = m_s.facts(h, &f); !st.ok() || f.isReparse() || f.isDirectory())
        return fail(why, LockResult::Insecure, QStringLiteral("%1 is not a regular file").arg(full));
    QByteArray sec;
    if (st = m_s.getSecurity(h, nt::kOwnerInfo | nt::kDaclInfo, &sec); !st.ok())
        return fail(why, LockResult::Insecure, QStringLiteral("could not read the permissions of %1: %2").arg(full, st.describe()));
    const std::optional<sd::Descriptor> d = sd::parse(sec);
    QString w;
    if (!d) return fail(why, LockResult::Insecure, QStringLiteral("the permissions of %1 cannot be decoded").arg(full));
    if (!sd::isLockFileSecure(*d, &w)) {
        const bool ownerTrusted = d->owner == QLatin1String(sd::kSystem) || d->owner == QLatin1String(sd::kAdministrators);
        if (m_role != LockRole::Applier || !ownerTrusted)
            return fail(why, LockResult::InsecureFile, QStringLiteral("%1 is not secured: %2").arg(full, w));
        // Repair through a separate mutation handle that must be the same file (INV-62 discipline).
        Handle m = 0;
        st = m_s.openRelative(m_dir.native(), name, nt::kReadControl | nt::kWriteDac | nt::kWriteOwner | nt::kReadAttributes,
                              nt::kShareAll, nt::kFileOpen, nt::kNonDirectoryFile | nt::kOpenReparsePoint, {}, &m);
        if (!st.ok()) return fail(why, LockResult::InsecureFile, QStringLiteral("could not open %1 to secure it: %2").arg(full, st.describe()));
        OwnedHandle mutation(m_s, m);
        Facts mf;
        if (st = m_s.facts(m, &mf); !st.ok() || mf.id != f.id)
            return fail(why, LockResult::InsecureFile, QStringLiteral("%1 changed while it was being secured").arg(full));
        st = m_s.setSecurity(m, nt::kOwnerInfo | nt::kDaclInfo, sd::lockFileDescriptor());
        if (!st.ok()) return fail(why, LockResult::InsecureFile, QStringLiteral("could not secure %1: %2").arg(full, st.describe()));
        // Reported the moment it is made, before the read-back: a failed check still leaves the change on disk.
        if (m_report) m_report(QStringLiteral("reset the permissions of %1 (%2)").arg(full, w));
        // Verified, not assumed: read it back through the lock handle.
        QByteArray again;
        const std::optional<sd::Descriptor> d2 =
            m_s.getSecurity(h, nt::kOwnerInfo | nt::kDaclInfo, &again).ok() ? sd::parse(again) : std::nullopt;
        QString w2;
        if (!d2 || !sd::isLockFileSecure(*d2, &w2))
            return fail(why, LockResult::InsecureFile, QStringLiteral("%1 is still not secured: %2").arg(full, w2));
    }
    if (out) *out = std::move(lock);
    return LockResult::Acquired;
}

}  // namespace asb::win
