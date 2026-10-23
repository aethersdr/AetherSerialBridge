#pragma once

// Where config, status and log live, and the --config option. Paths use '/' separators (Qt style) on every OS.
//
//  Windows: everything in %ProgramData%\AetherSerialBridge, where the service and the GUI meet
//           (the folder from SHGetKnownFolderPath(FOLDERID_ProgramData), not the environment).
//  Linux:   config in /etc/aether-serial-bridge, status (and the log file name) in
//           /run/aether-serial-bridge, the systemd RuntimeDirectory; the service logs to the journal.
//  --config <path>: everything moves next to that file, so a diagnostic run never touches the
//           system locations.
//
// FAILURE (Windows only). If the ProgramData folder cannot be determined (SHGetKnownFolderPath fails,
// or returns something that is not a drive-letter or UNC path), forOs(true) / forThisOs() return
// std::nullopt. There is deliberately no fallback: prepending an empty folder would give
// "/AetherSerialBridge", which Windows roots on the CURRENT drive, i.e. a data directory the process
// did not choose. WS-5 (service/CLI) and WS-6 (Windows adapters) MUST treat nullopt as fatal: refuse to
// start, with a clear error (kWindowsDataFolderError, then exit non-zero), and must not substitute a
// guessed folder, the environment's %ProgramData%, or the current directory. The Linux locations and
// --config paths cannot fail.

#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace asb {

struct AppLocations {
    QString configFile, statusFile, logFile;

    QString dataDirectory() const;   // the directory of configFile

    // nullopt only for `windows` when the ProgramData folder is unavailable (see FAILURE above).
    static std::optional<AppLocations> forOs(bool windows);
    // The same, with the Windows ProgramData lookup injected (empty string = lookup failed).
    static std::optional<AppLocations> forOs(bool windows, const std::function<QString()>& programData);
    static std::optional<AppLocations> forThisOs();
    // `configFile` made absolute (lexically, like Path.GetFullPath: no link resolution).
    static AppLocations beside(const QString& configFile);

    // The Windows %ProgramData% folder, '/' separators; EMPTY when it cannot be determined (on other OSes
    // a fixed "C:/ProgramData", only so forOs(true) can be tested anywhere).
    static QString windowsProgramData();

    bool operator==(const AppLocations&) const = default;
};

// The message for the fatal case above.
inline constexpr char kWindowsDataFolderError[] =
    "Cannot determine the Windows ProgramData folder (SHGetKnownFolderPath failed); refusing to start.";

inline constexpr char kLinuxConfigFile[] = "/etc/aether-serial-bridge/config.json";
inline constexpr char kLinuxStatusFile[] = "/run/aether-serial-bridge/status.json";

namespace CommandLine {

struct ConfigOption {
    QStringList rest;                    // the arguments without "--config <path>"
    std::optional<QString> configFile;   // the last --config value, if any
    QString error;                       // non-empty: "--config needs a file path."
};

// Pulls "--config <path>" out of the arguments wherever it appears (the option name is matched
// case-insensitively). A missing value, or one that starts with "--", is an error.
ConfigOption takeConfigOption(const QStringList& args);

}  // namespace CommandLine
}  // namespace asb
