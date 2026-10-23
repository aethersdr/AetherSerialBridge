#pragma once

// Reads AetherSDR's active theme name from its settings database (master §5.6, WS-11, S7, D30,
// INV-31, INV-53). Runs on the follower's worker thread; never on the UI thread.
//
//  - sqlite3_hard_heap_limit64(8 MiB): the GUI uses SQLite for nothing else.
//  - URI = QUrl::fromLocalFile(path).toString(FullyEncoded) + "?mode=ro" for a drive or POSIX
//    path. For a UNC path ("//server/share/…", or the backslash form \\server\share\…): an
//    empty authority and the percent-encoded path, "file:////server/share/…?mode=ro" (SQLite is
//    built without SQLITE_ALLOW_URI_AUTHORITY, so "file://server/…" is refused). Opened with
//    SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX; busy_timeout(150).
//  - One statement, SELECT value FROM app_settings WHERE key='ActiveTheme' (a WITHOUT ROWID
//    primary-key lookup), under a progress handler that aborts it 500 ms after it starts
//    stepping; then close.
//  - A read transaction on a WAL database is a consistent snapshot that includes committed WAL
//    frames, so a value that exists only in the WAL is seen. No immutable=1 (it would ignore the
//    WAL), no copy, no checkpoint (the handle is read-only).
//
// Permitted side effects, stated (R7): SQLite may open (or, if the directory allows, create)
// AetherSDR.db-shm and update its reader slots, as a second AetherSDR process would. The main
// database file and the -wal file are never written.

#include <functional>

#include <QString>

struct sqlite3;

namespace asb::gui {

// The open call, as a seam: tests record the flags and URI (INV-31, INV-53) or route the open
// through a wrapper VFS. Same contract as sqlite3_open_v2(uri, db, flags, nullptr).
using SqliteOpenFn = std::function<int(const char* uri, sqlite3** db, int flags)>;

// sqlite3_open_v2 with the default VFS.
SqliteOpenFn defaultSqliteOpen();

// The flags every open uses.
int themeDbOpenFlags();

// "file:///…/AetherSDR.db?mode=ro" — the path percent-encoded, never concatenated. A UNC path
// gets an empty authority: "file:////server/share/…/AetherSDR.db?mode=ro".
QString themeDbUri(const QString& databasePath);

// The same for a path already written with forward slashes (what QDir::fromNativeSeparators
// returns). themeDbUri() is this plus the platform's separator conversion; tests call it directly
// so the backslash-free half of the contract is checked on every platform.
QString themeDbUriFromSlashPath(const QString& slashPath);

inline constexpr long long kSqliteHeapLimit = 8LL * 1024 * 1024;

struct ThemeNameRead {
    enum class Status {
        Ok,       // `name` holds the value
        Missing,  // no ActiveTheme row: AetherSDR uses Default Dark
        Error,    // could not open or read; `error` says why
    };
    Status status = Status::Error;
    QString name;
    QString error;
};

ThemeNameRead readActiveThemeName(const QString& databasePath, const SqliteOpenFn& open);

}  // namespace asb::gui
