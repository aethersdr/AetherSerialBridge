#pragma once

// Finds the user theme file whose "name" is AetherSDR's active theme (master §5.6, WS-11,
// INV-85, INV-95). Runs on the follower's worker thread.
//
// Directory: <GenericConfigLocation>/AetherSDR/themes/. Bounded at every step:
//  - names only are enumerated, with a budget of 256 directory entries of any kind ("." and ".."
//    are not entries); reaching the 257th stops enumeration and nothing is followed;
//  - among them, names ending ".json" (case-insensitively, like AetherSDR's own "*.json"
//    filter), hidden ones included; more than 64 such names and nothing is followed;
//  - the listing must reach a clean end (POSIX readdir NULL with errno 0; Windows FindNextFileW
//    failing with ERROR_NO_MORE_FILES). A listing that stops with an error (EINTR included) is
//    incomplete: neither uniqueness nor the budgets were established, so nothing is followed
//    (Status::Error), even if the part that was listed holds exactly one match;
//  - each of those (all of them, so a duplicate is always seen) is opened no-follow and
//    nonblocking (POSIX O_NOFOLLOW|O_NONBLOCK relative to the directory handle; Windows
//    FILE_FLAG_OPEN_REPARSE_POINT), must be a regular file on the handle (POSIX fstat S_ISREG;
//    Windows GetFileType == FILE_TYPE_DISK and neither directory nor reparse point), and is read
//    from that handle up to 256 KiB + 1 bytes: more than 256 KiB rejects it (the cap is on the
//    read itself, not a size pre-check); a candidate whose open fails is classified by a no-follow
//    stat of its name instead (POSIX fstatat AT_SYMLINK_NOFOLLOW; Windows GetFileAttributesW, which
//    does not follow a final reparse point), never by the listing: a socket fails the open (ENXIO)
//    and is still "not a regular file";
//  - its "name" is parsed; built-in names are skipped. 0 matches: not found; more than 1:
//    ambiguous. Exactly 1 is also ambiguous when another candidate could not be examined but may
//    carry the same name: a link, a file over the cap, or one that could not be opened or read
//    (AetherSDR's own QDir::Files listing follows links and has no size cap, so it may load that
//    one). Candidates that are not regular files (on the handle, or by that stat when the open
//    fails: directories, FIFOs, sockets, devices) do not count: AetherSDR never loads those as
//    themes.
// The matching file's bytes are returned: they are what gets validated and activated, and the
// path is never read again.
//
// Which candidates count mirrors AetherSDR's listing, QDir::entryList({"*.json"}, QDir::Files)
// (ThemeManager::scanAvailableThemes at the pinned AetherSDR, Qt 6.12). Without QDir::Hidden it
// drops hidden entries; without QDir::System it drops entries that are not a file, a folder or a
// link, and links whose target does not resolve; without QDir::Dirs it drops folders and links to
// folders; without QDir::NoSymLinks it resolves links and lists the rest. "Hidden" is Qt's: on
// Unix a name starting with "." (also UF_HIDDEN on BSD and macOS, which the bridge does not build
// for), on Windows only FILE_ATTRIBUTE_HIDDEN from the listing. A hidden entry is dropped before
// anything else (a hidden link included), but still counts against both budgets.
//
//   entry named *.json                        | AetherSDR lists it? | bridge counts it?
//   ------------------------------------------+---------------------+-------------------------------
//   regular file                              | yes                 | yes, examined
//   ".x.json", POSIX                          | no (hidden)         | no ("hidden", never opened)
//   ".x.json", Windows                        | yes (not hidden)    | yes, examined
//   FILE_ATTRIBUTE_HIDDEN, Windows            | no (hidden)         | no ("hidden", never opened)
//   FILE_ATTRIBUTE_SYSTEM, Windows            | yes (Qt ignores it) | yes, examined
//   link to a regular file, POSIX             | yes                 | yes, never followed: a single
//                                             |                     | match next to it is ambiguous
//   link to a FIFO/socket/device, POSIX       | yes (a link passes  | yes, as above
//                                             | the System filter)  |
//   link to a folder, POSIX                   | no (resolves to dir)| no ("a link to a folder")
//   broken link or link loop, POSIX           | no (no System)      | no ("a broken link")
//   folder, junction or folder link, Windows  | no (a folder to Qt) | no ("not a regular file")
//   file symlink or other reparse point,      | yes, broken ones    | yes, as a link (never
//   Windows (AF_UNIX socket, WOF, cloud)      | too (Qt reads the   | followed): conservative
//                                             | listing's data)     |
//   folder, FIFO, socket, device              | no (not a file)     | no ("not a regular file")
//
// Links: the bridge never opens through a link, so it cannot read the name of the file a link
// points at, and a single match next to a link that AetherSDR would list stays ambiguous. To
// decide whether AetherSDR would list it, a following stat (POSIX fstatat without
// AT_SYMLINK_NOFOLLOW) reads the target's type only: it opens and reads nothing.
//
// Windows: an AF_UNIX socket is a reparse point, so it is treated like a link (it counts, and a
// single match next to one is ambiguous): the conservative side, and an unlikely setup.
//
// Windows: each file is opened by its full path, and FILE_FLAG_OPEN_REPARSE_POINT guards only the
// last path component, so a junction swapped in above it (themesDir or a folder above it, between
// the listing and the open) is followed. The open is still read-only and the file's "name" must
// still match.

#include <functional>

#include <QByteArray>
#include <QString>

namespace asb::gui {

inline constexpr int kMaxThemeDirEntries = 256;
inline constexpr int kMaxThemeJsonEntries = 64;

struct ThemeDiscoveryHooks {
    // Test seam: called on the worker after a candidate is opened and found to be a regular
    // file, before it is read (tst_ThemeFollower::growingFileCapped grows the file here).
    std::function<void(const QString& path)> afterOpen;
    // Test seam: called before each fetch of the next directory entry with the number of ".json"
    // names listed so far; returning true makes that fetch fail the way the platform reports a
    // failed directory read (POSIX readdir NULL with errno EIO; Windows FindNextFileW FALSE with
    // ERROR_IO_DEVICE), and the production check decides what that means.
    std::function<bool(int jsonListed)> failListing;
};

struct ThemeDiscovery {
    enum class Status { Found, NotFound, NoDirectory, TooManyEntries, TooManyJson, Ambiguous, Error };
    Status status = Status::Error;
    QString path;      // Found: the file the bytes came from (for messages only)
    QByteArray bytes;  // Found: exactly the bytes read from that file's handle
    QString detail;    // NotFound/Found: what was skipped and why; Ambiguous: why (for the reason text)
};

ThemeDiscovery findUserTheme(const QString& themesDir, const QString& name, const ThemeDiscoveryHooks& hooks = {});

// The names AetherSDR reserves for its bundled themes.
bool isBuiltInThemeName(const QString& name);

}  // namespace asb::gui
