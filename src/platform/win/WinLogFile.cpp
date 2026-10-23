#include "platform/win/WinLogFile.h"

namespace asb::win {
namespace {
void setErr(QString* err, const QString& e)
{
    if (err) *err = e;
}
}  // namespace

WinLogFile::WinLogFile(NtSeams& seams, const DirHandle& dir, QString name, QByteArray createSd)
    : m_s(seams), m_dir(dir), m_name(std::move(name)), m_sd(std::move(createSd))
{
}

qint64 WinLogFile::size()
{
    Handle h = 0;
    const Status st = m_s.openRelative(WinFileIo::nativeOf(m_dir), m_name, nt::kReadAttributes, nt::kShareAll, nt::kFileOpen,
                                       nt::kNonDirectoryFile | nt::kOpenReparsePoint, {}, &h);
    if (!st.ok()) return -1;
    OwnedHandle owned(m_s, h);
    Facts f;
    if (!m_s.facts(h, &f).ok() || f.isReparse()) return -1;
    return f.size;
}

bool WinLogFile::rollover(QString* err)
{
    Handle h = 0;
    Status st = m_s.openRelative(WinFileIo::nativeOf(m_dir), m_name, nt::kDelete | nt::kReadAttributes, nt::kShareAll,
                                 nt::kFileOpen, nt::kNonDirectoryFile | nt::kOpenReparsePoint, {}, &h);
    if (!st.ok()) {
        setErr(err, st.describe());
        return false;
    }
    OwnedHandle owned(m_s, h);
    Facts f;
    if (!m_s.facts(h, &f).ok() || f.isReparse()) {
        setErr(err, QStringLiteral("%1 is not a regular file").arg(m_name));
        return false;
    }
    st = m_s.rename(h, WinFileIo::nativeOf(m_dir), m_name + QStringLiteral(".1"), /*replace*/ true);
    if (!st.ok()) setErr(err, st.describe());
    return st.ok();
}

bool WinLogFile::append(const QByteArray& bytes, QString* err)
{
    const Handle d = WinFileIo::nativeOf(m_dir);
    if (!d) {
        setErr(err, QStringLiteral("not a directory handle of this adapter"));
        return false;
    }
    Handle h = 0;
    Status st = m_s.openRelative(d, m_name, nt::kAppendData | nt::kReadAttributes, nt::kShareRead | nt::kShareDelete,
                                 nt::kFileOpenIf, nt::kNonDirectoryFile | nt::kOpenReparsePoint, m_sd, &h);
    if (!st.ok()) {
        setErr(err, st.describe());
        return false;
    }
    OwnedHandle owned(m_s, h);
    Facts f;
    if (!m_s.facts(h, &f).ok() || f.isReparse()) {
        setErr(err, QStringLiteral("%1 is not a regular file").arg(m_name));
        return false;
    }
    st = m_s.write(h, -1, bytes);
    if (!st.ok()) setErr(err, st.describe());
    return st.ok();
}

}  // namespace asb::win
