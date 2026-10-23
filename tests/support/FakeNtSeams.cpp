#include "support/FakeNtSeams.h"

#include "platform/win/SecurityDescriptor.h"

namespace asb::test {

using win::Handle;
using win::Status;
namespace nt = win::nt;
namespace sd = win::sd;

namespace {

QStringList components(QString path)
{
    path.replace(QLatin1Char('/'), QLatin1Char('\\'));
    QStringList out;
    for (const QString& c : path.split(QLatin1Char('\\'), Qt::SkipEmptyParts)) out << c;
    return out;
}

void dataBits(quint32 a, bool* r, bool* w, bool* d)
{
    *r = (a & (nt::kReadData | nt::kGenericRead | 0x20u /*FILE_EXECUTE*/)) != 0;
    *w = (a & (nt::kWriteData | nt::kAppendData | nt::kGenericWrite)) != 0;
    *d = (a & nt::kDelete) != 0;
}

QList<sd::Ace> inheritedForDir(const QList<sd::Ace>& parent)
{
    QList<sd::Ace> out;
    for (const sd::Ace& a : parent) {
        sd::Ace c = a;
        if (a.flags & sd::kContainerInherit) {
            c.flags = sd::kInherited |
                      ((a.flags & sd::kNoPropagate) ? 0 : (a.flags & (sd::kObjectInherit | sd::kContainerInherit | sd::kInheritOnly)));
        } else if ((a.flags & sd::kObjectInherit) && !(a.flags & sd::kNoPropagate)) {
            c.flags = sd::kInherited | sd::kObjectInherit | sd::kInheritOnly;
        } else {
            continue;
        }
        out.append(c);
    }
    return out;
}

}  // namespace

FakeNtSeams::FakeNtSeams() : defaultOwner(QString::fromLatin1(sd::kAdministrators))
{
    m_root = std::make_shared<Node>();
    m_root->kind = Node::Kind::Dir;
    sd::Descriptor d;
    d.daclPresent = true;
    const quint8 inherit = sd::kObjectInherit | sd::kContainerInherit;
    d.dacl = {{sd::kAccessAllowed, inherit, sd::kFullControl, QString::fromLatin1(sd::kSystem)},
              {sd::kAccessAllowed, inherit, sd::kFullControl, QString::fromLatin1(sd::kAdministrators)},
              {sd::kAccessAllowed, inherit, sd::kReadExecute, QString::fromLatin1(sd::kUsers)}};
    auto drive = std::make_shared<Node>();
    drive->kind = Node::Kind::Dir;
    drive->id.volume = 0x1234;
    drive->id.id[0] = 0xFF;
    drive->sd = sd::build(QString::fromLatin1(sd::kSystem), d);
    attach(m_root.get(), QStringLiteral("C:"), drive);
}

// ---- tree ---------------------------------------------------------------------------------------

void FakeNtSeams::attach(Node* parent, const QString& name, const NodeP& child)
{
    parent->children.insert(name.toLower(), child);
    parent->names.insert(name.toLower(), name);
    child->parent = parent;
}

void FakeNtSeams::detach(Node* n)
{
    if (!n->parent) return;
    Node* p = n->parent;
    for (auto it = p->children.begin(); it != p->children.end(); ++it) {
        if (it->get() == n) {
            p->names.remove(it.key());
            p->children.erase(it);
            break;
        }
    }
    n->parent = nullptr;
}

FakeNtSeams::NodeP FakeNtSeams::resolve(const QString& path, bool followFinal, NodeP* parentOut, QString* leaf) const
{
    const QStringList parts = components(path);
    if (parentOut) *parentOut = nullptr;
    if (parts.isEmpty()) return nullptr;
    NodeP cur = m_root;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const bool last = i == parts.size() - 1;
        if (cur->kind != Node::Kind::Dir) return nullptr;
        if (last) {
            if (parentOut) *parentOut = cur;
            if (leaf) *leaf = parts[i];
        }
        NodeP next = cur->children.value(parts[i].toLower());
        if (!next) return nullptr;
        if (next->kind == Node::Kind::Link && (!last || followFinal)) {
            next = resolve(next->linkTarget, true, nullptr, nullptr);
            if (!next) return nullptr;
        }
        cur = next;
    }
    return cur;
}

