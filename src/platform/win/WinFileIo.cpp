#include "platform/win/WinFileIo.h"

#include "platform/win/SecurityDescriptor.h"

namespace asb::win {
namespace {

void setErr(QString* err, const QString& e)
{
    if (err) *err = e;
}

class WinTempFile final : public TempFile {
public:
    WinTempFile(NtSeams& s, WinFileIo& io, const DirHandle& dir, OwnedHandle h)
        : m_s(s), m_io(io), m_dir(dir), m_h(std::move(h))
    {
    }
    ~WinTempFile() override
    {
        if (!m_committed && m_h) m_s.dispose(m_h.get());   // delete by handle: never a name lookup
    }
    bool write(const QByteArray& bytes, QString* err) override
    {
        const Status st = m_s.write(m_h.get(), m_offset, bytes);
        if (!st.ok()) {
            setErr(err, st.describe());
            return false;
        }
        m_offset += bytes.size();
        return true;
    }
    bool flush(QString* err) override
    {
        const Status st = m_s.flush(m_h.get());
        if (!st.ok()) setErr(err, st.describe());
        return st.ok();
    }
    QByteArray readBack(qint64 cap, QString* err) override
    {
        QByteArray out;
        const Status st = m_s.read(m_h.get(), 0, cap + 1, &out);
        if (!st.ok()) {
            setErr(err, st.describe());
            return {};
        }
        if (out.size() > cap) {
            setErr(err, QStringLiteral("more than %1 bytes").arg(cap));
            return {};
        }
        return out;
    }
    FileId id() const override
    {
        Facts f;
        m_s.facts(m_h.get(), &f);
        return f.id;
    }
    Commit commitAs(const QString& name) override
    {
        Commit c;
        if (m_committed || !WinFileIo::isPlainName(name)) {
            c.error = QStringLiteral("cannot commit to %1").arg(name);
            return c;
        }
        const Status st = m_s.rename(m_h.get(), WinFileIo::nativeOf(m_dir), name, /*replace*/ true);
        if (!st.ok()) {
            c.error = QStringLiteral("could not replace %1: %2").arg(name, st.describe());
            return c;
        }
        m_committed = true;
        c.replaced = true;
        m_h.reset();
        QString err;
        c.durable = m_io.syncDir(m_dir, &err);
        if (!c.durable) c.error = err;
        return c;
    }

private:
    NtSeams& m_s;
    WinFileIo& m_io;
    const DirHandle& m_dir;
    OwnedHandle m_h;
    qint64 m_offset = 0;
    bool m_committed = false;
};

class WinExistingFile final : public ExistingFile {
public:
    WinExistingFile(NtSeams& s, OwnedHandle h) : m_s(s), m_h(std::move(h)) {}
    bool truncate(QString* err) override { return check(m_s.setEndOfFile(m_h.get(), 0), err); }
    bool writeAll(const QByteArray& bytes, QString* err) override { return check(m_s.write(m_h.get(), 0, bytes), err); }
    bool flush(QString* err) override { return check(m_s.flush(m_h.get()), err); }
    bool removeByHandle(QString* err) override { return check(m_s.dispose(m_h.get()), err); }

private:
    static bool check(const Status& st, QString* err)
    {
        if (!st.ok()) setErr(err, st.describe());
        return st.ok();
    }
    NtSeams& m_s;
    OwnedHandle m_h;
};

}  // namespace

CreateSecurity CreateSecurity::dataFolder()
{
    return {sd::ownerOnlyDescriptor(QString::fromLatin1(sd::kAdministrators)),
            sd::ownerOnlyDescriptor(QString::fromLatin1(sd::kSystem))};
}

CreateSecurity CreateSecurity::lockDirectory()
{
    return {sd::lockFileDescriptor(), sd::lockFileDescriptor()};
}

Handle WinFileIo::nativeOf(const DirHandle& d)
{
    const auto* w = dynamic_cast<const WinDirHandle*>(&d);
    return w ? w->native() : 0;
}

bool WinFileIo::isPlainName(const QString& n)
{
    if (n.isEmpty() || n == QLatin1String(".") || n == QLatin1String("..")) return false;
    for (const QChar c : n)
        if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char(':') || c.unicode() < 0x20) return false;
    return true;
}

