#pragma once

// An in-memory NTFS for the Windows adapters (WS-6): directories, files and reparse points with identities,
// self-relative security descriptors (inheritance computed like NTFS for the entries the adapters use),
// share-mode enforcement, LockFileEx, rename/dispose by handle with POSIX semantics, a call log and failure
// injection. Lets the adapter logic, and its mutants, run on every OS; the native rows run the same
// adapters on windows-latest (tests/win/integration).

#include "platform/win/NtSeams.h"

#include <QMap>

#include <functional>
#include <memory>
#include <optional>

namespace asb::test {

class FakeNtSeams final : public win::NtSeams {
public:
    struct Node {
        enum class Kind { Dir, File, Link } kind = Kind::File;
        FileId id;
        QByteArray sd;
        QByteArray data;
        QString linkTarget;                          // absolute path (links only)
        bool dirLink = false;                        // a junction or directory symlink
        QMap<QString, std::shared_ptr<Node>> children;   // key: lower-case name
        QMap<QString, QString> names;                // lower-case -> as created
        Node* parent = nullptr;
        bool deletePending = false;
        win::Handle lockedBy = 0;
    };
    using NodeP = std::shared_ptr<Node>;

    struct Call {
        QString op;          // createFile, openRelative, createDirectory, getSecurity, setSecurity, rename, dispose, ...
        QString path;        // createFile/createDirectory: the path; openRelative: "<dir path>\name"; else the node path
        QString name;
        quint32 access = 0, share = 0, disposition = 0, flags = 0;
        QByteArray sd;
        win::Handle handle = 0;
    };

    FakeNtSeams();

    // ---- configuration -------------------------------------------------------------------------
    QString userSid = QStringLiteral("S-1-5-21-1000-2000-3000-1001");
    QString defaultOwner;                            // the token's default owner; Administrators by default
    win::Volume volumeInfo{QStringLiteral("NTFS"), false};
    // Called before every operation; a returned Status is returned instead of performing it.
    std::function<std::optional<win::Status>(const Call&)> inject;
    // Called before every operation (after inject); may change the tree (identity swaps).
    std::function<void(const Call&)> before;
    QList<Call> calls;

    // ---- the tree ------------------------------------------------------------------------------
    NodeP node(const QString& path) const;           // without following a final link; nullptr if absent
    QString pathOf(const Node*) const;
    NodeP mkdir(const QString& path, const QByteArray& sd = {});          // parents must exist
    NodeP plantFile(const QString& path, const QByteArray& data, const QByteArray& sd = {});
    NodeP plantLink(const QString& path, const QString& target, bool directory);
    // Puts `replacement` at `path` (the old node is detached, still alive for open handles).
    void replace(const QString& path, const NodeP& replacement);
    NodeP newDir(const QByteArray& sd = {});         // detached
    void setSd(const QString& path, const QByteArray& sd);
    QStringList namesIn(const QString& path) const;
    int openHandles() const { return int(m_handles.size()); }
    int callCount(const QString& op) const;
    QList<Call> callsOf(const QString& op) const;

    // ---- NtSeams -------------------------------------------------------------------------------
    win::Status createFile(const QString& path, quint32 access, quint32 share, quint32 disposition, quint32 flags,
                           const QByteArray& sd, win::Handle* out) override;
    win::Status openRelative(win::Handle dir, const QString& name, quint32 access, quint32 share, quint32 disposition,
                             quint32 options, const QByteArray& sd, win::Handle* out) override;
    win::Status createDirectory(const QString& path, const QByteArray& sd) override;
    void close(win::Handle) override;
    win::Status facts(win::Handle, win::Facts*) override;
    win::Status getSecurity(win::Handle, quint32 info, QByteArray* sd) override;
    win::Status setSecurity(win::Handle, quint32 info, const QByteArray& sd) override;
    win::Status list(win::Handle dir, QStringList* names) override;
    win::Status read(win::Handle, qint64 offset, qint64 max, QByteArray* out) override;
    win::Status write(win::Handle, qint64 offset, const QByteArray&) override;
    win::Status setEndOfFile(win::Handle, qint64 size) override;
    win::Status flush(win::Handle) override;
    win::Status rename(win::Handle file, win::Handle dir, const QString& name, bool replace) override;
    win::Status dispose(win::Handle) override;
    win::Status setDeleteOnClose(win::Handle, bool del, bool posix) override;   // Call: name "posix"/"classic", flags = del
    win::Status lockExclusive(win::Handle) override;
    win::Status volume(win::Handle, win::Volume*) override;
    bool inheritable(win::Handle) override { return false; }
    QString currentUserSid() override { return userSid; }

private:
    struct Open {
        NodeP node;
        quint32 access = 0;
        quint32 share = 0;
        bool deleteOnClose = false;   // setDeleteOnClose: the node is detached when this handle closes
    };
    NodeP resolve(const QString& path, bool followFinal, NodeP* parent, QString* leaf) const;
    NodeP makeNode(Node::Kind, const QByteArray& givenSd, const Node* parent);
    win::Status shareCheck(const NodeP&, quint32 access, quint32 share) const;
    win::Status openNode(const NodeP&, quint32 access, quint32 share, win::Handle* out);
    std::optional<win::Status> preflight(const Call&);
    Open* find(win::Handle);
    void attach(Node* parent, const QString& name, const NodeP& child);
    void detach(Node* n);

    NodeP m_root;   // the machine: children are drives ("c:")
    QMap<win::Handle, Open> m_handles;
    win::Handle m_next = 0x100;
    quint64 m_nextId = 1;
};

}  // namespace asb::test