FakeNtSeams::NodeP FakeNtSeams::node(const QString& path) const
{
    return resolve(path, false, nullptr, nullptr);
}

QString FakeNtSeams::pathOf(const Node* n) const
{
    QStringList parts;
    while (n && n->parent) {
        const Node* p = n->parent;
        QString name;
        for (auto it = p->children.cbegin(); it != p->children.cend(); ++it)
            if (it->get() == n) name = p->names.value(it.key());
        parts.prepend(name);
        n = p;
    }
    if (n != m_root.get()) return QStringLiteral("<detached>");
    return parts.join(QLatin1Char('\\'));
}

FakeNtSeams::NodeP FakeNtSeams::makeNode(Node::Kind kind, const QByteArray& givenSd, const Node* parent)
{
    auto n = std::make_shared<Node>();
    n->kind = kind;
    n->id.volume = 0x1234;
    const quint64 id = m_nextId++;
    for (int i = 0; i < 8; ++i) n->id.id[size_t(i)] = quint8(id >> (8 * i));
    const std::optional<sd::Descriptor> g = givenSd.isEmpty() ? std::nullopt : sd::parse(givenSd);
    const QString owner = g && g->owner ? *g->owner : defaultOwner;
    sd::Descriptor d;
    if (g && g->daclPresent) {
        d = *g;
    } else {
        const std::optional<sd::Descriptor> p = parent ? sd::parse(parent->sd) : std::nullopt;
        const QList<sd::Ace> pd = p ? p->dacl : QList<sd::Ace>{};
        d.daclPresent = true;
        d.daclAutoInherited = true;
        d.dacl = kind == Node::Kind::Dir ? inheritedForDir(pd) : sd::inheritedForFile(pd, owner);
    }
    n->sd = sd::build(owner, d);
    return n;
}

FakeNtSeams::NodeP FakeNtSeams::newDir(const QByteArray& sdBytes)
{
    return makeNode(Node::Kind::Dir, sdBytes, nullptr);
}

FakeNtSeams::NodeP FakeNtSeams::mkdir(const QString& path, const QByteArray& sdBytes)
{
    NodeP parent;
    QString leaf;
    resolve(path, false, &parent, &leaf);
    Q_ASSERT(parent);
    NodeP n = makeNode(Node::Kind::Dir, sdBytes, parent.get());
    attach(parent.get(), leaf, n);
    return n;
}

FakeNtSeams::NodeP FakeNtSeams::plantFile(const QString& path, const QByteArray& data, const QByteArray& sdBytes)
{
    NodeP parent;
    QString leaf;
    resolve(path, false, &parent, &leaf);
    Q_ASSERT(parent);
    NodeP n = makeNode(Node::Kind::File, sdBytes, parent.get());
    n->data = data;
    attach(parent.get(), leaf, n);
    return n;
}

FakeNtSeams::NodeP FakeNtSeams::plantLink(const QString& path, const QString& target, bool directory)
{
    NodeP parent;
    QString leaf;
    resolve(path, false, &parent, &leaf);
    Q_ASSERT(parent);
    NodeP n = makeNode(Node::Kind::Link, {}, parent.get());
    n->linkTarget = target;
    n->dirLink = directory;
    attach(parent.get(), leaf, n);
    return n;
}

void FakeNtSeams::replace(const QString& path, const NodeP& replacement)
{
    NodeP parent;
    QString leaf;
    NodeP old = resolve(path, false, &parent, &leaf);
    Q_ASSERT(parent);
    if (old) detach(old.get());
    attach(parent.get(), leaf, replacement);
}

void FakeNtSeams::setSd(const QString& path, const QByteArray& sdBytes)
{
    NodeP n = node(path);
    Q_ASSERT(n);
    n->sd = sdBytes;
}

QStringList FakeNtSeams::namesIn(const QString& path) const
{
    NodeP n = node(path);
    return n ? n->names.values() : QStringList{};
}

int FakeNtSeams::callCount(const QString& op) const
{
    return int(callsOf(op).size());
}

QList<FakeNtSeams::Call> FakeNtSeams::callsOf(const QString& op) const
{
    QList<Call> out;
    for (const Call& c : calls)
        if (c.op == op) out << c;
    return out;
}

// ---- helpers ------------------------------------------------------------------------------------

