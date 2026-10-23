#pragma once

// An in-memory, POSIX-like model of the FileIo seam for tests (WS-3; reused by WS-5's Applier tests).
//
//  - One directory. Entries are files, symlinks or directories; files point at inodes, so a renamed or
//    removed file keeps its identity and data.
//  - Exclusive create: createTemp fails on ANY existing entry at the generated name (the O_EXCL /
//    CREATE_NEW contract) and never touches it.
//  - Ownership is modelled on POSIX: a file is created by the calling process ("process"); the Config
//    policy then logs `chown <name>` (fchown(0,0)) on the private fd, which makes the owner "root";
//    the Runtime policy never chowns. Both log `chmod <name> 0644`.
//  - Durability: a file's data is durable after flush; directory changes (create, rename, remove) are
//    durable after syncDir. crash() keeps only what was durable, so unflushed data disappears and
//    removed-but-unsynced entries come back (resurrection).
//  - Every operation is appended to `log` ("createTemp a Config", "chown a", "chmod a 0644",
//    "write a 12", "flush a", "readBack a", "rename a -> b", "syncDir", "removeTemp a", "remove a",
//    "openExisting a", "truncate a", "writeAll a 3", "flushExisting a", "removeByHandle a");
//    failures are logged with a "FAILED " prefix.
//  - Any operation can be made to fail once (failNext) or every time (failAlways).

#include "core/FileIo.h"

#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

namespace asb::test {

class FakeDir : public DirHandle {
public:
    explicit FakeDir(quint64 ino = 1) : m_id(FileId::fromPosix(42, ino)) {}
    FileId id() const override { return m_id; }

private:
    FileId m_id;
};

class FakeFileIo : public FileIo {
public:
    enum class Op {
        CreateTemp, Write, Flush, ReadBack, Rename, SyncDir, Remove,
        OpenExisting, Truncate, WriteAll, FlushExisting, RemoveByHandle,
    };
    enum class Kind { File, Symlink, Directory };

    QStringList log;

    void failNext(Op op, const QString& error = QStringLiteral("injected failure")) { m_failOnce[op] = error; }
    void failAlways(Op op, const QString& error = QStringLiteral("injected failure")) { m_failAlways[op] = error; }
    void clearFailures() { m_failOnce.clear(); m_failAlways.clear(); }
    // Called as every operation begins (e.g. to make it return after a deadline: advance a clock).
    std::function<void(Op)> onOp;

    // Planting (durable at once, owned by "attacker" unless said otherwise).
    void plantFile(const QString& name, const QByteArray& bytes, const QString& owner = QStringLiteral("attacker"));
    void plantSymlink(const QString& name, const QString& target);
    void plantDirectory(const QString& name);

    bool exists(const QString& name) const { return m_entries.contains(name); }
    std::optional<Kind> kind(const QString& name) const;
    QByteArray contents(const QString& name) const;   // a file's visible data (empty if not a file)
    QString owner(const QString& name) const;          // "root", "process", "attacker"; empty if not a file
    int mode(const QString& name) const;
    QStringList names() const;                          // sorted
    QStringList tempNames() const;                      // names matching isTempName

    // Power loss: only durable state survives.
    void crash();

    // FileIo
    std::unique_ptr<TempFile> createTemp(const DirHandle&, OwnerPolicy, const NameGen&, QString* err) override;
    bool remove(const DirHandle&, const QString& name, QString* err) override;
    bool syncDir(const DirHandle&, QString* err) override;
    std::unique_ptr<ExistingFile> openExisting(const DirHandle&, const QString& name, QString* err) override;

    // For the TempFile / ExistingFile implementations.
    struct Inode {
        QByteArray data, flushed;
        QString owner;
        int mode = 0600;
        bool flushedOnce = false;
    };
    struct Entry {
        Kind kind = Kind::File;
        quint64 inode = 0;     // files
        QString target;        // symlinks
    };
    bool injected(Op op, const QString& what, QString* err);
    Inode& inode(quint64 ino) { return m_inodes[ino]; }
    std::optional<Entry> entry(const QString& name) const;
    void setEntry(const QString& name, const Entry& e) { m_entries.insert(name, e); }
    void eraseEntry(const QString& name) { m_entries.remove(name); }

private:
    quint64 newInode(const QString& owner);

    QMap<QString, Entry> m_entries, m_durableEntries;
    QHash<quint64, Inode> m_inodes;
    quint64 m_nextInode = 100;
    QMap<Op, QString> m_failOnce, m_failAlways;
};

}  // namespace asb::test
