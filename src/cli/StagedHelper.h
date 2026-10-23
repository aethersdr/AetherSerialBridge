#pragma once

// The Linux window's two routes to a root helper (v1.1 D60, INV-122, INV-123, INV-124). Linux only. Pure: no process is
// created here (the window's PkexecClient is the process boundary, INV-112).
//
//   Installed  pkexec /usr/local/bin/aether-serial-bridge --helper           (v1.0, the bridge's own polkit action)
//   Staged     pkexec /bin/sh -c <packaging/linux/stage.sh> asb-stage <sha256> <program length> <request length>
//              (polkit's generic org.freedesktop.policykit.exec): the window writes its own program's bytes, then the
//              request, on stdin; the script copies both into a fresh root-only directory under
//              /usr/local/lib/aether-serial-bridge, checks the program's SHA-256 and runs the copy as --helper.
//
// Threat model. The window supplies both the bytes and the digest, so the check does not prove the bytes are authentic:
// the administrator's polkit approval of "run /bin/sh as root" for this window is the trust, as it is for
// `sudo ./….AppImage --install`. What the staged route adds: no root process opens a file another user (or anything
// after the check) could change; the bytes that run are the bytes that were checked; the digest pins them to the
// window's build, so a corrupted or swapped embedded program is refused. A process running as the same user can
// already drive the window and fake a password dialog; that is out of scope (R23).
//
// The script's own exits: 3 the digest does not match, 4 bad arguments / framing / an unsafe directory / staging I/O / a
// signal before the program started, 6 the verified copy could not be executed; each prints one "asb-stage: " line on
// stderr. The helper exits only 0, 1, 2 or 5 (INV-124), so 3, 4 and 6 always mean that nothing ran.
//
// Which route and op each window action takes (v1.1 WS-15 §3) is routeFor(): lifecycle actions (install, update,
// uninstall) are staged, the window's own build doing them (D62); day-to-day actions use the installed helper with
// v1.0 ops only, except Discard, which sends `discard-pending-if` only to an installed program that carries a build
// stamp (v1.1 or later).

#include "cli/HelperProtocol.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>

namespace asb::cli::staged {

inline constexpr char kShell[] = "/bin/sh";
inline constexpr char kScriptName[] = "asb-stage";   // the script's $0
inline constexpr char kStagingRoot[] = "/usr/local/lib/aether-serial-bridge";
inline constexpr char kLinePrefix[] = "asb-stage: ";

inline constexpr int kDigestMismatch = 3;
inline constexpr int kNothingStaged = 4;
inline constexpr int kNotExecutable = 6;

// The script, byte for byte as packaging/linux/stage.sh.
QByteArray script();

// The arguments pkexec gets after its own program name: /bin/sh -c <script> asb-stage <sha256> <plen> <rlen>.
QStringList pkexecArguments(const QByteArray& expectedSha256, qint64 programLength, qint64 requestLength);

// True only for exactly <kStagingRoot>/staging.<8 characters A-Za-z0-9>/aether-serial-bridge, the form `mktemp -d`
// makes (INV-123). `path` is canonical (/proc/self/exe's target): no "..", no symlink reaches the compare.
bool isStagedProgramPath(const QString& path);

// The script's own line on stderr without its prefix (empty when there is none): the last line that starts with
// "asb-stage: " (die prints it last, then exits). The staged helper's stderr never decides an outcome of the script's.
QString stageLine(const QByteArray& err);

// The directory an unsafe-ancestor line names ("<dir> is not a directory only root can write"), else empty.
QString unsafeDirectory(const QString& line);

// The terminal command that does what a staged action would have done, as the window shows it (INV-124: it runs as
// shown): sudo "<program>" --install | --uninstall [--remove-settings]. `program` is $APPIMAGE when the window runs
// from an AppImage (AppRun forwards both flags to the embedded program), else the headless program itself.
QString terminalCommand(const QString& program, helper::Op, bool removeSettings = false);

}  // namespace asb::cli::staged

namespace asb::cli {

enum class Route { Installed, Staged };

// The window's actions on Linux (WS-15 §3). Apply = Save & apply with the box ticked on a Current installation, or
// "No, keep it" on another build's; Install/Update = the staged install (with the editor's ports or without).
enum class WindowAction { Install, Update, Apply, Untick, Start, Stop, Repair, Discard, Uninstall };

// What routing needs to know about the installation: the installed program's build stamp (D61). nullopt: not read;
// an empty string: read, and it has none (every v1.0 program).
struct RouteFacts {
    std::optional<QString> installedVersion;
};

struct RoutePlan {
    helper::Op op = helper::Op::Inspect;
    Route route = Route::Installed;
};

RoutePlan routeFor(WindowAction, const RouteFacts&);

// True when the installed program carries a build stamp: every program from v1.1 on does (D61) and no v1.0 program
// does, so a stamped helper has the v1.1 ops (`discard-pending-if`). The stamp's version number is not compared: it
// is the CMake project version, which a development build of v1.1 may still carry as an earlier number.
bool helperHasV11Ops(const std::optional<QString>& installedVersion);

// The facts from the installed program's file (its stamp, read through its section table; nothing is executed).
RouteFacts readInstalledFacts(const QString& installedProgram);

}  // namespace asb::cli