std::optional<Status> FakeNtSeams::preflight(const Call& c)
{
    calls.append(c);
    if (before) before(c);
    if (inject)
        if (std::optional<Status> s = inject(c)) return s;
    return std::nullopt;
}

FakeNtSeams::Open* FakeNtSeams::find(Handle h)
{
    auto it = m_handles.find(h);
    return it == m_handles.end() ? nullptr : &it.value();
}

Status FakeNtSeams::shareCheck(const NodeP& n, quint32 access, quint32 share) const
{
    bool r = false, w = false, d = false;
    dataBits(access, &r, &w, &d);
    if (!r && !w && !d) return Status::success();   // attribute/security-only opens are not share-checked
    for (const Open& o : m_handles) {
        if (o.node != n) continue;
        bool orr = false, ow = false, od = false;
        dataBits(o.access, &orr, &ow, &od);
        if (!orr && !ow && !od) continue;
        if ((r && !(o.share & nt::kShareRead)) || (w && !(o.share & nt::kShareWrite)) || (d && !(o.share & nt::kShareDelete)) ||
            (orr && !(share & nt::kShareRead)) || (ow && !(share & nt::kShareWrite)) || (od && !(share & nt::kShareDelete)))
            return Status::fail(Status::Kind::SharingViolation, 0xC0000043u, QStringLiteral("sharing violation"));
    }
    return Status::success();
}

Status FakeNtSeams::openNode(const NodeP& n, quint32 access, quint32 share, Handle* out)
{
    if (Status s = shareCheck(n, access, share); !s.ok()) return s;
    const Handle h = m_next;
    m_next += 4;
    m_handles.insert(h, Open{n, access, share});
    *out = h;
    return Status::success();
}

// ---- NtSeams ------------------------------------------------------------------------------------

Status FakeNtSeams::createFile(const QString& path, quint32 access, quint32 share, quint32 disposition, quint32 flags,
                               const QByteArray& sdBytes, Handle* out)
{
    Call c{QStringLiteral("createFile"), path, {}, access, share, disposition, flags, sdBytes, 0};
    if (auto s = preflight(c)) return *s;
    NodeP parent;
    QString leaf;
    NodeP n = resolve(path, !(flags & nt::kFlagOpenReparsePoint), &parent, &leaf);
    if (!parent) return Status::fail(Status::Kind::NotFound, 3, QStringLiteral("path not found"));
    if (n && n->deletePending) return Status::fail(Status::Kind::DeletePending, 303, QStringLiteral("delete pending"));
    if (!n) {
        if (disposition == nt::kOpenExisting) return Status::fail(Status::Kind::NotFound, 2, QStringLiteral("file not found"));
        n = makeNode(Node::Kind::File, sdBytes, parent.get());
        attach(parent.get(), leaf, n);
    } else if (disposition == nt::kCreateNew) {
        return Status::fail(Status::Kind::Exists, 80, QStringLiteral("file exists"));
    }
    if ((n->kind == Node::Kind::Dir || n->dirLink) && !(flags & nt::kFlagBackupSemantics))
        return Status::fail(Status::Kind::AccessDenied, 5, QStringLiteral("access denied"));
    return openNode(n, access, share, out);
}

