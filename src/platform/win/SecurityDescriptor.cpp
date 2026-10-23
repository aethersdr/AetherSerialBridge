#include "platform/win/SecurityDescriptor.h"

#include <QStringList>

namespace asb::win::sd {
namespace {

constexpr int kHeaderSize = 20;   // SECURITY_DESCRIPTOR_RELATIVE
constexpr int kAclHeaderSize = 8;
constexpr int kMaxSubAuthorities = 15;

quint16 u16(const QByteArray& b, qsizetype at)
{
    return quint16(quint8(b[at]) | (quint8(b[at + 1]) << 8));
}
quint32 u32(const QByteArray& b, qsizetype at)
{
    return quint32(quint8(b[at])) | (quint32(quint8(b[at + 1])) << 8) | (quint32(quint8(b[at + 2])) << 16) |
           (quint32(quint8(b[at + 3])) << 24);
}
void put16(QByteArray& b, quint16 v)
{
    b.append(char(v & 0xff));
    b.append(char(v >> 8));
}
void put32(QByteArray& b, quint32 v)
{
    for (int i = 0; i < 4; ++i) b.append(char((v >> (8 * i)) & 0xff));
}
void set32(QByteArray& b, qsizetype at, quint32 v)
{
    for (int i = 0; i < 4; ++i) b[at + i] = char((v >> (8 * i)) & 0xff);
}

// The SID at `at`, bounded by `end`; its size in *size.
std::optional<QString> sidAt(const QByteArray& b, qsizetype at, qsizetype end, qsizetype* size)
{
    if (at < 0 || at + 8 > end) return std::nullopt;
    const int count = quint8(b[at + 1]);
    if (quint8(b[at]) != 1 || count > kMaxSubAuthorities) return std::nullopt;
    const qsizetype n = 8 + 4 * qsizetype(count);
    if (at + n > end) return std::nullopt;
    if (size) *size = n;
    return sidFromBinary(b.mid(at, n));
}

quint32 mapGeneric(quint32 m)
{
    quint32 out = m & 0x0FFFFFFF;
    if (m & 0x80000000u) out |= 0x120089;   // GENERIC_READ    -> FILE_GENERIC_READ
    if (m & 0x40000000u) out |= 0x120116;   // GENERIC_WRITE   -> FILE_GENERIC_WRITE
    if (m & 0x20000000u) out |= 0x1200A0;   // GENERIC_EXECUTE -> FILE_GENERIC_EXECUTE
    if (m & 0x10000000u) out |= kFullControl;
    return out;
}

Descriptor protectedDacl(QList<Ace> aces)
{
    Descriptor d;
    d.daclPresent = true;
    d.daclProtected = true;
    d.dacl = std::move(aces);
    return d;
}

}  // namespace

AceKind Ace::kind() const
{
    if (sid.isEmpty()) return AceKind::Unknown;
    if (type == kAccessAllowed) return AceKind::Allow;
    if (type == kAccessDenied) return AceKind::Deny;
    return AceKind::Unknown;
}

std::optional<QByteArray> sidToBinary(const QString& s)
{
    const QStringList parts = s.split(QLatin1Char('-'));
    if (parts.size() < 3 || parts.size() - 3 > kMaxSubAuthorities || parts[0] != QLatin1String("S") ||
        parts[1] != QLatin1String("1"))
        return std::nullopt;
    bool ok = false;
    quint64 authority = parts[2].startsWith(QLatin1String("0x"), Qt::CaseInsensitive) ? parts[2].mid(2).toULongLong(&ok, 16)
                                                                                         : parts[2].toULongLong(&ok, 10);
    if (!ok || authority > 0xFFFFFFFFFFFFull) return std::nullopt;
    QByteArray b;
    b.append(char(1));
    b.append(char(parts.size() - 3));
    for (int i = 5; i >= 0; --i) b.append(char((authority >> (8 * i)) & 0xff));
    for (qsizetype i = 3; i < parts.size(); ++i) {
        const quint64 v = parts[i].toULongLong(&ok, 10);
        if (!ok || v > 0xFFFFFFFFull || parts[i].isEmpty() || !parts[i][0].isDigit()) return std::nullopt;
        put32(b, quint32(v));
    }
    return b;
}

std::optional<QString> sidFromBinary(const QByteArray& b)
{
    if (b.size() < 8 || quint8(b[0]) != 1) return std::nullopt;
    const int count = quint8(b[1]);
    if (count > kMaxSubAuthorities || b.size() != 8 + 4 * count) return std::nullopt;
    quint64 authority = 0;
    for (int i = 2; i < 8; ++i) authority = (authority << 8) | quint8(b[i]);
    QString s = QStringLiteral("S-1-");
    s += authority >> 32 ? QStringLiteral("0x") + QString::number(authority, 16).rightJustified(12, QLatin1Char('0')).toUpper()
                         : QString::number(authority);
    for (int i = 0; i < count; ++i) s += QLatin1Char('-') + QString::number(u32(b, 8 + 4 * i));
    return s;
}

std::optional<Descriptor> parse(const QByteArray& b, QString* error)
{
    auto fail = [error](const char* why) -> std::optional<Descriptor> {
        if (error) *error = QString::fromLatin1(why);
        return std::nullopt;
    };
    const qsizetype end = b.size();
    if (end < kHeaderSize) return fail("security descriptor shorter than its header");
    if (quint8(b[0]) != 1) return fail("security descriptor revision is not 1");
    const quint16 control = u16(b, 2);
    if (!(control & kSelfRelative)) return fail("security descriptor is not self-relative");
    const quint32 ownerOff = u32(b, 4);
    const quint32 daclOff = u32(b, 16);
    Descriptor d;
    if (ownerOff != 0) {
        const std::optional<QString> owner = sidAt(b, qsizetype(ownerOff), end, nullptr);
        if (!owner) return fail("owner SID is malformed");
        d.owner = owner;
    }
    d.daclPresent = (control & kDaclPresent) != 0;
    d.daclProtected = (control & kDaclProtected) != 0;
    d.daclAutoInherited = (control & kDaclAutoInherited) != 0;
    if (!d.daclPresent) return d;
    if (daclOff == 0) {
        d.daclNull = true;
        return d;
    }
    const qsizetype acl = qsizetype(daclOff);
    if (acl + kAclHeaderSize > end) return fail("DACL header out of bounds");
    const quint8 aclRevision = quint8(b[acl]);
    if (aclRevision != 2 && aclRevision != 4) return fail("DACL revision is not 2 or 4");
    const qsizetype aclSize = u16(b, acl + 2);
    const int count = u16(b, acl + 4);
    if (aclSize < kAclHeaderSize || acl + aclSize > end) return fail("DACL size out of bounds");
    const qsizetype aclEnd = acl + aclSize;
    qsizetype at = acl + kAclHeaderSize;
    for (int i = 0; i < count; ++i) {
        if (at + 4 > aclEnd) return fail("ACE header out of bounds");
        Ace a;
        a.type = quint8(b[at]);
        a.flags = quint8(b[at + 1]);
        const qsizetype size = u16(b, at + 2);
        if (size < 4 || at + size > aclEnd) return fail("ACE size out of bounds");
        if (size >= 8) a.mask = u32(b, at + 4);
        if (a.type == kAccessAllowed || a.type == kAccessDenied) {
            qsizetype sidSize = 0;
            const std::optional<QString> sid = sidAt(b, at + 8, at + size, &sidSize);
            if (!sid) return fail("ACE SID is malformed");
            a.sid = *sid;
        }
        // Any other type (object, callback, compound, label, ...) stays sid-less: AceKind::Unknown.
        d.dacl.append(a);
        at += size;
    }
    return d;
}

QByteArray build(const QString& owner, const std::optional<Descriptor>& dacl)
{
    QByteArray ownerBin;
    if (!owner.isEmpty()) {
        const std::optional<QByteArray> o = sidToBinary(owner);
        if (!o) return {};
        ownerBin = *o;
    }
    QByteArray aclBin;
    quint16 control = kSelfRelative;
    if (dacl) {
        control |= kDaclPresent;
        if (dacl->daclProtected) control |= kDaclProtected;
        if (dacl->daclAutoInherited) control |= kDaclAutoInherited;
        QByteArray aces;
        for (const Ace& a : dacl->dacl) {
            if (a.type != kAccessAllowed && a.type != kAccessDenied) return {};
            const std::optional<QByteArray> sid = sidToBinary(a.sid);
            if (!sid) return {};
            aces.append(char(a.type));
            aces.append(char(a.flags));
            put16(aces, quint16(8 + sid->size()));
            put32(aces, a.mask);
            aces.append(*sid);
        }
        aclBin.append(char(2));
        aclBin.append(char(0));
        put16(aclBin, quint16(kAclHeaderSize + aces.size()));
        put16(aclBin, quint16(dacl->dacl.size()));
        put16(aclBin, 0);
        aclBin.append(aces);
    }
    QByteArray out;
    out.append(char(1));
    out.append(char(0));
    put16(out, control);
    put32(out, 0);   // owner
    put32(out, 0);   // group
    put32(out, 0);   // SACL
    put32(out, 0);   // DACL
    if (!ownerBin.isEmpty()) {
        set32(out, 4, quint32(out.size()));
        out.append(ownerBin);
    }
    if (dacl && !dacl->daclNull) {   // a NULL DACL is "present" with offset 0
        set32(out, 16, quint32(out.size()));
        out.append(aclBin);
    }
    return out;
}

QList<AceInfo> toAceInfos(const Descriptor& d)
{
    QList<AceInfo> out;
    if (!d.daclPresent || d.daclNull) {
        out.append(AceInfo{QStringLiteral("S-1-1-0"), AceJudge::GenericAll, AceKind::Unknown});
        return out;
    }
    for (const Ace& a : d.dacl) out.append(AceInfo{a.sid, a.mask, a.kind()});
    return out;
}

QList<Ace> inheritedForFile(const QList<Ace>& parentDacl, const QString& owner)
{
    QList<Ace> out;
    for (const Ace& a : parentDacl) {
        if (!(a.flags & kObjectInherit)) continue;
        Ace c = a;
        c.flags = kInherited;
        c.mask = mapGeneric(a.mask);
        if (c.sid == QLatin1String(kCreatorOwner)) c.sid = owner;
        out.append(c);
    }
    return out;
}

QByteArray dataFolderDescriptor()
{
    const quint8 inherit = kObjectInherit | kContainerInherit;
    return build(QString::fromLatin1(kAdministrators),
                 protectedDacl({{kAccessAllowed, inherit, kFullControl, QString::fromLatin1(kSystem)},
                                {kAccessAllowed, inherit, kFullControl, QString::fromLatin1(kAdministrators)},
                                {kAccessAllowed, inherit, kReadExecute, QString::fromLatin1(kUsers)}}));
}

QByteArray lockFileDescriptor()
{
    return build(QString::fromLatin1(kAdministrators),
                 protectedDacl({{kAccessAllowed, 0, kFullControl, QString::fromLatin1(kSystem)},
                                {kAccessAllowed, 0, kFullControl, QString::fromLatin1(kAdministrators)}}));
}

QByteArray lockDirectoryDescriptor()
{
    return dataFolderDescriptor();   // the same entries; kept separate so either can change alone
}

QByteArray ownerOnlyDescriptor(const QString& owner)
{
    return build(owner);
}

bool isLockFileSecure(const Descriptor& d, QString* why)
{
    auto fail = [why](const QString& w) {
        if (why) *why = w;
        return false;
    };
    const QString sy = QString::fromLatin1(kSystem), ba = QString::fromLatin1(kAdministrators);
    if (!d.owner || (*d.owner != sy && *d.owner != ba))
        return fail(QStringLiteral("owned by %1, not SYSTEM or Administrators").arg(d.owner.value_or(QStringLiteral("nobody"))));
    if (!d.daclPresent || d.daclNull || d.dacl.isEmpty()) return fail(QStringLiteral("it has no access list (everyone may open it)"));
    if (!d.daclProtected) return fail(QStringLiteral("its access list is not protected from inheritance"));
    for (const Ace& a : d.dacl)
        if (a.kind() != AceKind::Allow || (a.sid != sy && a.sid != ba))
            return fail(QStringLiteral("its access list has an entry other than SYSTEM or Administrators"));
    return true;
}

bool sameEntries(const QList<Ace>& a, const QList<Ace>& b)
{
    return a == b;
}

QString toDisplay(const Descriptor& d)
{
    QString s = QStringLiteral("O:%1 D:").arg(d.owner.value_or(QStringLiteral("-")));
    if (!d.daclPresent) return s + QStringLiteral("<absent>");
    if (d.daclNull) return s + QStringLiteral("<null>");
    if (d.daclProtected) s += QLatin1Char('P');
    if (d.daclAutoInherited) s += QStringLiteral("AI");
    for (const Ace& a : d.dacl)
        s += QStringLiteral("(%1;0x%2;0x%3;%4)")
                 .arg(a.type)
                 .arg(a.flags, 0, 16)
                 .arg(a.mask, 0, 16)
                 .arg(a.sid.isEmpty() ? QStringLiteral("?") : a.sid);
    return s;
}

}  // namespace asb::win::sd
