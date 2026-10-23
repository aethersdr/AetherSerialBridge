#include "core/AppLocations.h"

#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <knownfolders.h>
#  include <shlobj.h>
#endif

namespace asb {

QString AppLocations::dataDirectory() const
{
    return QFileInfo(configFile).path();
}

QString AppLocations::windowsProgramData()
{
#ifdef Q_OS_WIN
    PWSTR path = nullptr;
    QString dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &path)) && path)
        dir = QDir::fromNativeSeparators(QString::fromWCharArray(path));
    CoTaskMemFree(path);
    // There is no safe fallback: an empty result is returned as such and forOs() refuses it (a bare
    // prepend would give "/AetherSerialBridge", rooted on the current drive).
    return dir;
#else
    return QStringLiteral("C:/ProgramData");
#endif
}

namespace {

// "X:/..." or "//server/share...": a path that names its own drive or server, not one rooted on the
// current drive ("/x") or relative to the current directory ("x", "C:x").
bool isDriveOrUncPath(const QString& p)
{
    if (p.size() >= 3 && p[0].isLetter() && p[0].unicode() < 128 && p[1] == QLatin1Char(':') && p[2] == QLatin1Char('/'))
        return true;
    return p.size() >= 3 && p.startsWith(QLatin1String("//")) && p[2] != QLatin1Char('/');
}

}  // namespace

std::optional<AppLocations> AppLocations::forOs(bool windows, const std::function<QString()>& programData)
{
    if (windows) {
        const QString base = programData ? programData() : QString();
        if (!isDriveOrUncPath(base)) return std::nullopt;   // fail closed: see the header, FAILURE
        const QString dir = base + QStringLiteral("/AetherSerialBridge");
        return AppLocations{dir + QStringLiteral("/config.json"), dir + QStringLiteral("/status.json"),
                            dir + QStringLiteral("/bridge.log")};
    }
    const QString run = QFileInfo(QString::fromLatin1(kLinuxStatusFile)).path();
    return AppLocations{QString::fromLatin1(kLinuxConfigFile), QString::fromLatin1(kLinuxStatusFile),
                        run + QStringLiteral("/bridge.log")};
}

std::optional<AppLocations> AppLocations::forOs(bool windows)
{
    return forOs(windows, [] { return windowsProgramData(); });
}

std::optional<AppLocations> AppLocations::forThisOs()
{
#ifdef Q_OS_WIN
    return forOs(true);
#else
    return forOs(false);
#endif
}

AppLocations AppLocations::beside(const QString& configFile)
{
    const QString full = QDir::cleanPath(QDir::current().absoluteFilePath(configFile));
    const QString dir = QFileInfo(full).path();
    return {full, dir + QStringLiteral("/status.json"), dir + QStringLiteral("/bridge.log")};
}

namespace CommandLine {

ConfigOption takeConfigOption(const QStringList& args)
{
    ConfigOption r;
    for (qsizetype i = 0; i < args.size(); ++i) {
        if (args[i].compare(QLatin1String("--config"), Qt::CaseInsensitive) == 0) {
            if (i + 1 >= args.size() || args[i + 1].startsWith(QLatin1String("--"))) {
                r.error = QStringLiteral("--config needs a file path.");
                return r;
            }
            r.configFile = args[++i];
        } else {
            r.rest.append(args[i]);
        }
    }
    return r;
}

}  // namespace CommandLine
}  // namespace asb
