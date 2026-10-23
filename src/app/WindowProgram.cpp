#include "app/WindowProgram.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef _WIN32
#include <windows.h>
#endif

namespace asb::app {
namespace {

QString ownExecutablePath()
{
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    return QDir::fromNativeSeparators(QString::fromWCharArray(buf.data(), int(n)));
#else
    return QFileInfo(QStringLiteral("/proc/self/exe")).symLinkTarget();
#endif
}

}  // namespace

QString windowProgramPath(const QString& ownExecutable, const QString& appDir)
{
    const QString ownDir = QFileInfo(ownExecutable).absolutePath();
#ifdef _WIN32
    (void)appDir;
    return ownDir + QStringLiteral("/aether-serial-bridge.exe");
#else
    if (!appDir.isEmpty()) {
        const QString imageRoot = QDir::cleanPath(ownDir + QStringLiteral("/../.."));
        if (QDir::cleanPath(appDir) == imageRoot) return imageRoot + QStringLiteral("/usr/libexec/aether-serial-bridge");
    }
    return ownDir + QStringLiteral("/aether-serial-bridge");
#endif
}

WindowProgram WindowProgramProbe::check(const QByteArray& expectedSha256)
{
    std::lock_guard<std::mutex> g(m_mutex);
    if (!m_read) {
        m_read = true;
        QFile f(m_path);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!f.open(QIODevice::ReadOnly) || !hash.addData(&f))
            m_readError = f.errorString();
        else
            m_sha256 = QString::fromLatin1(hash.result().toHex());
    }
    WindowProgram w;
    w.path = m_path;
    w.sha256 = m_sha256;
    if (m_sha256.isEmpty()) {
        w.problem = QStringLiteral("This window's program is not the one it was built with (%1 cannot be read: %2).")
                        .arg(QDir::toNativeSeparators(m_path), m_readError);
        return w;
    }
    w.digestOk = !expectedSha256.isEmpty() && m_sha256.toLatin1() == expectedSha256.toLower();
    if (!w.digestOk)
        w.problem = QStringLiteral("This window's program is not the one it was built with (%1 is program sha256 %2, "
                                   "this window expects %3).")
                        .arg(QDir::toNativeSeparators(m_path), m_sha256.left(12),
                             QString::fromLatin1(expectedSha256.left(12)));
    return w;
}

WindowProgram windowProgram(const QByteArray& expectedSha256)
{
    static WindowProgramProbe probe(windowProgramPath(ownExecutablePath(), qEnvironmentVariable("APPDIR")));
    return probe.check(expectedSha256);
}

}  // namespace asb::app
