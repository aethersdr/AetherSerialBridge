#include "cli/ScratchRun.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace asb::cli::ScratchRun {
namespace {

#ifdef _WIN32
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

QString absoluteClean(const QString& p)
{
    return QDir::cleanPath(QFileInfo(p).absoluteFilePath());
}

// `path` is `folder` or lies inside it, lexically.
bool lexicallyWithin(const QString& path, const QString& folder)
{
    if (path.compare(folder, kPathCase) == 0) return true;
    const QString prefix = folder.endsWith(QLatin1Char('/')) ? folder : folder + QLatin1Char('/');
    return path.startsWith(prefix, kPathCase);
}

// Some directory from `dir` up to the root has identity `key`.
bool ancestorIs(QString dir, const FileKey& key, const IdentityOf& identity)
{
    for (int guard = 0; guard < 4096 && !dir.isEmpty(); ++guard) {
        if (const std::optional<FileKey> k = identity(dir); k && *k == key) return true;
        const QString parent = QFileInfo(dir).absolutePath();
        if (parent == dir || QDir(dir).isRoot()) break;
        dir = parent;
    }
    return false;
}

}  // namespace

std::optional<FileKey> fileKey(const QString& path)
{
#ifdef _WIN32
    const std::wstring w = QDir::toNativeSeparators(path).toStdWString();
    // FILE_READ_ATTRIBUTES only, every share mode: never blocks another user of the file. BACKUP_SEMANTICS opens
    // a directory too. Links are followed (no FILE_FLAG_OPEN_REPARSE_POINT): the identity is the target's.
    HANDLE h = CreateFileW(w.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    if (!ok) return std::nullopt;
    FileKey k;
    k.device = static_cast<quint64>(info.dwVolumeSerialNumber);
    k.index = (static_cast<quint64>(info.nFileIndexHigh) << 32) | static_cast<quint64>(info.nFileIndexLow);
    return k;
#else
    struct stat st {};
    const QByteArray p = QFile::encodeName(path);
    if (::stat(p.constData(), &st) != 0) return std::nullopt;
    return FileKey{static_cast<quint64>(st.st_dev), static_cast<quint64>(st.st_ino)};
#endif
}

QString refuseManagedPath(const QString& configFile, const QString& managedConfigFile, const QString& dataFolder,
                          const IdentityOf& identity)
{
    const QString config = absoluteClean(configFile);
    const QString data = absoluteClean(dataFolder);
    const QString dir = QFileInfo(config).absolutePath();
    const QString inside = QStringLiteral("--config %1 is inside the data folder %2; a scratch config must be a copy in a "
                                          "folder of its own outside it")
                               .arg(QDir::toNativeSeparators(configFile), QDir::toNativeSeparators(dataFolder));
    if (lexicallyWithin(dir, data)) return inside;
    const std::optional<FileKey> mine = identity(config);
    const std::optional<FileKey> managed = identity(managedConfigFile);
    if (mine && managed && *mine == *managed)
        return QStringLiteral("--config %1 is the installed config.json (%2); a scratch config must be a copy in a folder "
                              "of its own outside %3")
            .arg(QDir::toNativeSeparators(configFile), QDir::toNativeSeparators(managedConfigFile),
                 QDir::toNativeSeparators(dataFolder));
    if (const std::optional<FileKey> folder = identity(data)) {
        if (ancestorIs(dir, *folder, identity)) return inside;
        const QString resolved = QFileInfo(dir).canonicalFilePath();
        if (!resolved.isEmpty() && ancestorIs(resolved, *folder, identity)) return inside;
    }
    return {};
}

QString refuseStatusFileName(const QString& configFile, bool windows, const IdentityOf& identity)
{
    const QFileInfo info(configFile);
    const QString status = QStringLiteral("status.json");
    const QString beside = QDir(info.absolutePath()).filePath(status);
    bool same = info.fileName().compare(status, windows ? Qt::CaseInsensitive : Qt::CaseSensitive) == 0;
    if (!same) {
        const std::optional<FileKey> mine = identity(info.absoluteFilePath());
        const std::optional<FileKey> theirs = identity(beside);
        same = mine && theirs && *mine == *theirs;
    }
    if (!same) return {};
    return QStringLiteral("--config %1 is where the diagnostic run writes its status (%2); give the scratch config "
                          "another name")
        .arg(QDir::toNativeSeparators(configFile), QDir::toNativeSeparators(beside));
}

QString refuseInstalledImage(const QString& ownExecutable, const QString& installedExecutable, const IdentityOf& identity)
{
    const std::optional<FileKey> installed = identity(installedExecutable);
    if (!installed) return {};   // nothing installed there
    const std::optional<FileKey> mine = identity(ownExecutable);
    if (!mine)
        return QStringLiteral("cannot tell whether this program is the installed service program %1, so --run refuses")
            .arg(QDir::toNativeSeparators(installedExecutable));
    if (*mine == *installed)
        return QStringLiteral("this is the installed service program (%1); --run never runs from it. Run a copy of the "
                              "program from somewhere else")
            .arg(QDir::toNativeSeparators(installedExecutable));
    return {};
}

QString loopbackFor(const QString& bindAddress)
{
    static const QString v4 = QStringLiteral("127.0.0.1");
    static const QString v6 = QStringLiteral("::1");
    const std::optional<IpAddress> ip = parseIpAddress(bindAddress);
    if (!ip || ip->family == IpAddress::Family::V4) return v4;
    // ::ffff:a.b.c.d is an IPv4 address on a dual-stack socket.
    bool mapped = ip->bytes[10] == 0xff && ip->bytes[11] == 0xff;
    for (int i = 0; i < 10; ++i)
        if (ip->bytes[size_t(i)] != 0) mapped = false;
    return mapped ? v4 : v6;
}

BridgeConfig forceLoopback(const BridgeConfig& config, QStringList* warnings)
{
    BridgeConfig out = config;
    for (PortMapping& p : out.ports) {
        const QString loop = loopbackFor(p.bindAddress);
        if (p.bindAddress == loop) continue;
        if (warnings && p.enabled)
            *warnings << QStringLiteral("Warning: %1 listens on %2 only, not on %3 as the config says: a diagnostic run is "
                                        "reachable from this machine only.")
                             .arg(p.displayName(), loop, p.bindAddress.isEmpty() ? QStringLiteral("(no address)") : p.bindAddress);
        p.bindAddress = loop;
    }
    return out;
}

}  // namespace asb::cli::ScratchRun