Status FakeNtSeams::openRelative(Handle dir, const QString& name, quint32 access, quint32 share, quint32 disposition,
                                 quint32 options, const QByteArray& sdBytes, Handle* out)
{
    Open* d = find(dir);
    Call c{QStringLiteral("openRelative"), (d ? pathOf(d->node.get()) : QStringLiteral("?")) + QLatin1Char('\\') + name, name,
           access, share, disposition, options, sdBytes, dir};
    if (auto s = preflight(c)) return *s;
    d = find(dir);
    if (!d) return Status::fail(Status::Kind::Other, 6, QStringLiteral("invalid handle"));
    if (d->node->kind != Node::Kind::Dir) return Status::fail(Status::Kind::Other, 0xC0000103u, QStringLiteral("not a directory"));
    if (name.isEmpty()) return openNode(d->node, access, share, out);
    if (name.contains(QLatin1Char('\\')) || name.contains(QLatin1Char('/')))
        return Status::fail(Status::Kind::Other, 0xC0000033u, QStringLiteral("invalid name"));
    NodeP parent = d->node;
    NodeP n = parent->children.value(name.toLower());
    if (n && n->deletePending) return Status::fail(Status::Kind::DeletePending, 0xC0000056u, QStringLiteral("delete pending"));
    if (!n) {
        if (disposition == nt::kFileOpen) return Status::fail(Status::Kind::NotFound, 0xC0000034u, QStringLiteral("not found"));
        n = makeNode((options & nt::kDirectoryFile) ? Node::Kind::Dir : Node::Kind::File, sdBytes, parent.get());
        attach(parent.get(), name, n);
        return openNode(n, access, share, out);
    }
    if (disposition == nt::kFileCreate) return Status::fail(Status::Kind::Exists, 0xC0000035u, QStringLiteral("name collision"));
    if (n->kind == Node::Kind::Link && !(options & nt::kOpenReparsePoint)) {
        n = resolve(n->linkTarget, true, nullptr, nullptr);
        if (!n) return Status::fail(Status::Kind::NotFound, 0xC0000034u, QStringLiteral("not found"));
    }
    const bool isDir = n->kind == Node::Kind::Dir || n->dirLink;
    if ((options & nt::kNonDirectoryFile) && isDir)
        return Status::fail(Status::Kind::IsDirectory, 0xC00000BAu, QStringLiteral("file is a directory"));
    if ((options & nt::kDirectoryFile) && !isDir)
        return Status::fail(Status::Kind::Other, 0xC0000103u, QStringLiteral("not a directory"));
    return openNode(n, access, share, out);
}

Status FakeNtSeams::createDirectory(const QString& path, const QByteArray& sdBytes)
{
    Call c{QStringLiteral("createDirectory"), path, {}, 0, 0, 0, 0, sdBytes, 0};
    if (auto s = preflight(c)) return *s;
    NodeP parent;
    QString leaf;
    NodeP n = resolve(path, false, &parent, &leaf);
    if (!parent) return Status::fail(Status::Kind::NotFound, 3, QStringLiteral("path not found"));
    if (n) return Status::fail(Status::Kind::Exists, 183, QStringLiteral("already exists"));
    attach(parent.get(), leaf, makeNode(Node::Kind::Dir, sdBytes, parent.get()));
    return Status::success();
}

void FakeNtSeams::close(Handle h)
{
    Open* o = find(h);
    if (o && o->node->lockedBy == h) o->node->lockedBy = 0;
    if (o && o->deleteOnClose && o->node->parent) detach(o->node.get());
    m_handles.remove(h);
}

Status FakeNtSeams::facts(Handle h, win::Facts* f)
{
    Open* o = find(h);
    Call c{QStringLiteral("facts"), o ? pathOf(o->node.get()) : QString(), {}, 0, 0, 0, 0, {}, h};
    if (auto s = preflight(c)) return *s;
    o = find(h);
    if (!o) return Status::fail(Status::Kind::Other, 6, QStringLiteral("invalid handle"));
    f->id = o->node->id;
    f->attributes = o->node->kind == Node::Kind::Dir ? nt::kAttrDirectory : 0x20u /*ARCHIVE*/;
    if (o->node->kind == Node::Kind::Link) f->attributes = nt::kAttrReparsePoint | (o->node->dirLink ? nt::kAttrDirectory : 0);
    f->reparseTag = o->node->kind == Node::Kind::Link ? (o->node->dirLink ? 0xA0000003u : 0xA000000Cu) : 0;
    f->size = o->node->data.size();
    return Status::success();
}

Status FakeNtSeams::getSecurity(Handle h, quint32 info, QByteArray* out)
{
    Open* o = find(h);
    Call c{QStringLiteral("getSecurity"), o ? pathOf(o->node.get()) : QString(), {}, info, 0, 0, 0, {}, h};
    if (auto s = preflight(c)) return *s;
    o = find(h);
    if (!o) return Status::fail(Status::Kind::Other, 6, QStringLiteral("invalid handle"));
    if (!(o->access & nt::kReadControl)) return Status::fail(Status::Kind::AccessDenied, 5, QStringLiteral("access denied"));
    *out = o->node->sd;
    return Status::success();
}

