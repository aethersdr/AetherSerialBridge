#include "platform/posix/PosixDataFolder.h"

#include "core/ConfigFiles.h"
#include "core/FilesystemPolicy.h"

#include <QDateTime>
#include <QRandomGenerator>

#include <cerrno>

namespace asb::posix {
namespace {

FileId idOf(const struct stat& st)
{
    return FileId::fromPosix(quint64(st.st_dev), quint64(st.st_ino));
}

QString ownerName(uid_t uid)
{
    return QStringLiteral("uid:%1").arg(qulonglong(uid));
}

// The bridge's own files in the folder (config, pending, token): the ones normalise() keeps readable for the
// DynamicUser service.
bool isNormalised(const QString& name)
{
    return name == QLatin1String(ConfigFiles::kConfig) || name == QLatin1String(ConfigFiles::kPending) ||
           name == QLatin1String(ConfigFiles::kStartToken);
}

}  // namespace

PosixDataFolder::PosixDataFolder(QString path, Options options, PosixSys& sys)
    : m_sys(sys), m_path(std::move(path)), m_opt(std::move(options))
{
    while (m_path.size() > 1 && m_path.endsWith(QLatin1Char('/'))) m_path.chop(1);
    const qsizetype slash = m_path.lastIndexOf(QLatin1Char('/'));
    m_parent = slash <= 0 ? QStringLiteral("/") : m_path.left(slash);
    m_name = m_path.mid(slash + 1);
    if (!m_opt.asideSuffix) m_opt.asideSuffix = &PosixDataFolder::defaultAsideSuffix;
}

QString PosixDataFolder::defaultAsideSuffix()
{
    return QStringLiteral("untrusted-%1-%2")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")))
        .arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
}

FsEntry PosixDataFolder::entryFor(const QString& name, const struct stat& st)
{
    FsEntry e;
    e.name = name;
    if (S_ISLNK(st.st_mode)) e.kind = FsEntry::Kind::Link;
    else if (S_ISDIR(st.st_mode)) e.kind = FsEntry::Kind::Directory;
    else if (S_ISREG(st.st_mode)) e.kind = FsEntry::Kind::File;
    else e.kind = FsEntry::Kind::Other;
    e.owner = ownerName(st.st_uid);
    if (st.st_mode & S_IWGRP)
        e.acl << AceInfo{QStringLiteral("group:%1").arg(qulonglong(st.st_gid)), AceJudge::WriteData, AceKind::Allow};
    if (st.st_mode & S_IWOTH) e.acl << AceInfo{QStringLiteral("other"), AceJudge::WriteData, AceKind::Allow};
    return e;
}

TrustPolicy PosixDataFolder::policyFor(TrustMode mode, const Options& o)
{
    TrustPolicy p;
    p.trustedOwners << ownerName(o.configOwner.uid);
    if (mode == TrustMode::Scratch && o.scratchUser != uid_t(-1)) p.alsoTrusted = ownerName(o.scratchUser);
    return p;
}

int PosixDataFolder::openParent()
{
    const QByteArray p = m_parent.toLocal8Bit();
    return m_sys.open(p.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

Inspection PosixDataFolder::inspect(TrustMode mode)
{
    Inspection out;
    m_children.clear();
    m_dir = PosixDirHandle();
    m_mode = mode;
    Fd parent(m_sys, openParent());
    if (!parent) {
        out.action = DirAction::Refuse;
        out.reason = QStringLiteral("the parent folder %1 cannot be opened: %2").arg(m_parent, errnoText());
        return out;
    }
    const QByteArray n = m_name.toLocal8Bit();
    const int fd = m_sys.openat(parent.get(), n.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        const int e = errno;
        if (e == ENOENT) {
            // Nothing there. Where it would be created must itself be a supported file system.
            struct statfs fs {};
            if (m_sys.fstatfs(parent.get(), &fs) == 0 &&
                !FilesystemPolicy::linuxDataFolderSupported(FilesystemPolicy::fromLinuxMagic(quint64(fs.f_type)))) {
                out.action = DirAction::Refuse;
                out.reason = QStringLiteral("%1 is on %2, which is not supported")
                                 .arg(m_parent, FilesystemPolicy::name(FilesystemPolicy::fromLinuxMagic(quint64(fs.f_type))));
                return out;
            }
            out.action = DirAction::Create;
            return out;
        }
        out.action = DirAction::Refuse;
        out.reason = (e == ELOOP || e == ENOTDIR)
                         ? QStringLiteral("%1 is a symbolic link or a file, not a plain folder").arg(m_path)
                         : QStringLiteral("%1 cannot be opened: %2").arg(m_path, errnoText(e));
        return out;
    }
    m_dir.reset(m_sys, fd);
    struct stat st {};
    if (m_sys.fstat(fd, &st) != 0) {
        out.action = DirAction::Refuse;
        out.reason = QStringLiteral("%1 cannot be examined: %2").arg(m_path, errnoText());
        return out;
    }
    out.folderId = idOf(st);
    struct statfs fs {};
    if (m_sys.fstatfs(fd, &fs) != 0) {
        out.action = DirAction::Refuse;
        out.reason = QStringLiteral("the file system of %1 cannot be determined: %2").arg(m_path, errnoText());
        return out;
    }
    const FilesystemPolicy::Kind kind = FilesystemPolicy::fromLinuxMagic(quint64(fs.f_type));
    if (!FilesystemPolicy::linuxDataFolderSupported(kind)) {
        out.action = DirAction::Refuse;
        out.reason = QStringLiteral("%1 is on %2, which is not supported").arg(m_path, FilesystemPolicy::name(kind));
        return out;
    }
    const FsEntry folder = entryFor(m_name, st);

    QList<FsEntry> children;
    const std::optional<QStringList> names = m_sys.listDirectory(fd);
    if (!names) {
        out.action = DirAction::Recreate;
        out.reason = QStringLiteral("the folder cannot be listed: %1").arg(errnoText());
        return out;
    }
    for (const QString& c : *names) {
        out.children << c;
        struct stat cs {};
        const QByteArray cn = c.toLocal8Bit();
        if (m_sys.fstatat(fd, cn.constData(), &cs, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) {
                children << FsEntry::gone(c);   // listed, then gone: not evidence either way
                continue;
            }
            FsEntry unreadable;
            unreadable.name = c;
            unreadable.kind = FsEntry::Kind::Other;   // owner nullopt: untrusted
            children << unreadable;
            continue;
        }
        children << entryFor(c, cs);
        if (S_ISREG(cs.st_mode)) m_children.insert(c, idOf(cs));
    }
    const DataDirectoryTrust::Decision d = DataDirectoryTrust::decide(folder, children, policyFor(mode, m_opt));
    out.action = d.action;
    out.reason = d.reason;
    return out;
}

bool PosixDataFolder::sameFolder(const Inspection& insp) const
{
    return m_dir.valid() && m_dir.id() == insp.folderId && insp.folderId != FileId{};
}

std::optional<QByteArray> PosixDataFolder::readFile(const Inspection& insp, const QString& name, qint64 cap, QString* err)
{
    if (!sameFolder(insp)) {
        if (err) *err = QStringLiteral("the data folder changed since it was inspected");
        return std::nullopt;
    }
    if (name.isEmpty() || name.contains(QLatin1Char('/')) || name == QLatin1String(".") || name == QLatin1String("..")) {
        if (err) *err = QStringLiteral("not a file name: %1").arg(name);
        return std::nullopt;
    }
    const QByteArray n = name.toLocal8Bit();
    Fd fd(m_sys, m_sys.openat(m_dir.fd(), n.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (!fd) {
        const int e = errno;
        if (err) *err = e == ENOENT ? fileNotFoundError()
                                    : (e == ELOOP ? QStringLiteral("%1 is a symbolic link").arg(name)
                                                  : QStringLiteral("%1 cannot be opened: %2").arg(name, errnoText(e)));
        return std::nullopt;
    }
    // Every check is made on the fd that the bytes are read from (INV-65): never on the name.
    struct stat st {};
    if (m_sys.fstat(fd.get(), &st) != 0) {
        if (err) *err = QStringLiteral("%1 cannot be examined: %2").arg(name, errnoText());
        return std::nullopt;
    }
    if (!S_ISREG(st.st_mode)) {
        if (err) *err = QStringLiteral("%1 is not a regular file").arg(name);
        return std::nullopt;
    }
    // The owners the inspection trusted, no others: the Config owner (root), and in Scratch mode also the
    // invoking user (policyFor: WS-9's `--run --config` is the first Scratch reader).
    const bool scratchMode = m_mode == TrustMode::Scratch && m_opt.scratchUser != uid_t(-1);
    const bool scratchOwner = scratchMode && st.st_uid == m_opt.scratchUser;
    if (st.st_uid != m_opt.configOwner.uid && !scratchOwner) {
        // Every owner that would have been trusted: in Scratch mode the invoking user too.
        QString owners = QStringLiteral("uid %1").arg(qulonglong(m_opt.configOwner.uid));
        if (scratchMode && m_opt.scratchUser != m_opt.configOwner.uid)
            owners += QStringLiteral(" or uid %1 (the user running this command)").arg(qulonglong(m_opt.scratchUser));
        if (err) *err = QStringLiteral("%1 is owned by uid %2, not by %3").arg(name).arg(qulonglong(st.st_uid)).arg(owners);
        return std::nullopt;
    }
    if (st.st_mode & (S_IWGRP | S_IWOTH)) {
        if (err) *err = QStringLiteral("%1 can be changed by other accounts (mode %2)").arg(name).arg(st.st_mode & 07777, 4, 8, QLatin1Char('0'));
        return std::nullopt;
    }
    QString e;
    std::optional<QByteArray> bytes = preadAll(m_sys, fd.get(), cap, &e);
    if (!bytes && err) *err = QStringLiteral("%1: %2").arg(name, e);
    return bytes;
}

void PosixDataFolder::createLockedDown()
{
    Fd parent(m_sys, openParent());
    if (!parent) throw PlatformError(QStringLiteral("cannot open %1: %2").arg(m_parent, errnoText()).toStdString());
    const QByteArray n = m_name.toLocal8Bit();
    if (m_sys.mkdirat(parent.get(), n.constData(), 0755) != 0)
        throw PlatformError(QStringLiteral("cannot create %1: %2").arg(m_path, errnoText()).toStdString());
    // From here the folder exists: a later failure reports it as made (master §4.6 P4, PlatformErrorAfterChanges),
    // and the operation stops instead of saving into a folder whose own entry may not survive a crash.
    const QStringList made{QStringLiteral("created the data folder")};
    const auto after = [&](const QString& what) {
        return PlatformErrorAfterChanges(QStringLiteral("%1: %2").arg(what, errnoText()).toStdString(), made);
    };
    Fd fd(m_sys, m_sys.openat(parent.get(), n.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!fd) throw after(QStringLiteral("cannot open the new %1").arg(m_path));
    struct stat st {};
    if (m_sys.fstat(fd.get(), &st) != 0) throw after(QStringLiteral("cannot examine the new %1").arg(m_path));
    if (st.st_uid != m_opt.configOwner.uid && m_sys.fchown(fd.get(), m_opt.configOwner.uid, m_opt.configOwner.gid) != 0)
        throw after(QStringLiteral("cannot set the owner of %1").arg(m_path));
    if (m_sys.fchmod(fd.get(), 0755) != 0) throw after(QStringLiteral("cannot set the mode of %1").arg(m_path));
    // The folder's entry is durable before anything is written into it (the result counts).
    if (m_sys.fsync(parent.get()) != 0)
        throw after(QStringLiteral("cannot flush %1 after creating %2").arg(m_parent, m_name));
}

QStringList PosixDataFolder::normalise(const Inspection& insp)
{
    QStringList changes;
    if (!sameFolder(insp))
        throw PlatformError(QStringLiteral("the data folder changed since it was inspected; nothing was changed").toStdString());
    struct stat st {};
    if (m_sys.fstat(m_dir.fd(), &st) != 0 || idOf(st) != insp.folderId)
        throw PlatformError(QStringLiteral("the data folder changed since it was inspected; nothing was changed").toStdString());
    // A failure after a change carries every change already made (WS-6's PlatformErrorAfterChanges, INV-76).
    const auto fail = [&changes](const QString& what) {
        if (changes.isEmpty()) throw PlatformError(what.toStdString());
        throw PlatformErrorAfterChanges(what.toStdString(), changes);
    };
    if ((st.st_mode & 07777) != 0755) {
        if (m_sys.fchmod(m_dir.fd(), 0755) != 0)
            fail(QStringLiteral("cannot set the mode of %1: %2").arg(m_path, errnoText()));
        changes << QStringLiteral("%1: mode %2 -> 0755").arg(m_path).arg(st.st_mode & 07777, 4, 8, QLatin1Char('0'));
    }
    for (auto it = m_children.cbegin(); it != m_children.cend(); ++it) {
        if (!isNormalised(it.key())) continue;
        const QByteArray n = it.key().toLocal8Bit();
        Fd fd(m_sys, m_sys.openat(m_dir.fd(), n.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        if (!fd) {
            if (errno == ENOENT) continue;   // gone since the inspection: nothing to secure
            fail(QStringLiteral("cannot open %1: %2").arg(it.key(), errnoText()));
        }
        struct stat cs {};
        if (m_sys.fstat(fd.get(), &cs) != 0 || idOf(cs) != it.value() || !S_ISREG(cs.st_mode))
            fail(QStringLiteral("%1 changed since the folder was inspected; nothing more was changed").arg(it.key()));
        if ((cs.st_mode & 07777) != 0644) {
            if (m_sys.fchmod(fd.get(), 0644) != 0)
                fail(QStringLiteral("cannot set the mode of %1: %2").arg(it.key(), errnoText()));
            changes << QStringLiteral("%1: mode %2 -> 0644").arg(it.key()).arg(cs.st_mode & 07777, 4, 8, QLatin1Char('0'));
        }
    }
    return changes;
}

QStringList PosixDataFolder::knownFiles() const
{
    return {QString::fromLatin1(ConfigFiles::kConfig), QString::fromLatin1(ConfigFiles::kPending),
            QString::fromLatin1(ConfigFiles::kStartToken)};
}

bool PosixDataFolder::removeIfEmpty(const Inspection& insp, QString* err)
{
    Fd parent(m_sys, openParent());
    if (!parent) {
        *err = QStringLiteral("cannot open %1: %2").arg(m_parent, errnoText());
        return false;
    }
    const QByteArray n = m_name.toLocal8Bit();
    struct stat st {};
    if (m_sys.fstatat(parent.get(), n.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode) ||
        idOf(st) != insp.folderId || insp.folderId == FileId{}) {
        *err = QStringLiteral("%1 changed since it was inspected").arg(m_path);
        return false;
    }
    if (m_sys.unlinkat(parent.get(), n.constData(), AT_REMOVEDIR) != 0) {
        *err = errnoText();
        return false;
    }
    m_dir = PosixDirHandle();
    m_children.clear();
    m_sys.fsync(parent.get());
    return true;
}

QStringList PosixDataFolder::asideFolders()
{
    QStringList out;
    Fd parent(m_sys, openParent());
    if (!parent) return out;
    const std::optional<QStringList> names = m_sys.listDirectory(parent.get());
    if (!names) return out;
    const QString prefix = m_name + QStringLiteral(".untrusted-");
    const QString base = m_parent == QLatin1String("/") ? QString() : m_parent;
    for (const QString& n : *names)
        if (n.startsWith(prefix)) out << base + QLatin1Char('/') + n;
    out.sort();
    return out;
}

QString PosixDataFolder::renameAside(const Inspection& insp)
{
    Fd parent(m_sys, openParent());
    if (!parent) throw PlatformError(QStringLiteral("cannot open %1: %2").arg(m_parent, errnoText()).toStdString());
    const QByteArray n = m_name.toLocal8Bit();
    struct stat st {};
    if (m_sys.fstatat(parent.get(), n.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0)
        throw PlatformError(QStringLiteral("cannot examine %1: %2").arg(m_path, errnoText()).toStdString());
    if (!S_ISDIR(st.st_mode) || idOf(st) != insp.folderId || insp.folderId == FileId{})
        throw PlatformError(QStringLiteral("%1 changed since it was inspected; it was not moved").arg(m_path).toStdString());
    const QString aside = m_name + QLatin1Char('.') + m_opt.asideSuffix();
    const QByteArray a = aside.toLocal8Bit();
    if (m_sys.renameNoReplace(parent.get(), n.constData(), parent.get(), a.constData()) != 0)
        throw PlatformError(QStringLiteral("cannot move %1 aside: %2").arg(m_path, errnoText()).toStdString());
    m_dir = PosixDirHandle();
    m_children.clear();
    const QString where = (m_parent == QLatin1String("/") ? QString() : m_parent) + QLatin1Char('/') + aside;
    if (m_sys.fsync(parent.get()) != 0)   // moved, but the move may not survive a crash: reported, and the step fails
        throw PlatformErrorAfterChanges(QStringLiteral("cannot flush %1 after moving the folder aside: %2").arg(m_parent, errnoText()).toStdString(),
                                        {QStringLiteral("moved the untrusted folder to %1 (nothing was deleted)").arg(where)});
    return where;
}

}  // namespace asb::posix