std::unique_ptr<TempFile> WinFileIo::createTemp(const DirHandle& dir, OwnerPolicy policy, const NameGen& gen, QString* err)
{
    const Handle d = nativeOf(dir);
    if (!d) {
        setErr(err, QStringLiteral("not a directory handle of this adapter"));
        return nullptr;
    }
    const QString name = gen ? gen() : QString();
    if (!isPlainName(name)) {
        setErr(err, QStringLiteral("invalid temp name"));
        return nullptr;
    }
    const QByteArray& sec = policy == OwnerPolicy::Config ? m_sec.config : m_sec.runtime;
    Handle h = 0;
    // FILE_CREATE: an existing entry of any kind at this name is a failure, never opened (INV-15).
    const Status st = m_s.openRelative(d, name,
                                       nt::kGenericRead | nt::kGenericWrite | nt::kDelete | nt::kReadControl |
                                           nt::kReadAttributes,
                                       nt::kShareRead | nt::kShareDelete, nt::kFileCreate,
                                       nt::kNonDirectoryFile | nt::kOpenReparsePoint, sec, &h);
    if (!st.ok()) {
        setErr(err, QStringLiteral("could not create %1: %2").arg(name, st.describe()));
        return nullptr;
    }
    return std::make_unique<WinTempFile>(m_s, *this, dir, OwnedHandle(m_s, h));
}

bool WinFileIo::remove(const DirHandle& dir, const QString& name, QString* err)
{
    const Handle d = nativeOf(dir);
    if (!d || !isPlainName(name)) {
        setErr(err, QStringLiteral("invalid directory or name"));
        return false;
    }
    Handle h = 0;
    const Status st = m_s.openRelative(d, name, nt::kDelete | nt::kReadAttributes, nt::kShareAll, nt::kFileOpen,
                                       nt::kNonDirectoryFile | nt::kOpenReparsePoint, {}, &h);
    if (!st.ok()) {
        setErr(err, st.gone() ? fileNotFoundError() : st.describe());
        return false;
    }
    OwnedHandle owned(m_s, h);
    Facts f;
    if (const Status fs = m_s.facts(h, &f); !fs.ok()) {
        setErr(err, fs.describe());
        return false;
    }
    if (f.isReparse() || f.isDirectory()) {
        setErr(err, QStringLiteral("%1 is not a regular file").arg(name));
        return false;
    }
    const Status ds = m_s.dispose(h);
    if (!ds.ok()) setErr(err, ds.describe());
    return ds.ok();
}

bool WinFileIo::syncDir(const DirHandle& dir, QString* err)
{
    const Handle d = nativeOf(dir);
    if (!d) {
        setErr(err, QStringLiteral("not a directory handle of this adapter"));
        return false;
    }
    // FlushFileBuffers on a directory needs FILE_ADD_FILE (probed on windows-latest); reopen the same
    // directory object (empty name) rather than resolving its path again.
    Handle h = 0;
    Status st = m_s.openRelative(d, QString(), nt::kAddFile, nt::kShareAll, nt::kFileOpen, nt::kDirectoryFile, {}, &h);
    if (!st.ok()) {
        setErr(err, QStringLiteral("could not open the folder to flush it: %1").arg(st.describe()));
        return false;
    }
    OwnedHandle owned(m_s, h);
    st = m_s.flush(h);
    if (!st.ok()) setErr(err, QStringLiteral("could not flush the folder: %1").arg(st.describe()));
    return st.ok();
}

std::unique_ptr<ExistingFile> WinFileIo::openExisting(const DirHandle& dir, const QString& name, QString* err)
{
    const Handle d = nativeOf(dir);
    if (!d || !isPlainName(name)) {
        setErr(err, QStringLiteral("invalid directory or name"));
        return nullptr;
    }
    Handle h = 0;
    const Status st = m_s.openRelative(d, name, nt::kGenericWrite | nt::kDelete | nt::kReadAttributes, nt::kShareAll,
                                       nt::kFileOpen, nt::kNonDirectoryFile | nt::kOpenReparsePoint, {}, &h);
    if (!st.ok()) {
        setErr(err, st.kind == Status::Kind::NotFound ? fileNotFoundError() : st.describe());
        return nullptr;
    }
    OwnedHandle owned(m_s, h);
    Facts f;
    if (const Status fs = m_s.facts(h, &f); !fs.ok() || f.isReparse() || f.isDirectory()) {
        setErr(err, fs.ok() ? QStringLiteral("%1 is not a regular file").arg(name) : fs.describe());
        return nullptr;
    }
    return std::make_unique<WinExistingFile>(m_s, std::move(owned));
}

}  // namespace asb::win