Status FakeNtSeams::setSecurity(Handle h, quint32 info, const QByteArray& sdBytes)
{
    Open* o = find(h);
    Call c{QStringLiteral("setSecurity"), o ? pathOf(o->node.get()) : QString(), {}, info, 0, 0, 0, sdBytes, h};
    if (auto s = preflight(c)) return *s;
    o = find(h);
    if (!o) return Status::fail(Status::Kind::Other, 6, QStringLiteral("invalid handle"));
    if (((info & nt::kDaclInfo) && !(o->access & nt::kWriteDac)) || ((info & nt::kOwnerInfo) && !(o->access & nt::kWriteOwner)))
        return Status::fail(Status::Kind::AccessDenied, 5, QStringLiteral("access denied"));
    std::optional<sd::Descriptor> cur = sd::parse(o->node->sd);
    const std::optional<sd::Descriptor> given = sd::parse(sdBytes);
    if (!cur || !given) return Status::fail(Status::Kind::Other, 1338, QStringLiteral("invalid security descriptor"));
    if (info & nt::kOwnerInfo) cur->owner = given->owner;
    if (info & nt::kDaclInfo) {
        cur->daclPresent = given->daclPresent;
        cur->daclNull = given->daclNull;
        cur->daclProtected = given->daclProtected;
        cur->daclAutoInherited = given->daclAutoInherited;
        cur->dacl = given->dacl;
    }
    o->node->sd = sd::build(cur->owner.value_or(QString()), cur->daclPresent ? std::optional<sd::Descriptor>(*cur) : std::nullopt);
    return Status::success();
}

Status FakeNtSeams::list(Handle dir, QStringList* names)
{
    Open* o = find(dir);
    Call c{QStringLiteral("list"), o ? pathOf(o->node.get()) : QString(), {}, 0, 0, 0, 0, {}, dir};
    if (auto s = preflight(c)) return *s;
    o = find(dir);
    if (!o || o->node->kind != Node::Kind::Dir) return Status::fail(Status::Kind::Other, 6, QStringLiteral("invalid handle"));
    *names = o->node->names.values();
    return Status::success();
}

Status FakeNtSeams::read(Handle h, qint64 offset, qint64 max, QByteArray* out)
{
    Open* o = find(h);
    Call c{QStringLiteral("read"), o ? pathOf(o->node.get()) : QString(), {}, 0, 0, 0, 0, {}, h};
    if (auto s = preflight(c)) return *s;
    o = find(h);
    if (!o) return Status::fail(Status::Kind::Other, 6, QStringLiteral("invalid handle"));
    if (!(o->access & (nt::kReadData | nt::kGenericRead))) return Status::fail(Status::Kind::AccessDenied, 5, QStringLiteral("access denied"));
    *out = o->node->data.mid(offset, max);
    return Status::success();
}

Status FakeNtSeams::write(Handle h, qint64 offset, const QByteArray& bytes)
{
    Open* o = find(h);
    Call c{QStringLiteral("write"), o ? pathOf(o->node.get()) : QString(), {}, 0, 0, 0, 0, bytes, h};
    if (auto s = preflight(c)) return *s;
    o = find(h);
    if (!o) return Status::fail(Status::Kind::Other, 6, QStringLiteral("invalid handle"));
    if (!(o->access & (nt::kWriteData | nt::kAppendData | nt::kGenericWrite)))
        return Status::fail(Status::Kind::AccessDenied, 5, QStringLiteral("access denied"));
    QByteArray& data = o->node->data;
    if (offset < 0) offset = data.size();
    if (data.size() < offset + bytes.size()) data.resize(offset + bytes.size());
    data.replace(offset, bytes.size(), bytes);
    return Status::success();
}

Status FakeNtSeams::setEndOfFile(Handle h, qint64 size)
{
    Open* o = find(h);
    Call c{QStringLiteral("setEndOfFile"), o ? pathOf(o->node.get()) : QString(), {}, 0, 0, 0, 0, {}, h};
    if (auto s = preflight(c)) return *s;
    o = find(h);
    if (!o) return Status::fail(Status::Kind::Other, 6, QStringLiteral("invalid handle"));
    o->node->data.resize(size);
    return Status::success();
}

