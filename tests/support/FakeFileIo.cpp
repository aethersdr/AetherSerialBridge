#include "support/FakeFileIo.h"

#include "core/AtomicJson.h"

namespace asb::test {
namespace {

class FakeTempFile : public TempFile {
public:
    FakeTempFile(FakeFileIo& io, QString name, quint64 ino) : m_io(io), m_name(std::move(name)), m_ino(ino) {}
    ~FakeTempFile() override
    {
        if (m_committed) return;
        // Removes the temp by handle: only if the name still refers to this file.
        const auto e = m_io.entry(m_name);
        if (e && e->kind == FakeFileIo::Kind::File && e->inode == m_ino) {
            m_io.eraseEntry(m_name);
            m_io.log << QStringLiteral("removeTemp %1").arg(m_name);
        }
    }

    bool write(const QByteArray& bytes, QString* err) override
    {
        if (m_io.injected(FakeFileIo::Op::Write, QStringLiteral("write %1").arg(m_name), err)) return false;
        m_io.inode(m_ino).data += bytes;
        m_io.log << QStringLiteral("write %1 %2").arg(m_name).arg(bytes.size());
        return true;
    }
    bool flush(QString* err) override
    {
        if (m_io.injected(FakeFileIo::Op::Flush, QStringLiteral("flush %1").arg(m_name), err)) return false;
        auto& n = m_io.inode(m_ino);
        n.flushed = n.data;
        n.flushedOnce = true;
        m_io.log << QStringLiteral("flush %1").arg(m_name);
        return true;
    }
    QByteArray readBack(qint64 cap, QString* err) override
    {
        if (m_io.injected(FakeFileIo::Op::ReadBack, QStringLiteral("readBack %1").arg(m_name), err)) return {};
        m_io.log << QStringLiteral("readBack %1").arg(m_name);
        const QByteArray& d = m_io.inode(m_ino).data;
        if (d.size() > cap) {
            if (err) *err = QStringLiteral("larger than %1 bytes").arg(cap);
            return {};
        }
        return d;
    }
    FileId id() const override { return FileId::fromPosix(42, m_ino); }

