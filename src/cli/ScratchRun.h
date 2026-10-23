#pragma once

// The rules of a scratch config (--config <file>) and of the diagnostic `--run --config <file>` (master §4.10, D42;
// INV-26, INV-90, INV-101). Pure decisions over an injectable file-identity seam; the run itself (the engine) is
// the headless executable's (app/ScratchHost), because the GUI links this library and never the engine (INV-1).
//
//  - A scratch config is never the managed one: refused when the file IS the managed config.json (same file
//    identity, so a hard link or a symbolic link to it is refused too), and when its directory is the data folder
//    or lies inside it, judged by identity along the lexical AND the resolved ancestors, and by the lexical path
//    as well (so a data folder that does not exist yet cannot be named either). Never by string comparison alone.
//  - `--run` never runs from the installed service program: refused when this executable and the installed one
//    are the same file (identity: st_dev/st_ino, or the volume serial number and file index). A hard link to the
//    installed program is refused; a copy is a different file and runs (INV-101).
//  - Every scratch listener binds loopback only (v6/N2): any IPv4 address (0.0.0.0 included) -> 127.0.0.1; any
//    IPv6 address (:: and global ones included) -> ::1; an IPv4-mapped IPv6 address -> 127.0.0.1. Never
//    QHostAddress::Any / AnyIPv6. A changed address is said in a warning.

#include "core/BridgeConfig.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace asb::cli::ScratchRun {

// The identity of a file system object: the device and inode (POSIX) or the volume serial number and the file
// index (Windows). Links are followed: the identity is that of the object a path names.
struct FileKey {
    quint64 device = 0;
    quint64 index = 0;
    bool operator==(const FileKey&) const = default;
};
using IdentityOf = std::function<std::optional<FileKey>(const QString& path)>;
// The real lookup (stat / CreateFileW + GetFileInformationByHandle). nullopt when the path names nothing.
std::optional<FileKey> fileKey(const QString& path);

// Empty when `configFile` may be used as a scratch config; otherwise why not (one sentence, no full stop).
QString refuseManagedPath(const QString& configFile, const QString& managedConfigFile, const QString& dataFolder,
                          const IdentityOf& identity = &fileKey);

// Empty when `configFile` may be a scratch config next to the diagnostic run's status: refused when it is named
// status.json (ASCII case-insensitively on Windows, where the file system folds case) or is the same file as the
// status.json beside it (identity), because the run's first status publication would replace it.
QString refuseStatusFileName(const QString& configFile, bool windows, const IdentityOf& identity = &fileKey);

// Empty when this executable may run --run; otherwise why not.
QString refuseInstalledImage(const QString& ownExecutable, const QString& installedExecutable,
                             const IdentityOf& identity = &fileKey);

// The loopback address a scratch listener binds instead of `bindAddress` ("127.0.0.1" or "::1").
QString loopbackFor(const QString& bindAddress);
// `config` with every port's bind address replaced by its loopback; one warning per port whose address changed.
BridgeConfig forceLoopback(const BridgeConfig& config, QStringList* warnings);

}  // namespace asb::cli::ScratchRun