Status FakeNtSeams::flush(Handle h)
{
    Open* o = find(h);
    Call c{QStringLiteral("flush"), o ? pathOf(o->node.get()) : QString(), {}, o ? o->access : 0, 0, 0, 0, {}, h};
    if (auto s = preflight(c)) return *s;
    o = find(h);
    if (!o) return Status::fail(Status::Kind::Other, 6, QStringLiteral("invalid handle"));
    // A directory flush needs FILE_ADD_FILE (as probed on windows-latest); a file flush needs write access.
    const quint32 need = o->node->kind == Node::Kind::Dir ? nt::kAddFile : (nt::kWriteData | nt::kAppendData | nt::kGenericWrite);
    if (!(o->access & need)) return Status::fail(Status::Kind::AccessDenied, 5, QStringLiteral("access denied"));
    return Status::success();
}

Status FakeNtSeams::rename(Handle file, Handle dir, const QString& name, bool replaceExisting)
{
    Open* f = find(file);
    Open* d = find(dir);
    Call c{QStringLiteral("rename"), f ? pathOf(f->node.get()) : QString(), name, 0, 0, replaceExisting ? 1u : 0u, 0, {}, file};
    if (auto s = preflight(c)) return *s;
    f = find(file);
    d = find(dir);
    if (!f || !d) return Status::fail(Status::Kind::Other, 6, QStringLiteral("invalid handle"));
    if (!(f->access & nt::kDelete)) return Status::fail(Status::Kind::AccessDenied, 5, QStringLiteral("access denied"));
    NodeP target = d->node;
    NodeP existing = target->children.value(name.toLower());
    if (existing == f->node) return Status::success();
    if (existing) {
        if (!replaceExisting) return Status::fail(Status::Kind::Exists, 0xC0000035u, QStringLiteral("name collision"));
        if (existing->kind == Node::Kind::Dir) return Status::fail(Status::Kind::IsDirectory, 0xC00000BAu, QStringLiteral("file is a directory"));
        for (const Open& o : m_handles)
            if (o.node == existing && !(o.share & nt::kShareDelete))
                return Status::fail(Status::Kind::SharingViolation, 0xC0000043u, QStringLiteral("sharing violation"));
        detach(existing.get());
    }
    NodeP moving = f->node;
    detach(moving.get());
    attach(target.get(), name, moving);
    return Status::success();
}

Status FakeNtSeams::dispose(Handle h)
{
    Open* o = find(h);
    Call c{QStringLiteral("dispose"), o ? pathOf(o->node.get()) : QString(), {}, 0, 0, 0, 0, {}, h};
    if (auto s = preflight(c)) return *s;
    o = find(h);
    if (!o) return Status::fail(Status::Kind::Other, 6, QStringLiteral("invalid handle"));
    if (!(o->access & nt::kDelete)) return Status::fail(Status::Kind::AccessDenied, 5, QStringLiteral("access denied"));
    detach(o->node.get());
    return Status::success();
}

Status FakeNtSeams::setDeleteOnClose(Handle h, bool del, bool posix)
{
    Open* o = find(h);
    Call c{QStringLiteral("setDeleteOnClose"), o ? pathOf(o->node.get()) : QString(),
           posix ? QStringLiteral("posix") : QStringLiteral("classic"), 0, 0, 0, del ? 1u : 0u, {}, h};
    if (auto s = preflight(c)) return *s;
    o = find(h);
    if (!o) return Status::fail(Status::Kind::Other, 6, QStringLiteral("invalid handle"));
    if (!(o->access & nt::kDelete)) return Status::fail(Status::Kind::AccessDenied, 5, QStringLiteral("access denied"));
    o->deleteOnClose = del;
    return Status::success();
}

Status FakeNtSeams::lockExclusive(Handle h)
{
    Open* o = find(h);
    Call c{QStringLiteral("lockExclusive"), o ? pathOf(o->node.get()) : QString(), {}, 0, 0, 0, 0, {}, h};
    if (auto s = preflight(c)) return *s;
    o = find(h);
    if (!o) return Status::fail(Status::Kind::Other, 6, QStringLiteral("invalid handle"));
    if (o->node->lockedBy && o->node->lockedBy != h)
        return Status::fail(Status::Kind::LockViolation, 33, QStringLiteral("lock violation"));
    o->node->lockedBy = h;
    return Status::success();
}

Status FakeNtSeams::volume(Handle h, win::Volume* v)
{
    Open* o = find(h);
    Call c{QStringLiteral("volume"), o ? pathOf(o->node.get()) : QString(), {}, 0, 0, 0, 0, {}, h};
    if (auto s = preflight(c)) return *s;
    *v = volumeInfo;
    return Status::success();
}

}  // namespace asb::test