    Commit commitAs(const QString& target) override
    {
        Commit c;
        if (m_committed) {
            c.error = QStringLiteral("already committed");
            return c;
        }
        const QString what = QStringLiteral("rename %1 -> %2").arg(m_name, target);
        if (m_io.injected(FakeFileIo::Op::Rename, what, &c.error)) return c;
        const auto existing = m_io.entry(target);
        if (existing && existing->kind == FakeFileIo::Kind::Directory) {
            c.error = QStringLiteral("EISDIR");
            m_io.log << QStringLiteral("FAILED ") + what;
            return c;
        }
        // rename(2) replaces a symlink entry itself; it never writes through it.
        m_io.eraseEntry(m_name);
        m_io.setEntry(target, FakeFileIo::Entry{FakeFileIo::Kind::File, m_ino, {}});
        m_io.log << what;
        m_committed = true;
        m_name = target;
        c.replaced = true;
        QString err;
        c.durable = m_io.syncDir(FakeDir(), &err);
        if (!c.durable) c.error = err;
        return c;
    }

private:
    FakeFileIo& m_io;
    QString m_name;
    quint64 m_ino;
    bool m_committed = false;
};

class FakeExistingFile : public ExistingFile {
public:
    FakeExistingFile(FakeFileIo& io, QString name, quint64 ino) : m_io(io), m_name(std::move(name)), m_ino(ino) {}
    bool truncate(QString* err) override
    {
        if (m_io.injected(FakeFileIo::Op::Truncate, QStringLiteral("truncate %1").arg(m_name), err)) return false;
        m_io.inode(m_ino).data.clear();
        m_io.log << QStringLiteral("truncate %1").arg(m_name);
        return true;
    }
    bool writeAll(const QByteArray& bytes, QString* err) override
    {
        if (m_io.injected(FakeFileIo::Op::WriteAll, QStringLiteral("writeAll %1").arg(m_name), err)) return false;
        m_io.inode(m_ino).data += bytes;
        m_io.log << QStringLiteral("writeAll %1 %2").arg(m_name).arg(bytes.size());
        return true;
    }
    bool flush(QString* err) override
    {
        if (m_io.injected(FakeFileIo::Op::FlushExisting, QStringLiteral("flushExisting %1").arg(m_name), err))
            return false;
        auto& n = m_io.inode(m_ino);
        n.flushed = n.data;
        m_io.log << QStringLiteral("flushExisting %1").arg(m_name);
        return true;
    }
    bool removeByHandle(QString* err) override
    {
        if (m_io.injected(FakeFileIo::Op::RemoveByHandle, QStringLiteral("removeByHandle %1").arg(m_name), err))
            return false;
        const auto e = m_io.entry(m_name);
        if (!e || e->kind != FakeFileIo::Kind::File || e->inode != m_ino) {
            if (err) *err = QStringLiteral("identity changed");
            m_io.log << QStringLiteral("FAILED removeByHandle %1").arg(m_name);
            return false;
        }
        m_io.eraseEntry(m_name);
        m_io.log << QStringLiteral("removeByHandle %1").arg(m_name);
        return true;
    }

private:
    FakeFileIo& m_io;
    QString m_name;
    quint64 m_ino;
};

}  // namespace

bool FakeFileIo::injected(Op op, const QString& what, QString* err)
{
    if (onOp) onOp(op);
    QString e;
    if (auto it = m_failOnce.find(op); it != m_failOnce.end()) {
        e = it.value();
        m_failOnce.erase(it);
    } else if (auto a = m_failAlways.find(op); a != m_failAlways.end()) {
        e = a.value();
    } else {
        return false;
    }
    if (err) *err = e;
    log << QStringLiteral("FAILED ") + what;
    return true;
}

quint64 FakeFileIo::newInode(const QString& owner)
{
    const quint64 ino = m_nextInode++;
    m_inodes[ino].owner = owner;
    return ino;
}

std::optional<FakeFileIo::Entry> FakeFileIo::entry(const QString& name) const
{
    const auto it = m_entries.find(name);
    if (it == m_entries.end()) return std::nullopt;
    return it.value();
}

void FakeFileIo::plantFile(const QString& name, const QByteArray& bytes, const QString& owner)
{
    const quint64 ino = newInode(owner);
    m_inodes[ino].data = m_inodes[ino].flushed = bytes;
    m_inodes[ino].mode = 0644;
    m_entries.insert(name, Entry{Kind::File, ino, {}});
    m_durableEntries.insert(name, Entry{Kind::File, ino, {}});
}

void FakeFileIo::plantSymlink(const QString& name, const QString& target)
{
    m_entries.insert(name, Entry{Kind::Symlink, 0, target});
    m_durableEntries.insert(name, Entry{Kind::Symlink, 0, target});
}

void FakeFileIo::plantDirectory(const QString& name)
{
    m_entries.insert(name, Entry{Kind::Directory, 0, {}});
    m_durableEntries.insert(name, Entry{Kind::Directory, 0, {}});
}

std::optional<FakeFileIo::Kind> FakeFileIo::kind(const QString& name) const
{
    const auto e = entry(name);
    return e ? std::optional<Kind>(e->kind) : std::nullopt;
}

QByteArray FakeFileIo::contents(const QString& name) const
{
    const auto e = entry(name);
    return (e && e->kind == Kind::File) ? m_inodes.value(e->inode).data : QByteArray();
}

QString FakeFileIo::owner(const QString& name) const
{
    const auto e = entry(name);
    return (e && e->kind == Kind::File) ? m_inodes.value(e->inode).owner : QString();
}

int FakeFileIo::mode(const QString& name) const
{
    const auto e = entry(name);
    return (e && e->kind == Kind::File) ? m_inodes.value(e->inode).mode : 0;
}

QStringList FakeFileIo::names() const
{
    return m_entries.keys();
}

QStringList FakeFileIo::tempNames() const
{
    QStringList out;
    for (const QString& n : m_entries.keys())
        if (isTempName(n)) out << n;
    return out;
}

void FakeFileIo::crash()
{
    m_entries = m_durableEntries;
    for (auto& n : m_inodes) n.data = n.flushed;
    log << QStringLiteral("CRASH");
}

std::unique_ptr<TempFile> FakeFileIo::createTemp(const DirHandle&, OwnerPolicy policy, const NameGen& gen, QString* err)
{
    const QString name = gen();
    const QString pol = policy == OwnerPolicy::Config ? QStringLiteral("Config") : QStringLiteral("Runtime");
    const QString what = QStringLiteral("createTemp %1 %2").arg(name, pol);
    if (injected(Op::CreateTemp, what, err)) return nullptr;
    if (m_entries.contains(name)) {   // O_CREAT|O_EXCL|O_NOFOLLOW: any existing entry, links included
        if (err) *err = QStringLiteral("EEXIST");
        log << QStringLiteral("FAILED ") + what;
        return nullptr;
    }
    const quint64 ino = newInode(QStringLiteral("process"));
    m_entries.insert(name, Entry{Kind::File, ino, {}});
    log << what;
    if (policy == OwnerPolicy::Config) {
        m_inodes[ino].owner = QStringLiteral("root");
        log << QStringLiteral("chown %1").arg(name);
    }
    m_inodes[ino].mode = 0644;
    log << QStringLiteral("chmod %1 0644").arg(name);
    return std::make_unique<FakeTempFile>(*this, name, ino);
}

bool FakeFileIo::remove(const DirHandle&, const QString& name, QString* err)
{
    if (injected(Op::Remove, QStringLiteral("remove %1").arg(name), err)) return false;
    const auto e = entry(name);
    if (!e) {
        if (err) *err = fileNotFoundError();
        log << QStringLiteral("FAILED remove %1").arg(name);
        return false;
    }
    if (e->kind != Kind::File) {
        if (err) *err = QStringLiteral("not a regular file");
        log << QStringLiteral("FAILED remove %1").arg(name);
        return false;
    }
    m_entries.remove(name);
    log << QStringLiteral("remove %1").arg(name);
    return true;
}

bool FakeFileIo::syncDir(const DirHandle&, QString* err)
{
    if (injected(Op::SyncDir, QStringLiteral("syncDir"), err)) return false;
    m_durableEntries = m_entries;
    log << QStringLiteral("syncDir");
    return true;
}

std::unique_ptr<ExistingFile> FakeFileIo::openExisting(const DirHandle&, const QString& name, QString* err)
{
    if (injected(Op::OpenExisting, QStringLiteral("openExisting %1").arg(name), err)) return nullptr;
    const auto e = entry(name);
    if (!e) {
        if (err) *err = fileNotFoundError();
        log << QStringLiteral("FAILED openExisting %1").arg(name);
        return nullptr;
    }
    if (e->kind != Kind::File) {
        if (err) *err = e->kind == Kind::Symlink ? QStringLiteral("ELOOP") : QStringLiteral("EISDIR");
        log << QStringLiteral("FAILED openExisting %1").arg(name);
        return nullptr;
    }
    log << QStringLiteral("openExisting %1").arg(name);
    return std::make_unique<FakeExistingFile>(*this, name, e->inode);
}

}  // namespace asb::test
