#include "gui/ThemeNameReader.h"

#include <chrono>

#include <QDir>
#include <QUrl>

#include <sqlite3.h>

namespace asb::gui {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kBusyTimeoutMs = 150;
constexpr auto kStepBudget = std::chrono::milliseconds(500);
constexpr int kProgressOpcodes = 100;
// AetherSDR theme names are short display names; anything longer is not a name we can use.
constexpr qsizetype kMaxNameLength = 256;

int abortAfterDeadline(void* arg) {
    const auto* deadline = static_cast<const Clock::time_point*>(arg);
    return Clock::now() > *deadline ? 1 : 0;
}

struct Db {
    sqlite3* db = nullptr;
    ~Db() {
        if (db) {
            sqlite3_close_v2(db);
        }
    }
};

struct Stmt {
    sqlite3_stmt* stmt = nullptr;
    ~Stmt() {
        if (stmt) {
            sqlite3_finalize(stmt);
        }
    }
};

QString sqliteError(sqlite3* db, int rc) {
    const char* msg = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
    return QStringLiteral("SQLite %1: %2").arg(rc).arg(QString::fromUtf8(msg));
}

bool usableName(const QString& name) {
    if (name.isEmpty() || name.size() > kMaxNameLength) {
        return false;
    }
    for (const QChar c : name) {
        if (c.category() == QChar::Other_Control) {
            return false;
        }
    }
    return true;
}

}  // namespace

SqliteOpenFn defaultSqliteOpen() {
    return [](const char* uri, sqlite3** db, int flags) { return sqlite3_open_v2(uri, db, flags, nullptr); };
}

int themeDbOpenFlags() { return SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX; }

QString themeDbUriFromSlashPath(const QString& slashPath) {
    if (slashPath.startsWith(QLatin1String("//"))) {
        // A UNC path. QUrl::fromLocalFile would turn "//server/share/x" into the authority
        // "server" ("file://server/share/x"), and SQLite refuses any authority but an empty one
        // or "localhost" unless it is built with SQLITE_ALLOW_URI_AUTHORITY (it is not). With an
        // empty authority the whole path, "//server/share/x", is the file name: "file:" + "//"
        // + "//server/share/x". Percent-encoded like QUrl does a path (RFC 3986 pchar).
        static const QByteArray keep = QByteArrayLiteral("/!$&'()*+,;=:@");
        return QStringLiteral("file://") + QString::fromLatin1(QUrl::toPercentEncoding(slashPath, keep)) +
               QStringLiteral("?mode=ro");
    }
    return QUrl::fromLocalFile(slashPath).toString(QUrl::FullyEncoded) + QStringLiteral("?mode=ro");
}

QString themeDbUri(const QString& databasePath) {
    return themeDbUriFromSlashPath(QDir::fromNativeSeparators(databasePath));
}

ThemeNameRead readActiveThemeName(const QString& databasePath, const SqliteOpenFn& open) {
    ThemeNameRead r;
    sqlite3_hard_heap_limit64(kSqliteHeapLimit);

    const QByteArray uri = themeDbUri(databasePath).toUtf8();
    Db db;
    const int openRc = open(uri.constData(), &db.db, themeDbOpenFlags());
    if (openRc != SQLITE_OK) {
        r.error = QStringLiteral("cannot open AetherSDR's settings (%1)").arg(sqliteError(db.db, openRc));
        return r;
    }
    sqlite3_busy_timeout(db.db, kBusyTimeoutMs);

    Stmt stmt;
    const int prepRc = sqlite3_prepare_v2(db.db, "SELECT value FROM app_settings WHERE key='ActiveTheme'", -1,
                                          &stmt.stmt, nullptr);
    if (prepRc != SQLITE_OK) {
        r.error = QStringLiteral("cannot read AetherSDR's settings (%1)").arg(sqliteError(db.db, prepRc));
        return r;
    }

    // The budget covers the statement's execution. Filesystem stalls inside the VFS are not
    // bounded here (the handler only runs between VM instructions); the follower's UI deadline
    // bounds what the user sees, and the worker is detached.
    Clock::time_point deadline = Clock::now() + kStepBudget;
    sqlite3_progress_handler(db.db, kProgressOpcodes, &abortAfterDeadline, &deadline);
    const int stepRc = sqlite3_step(stmt.stmt);
    if (stepRc == SQLITE_DONE) {
        r.status = ThemeNameRead::Status::Missing;
        return r;
    }
    if (stepRc != SQLITE_ROW) {
        r.error = QStringLiteral("cannot read AetherSDR's settings (%1)").arg(sqliteError(db.db, stepRc));
        return r;
    }
    if (sqlite3_column_type(stmt.stmt, 0) != SQLITE_TEXT) {
        r.error = QStringLiteral("AetherSDR's ActiveTheme value is not text");
        return r;
    }
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 0));
    const QString name = QString::fromUtf8(text, sqlite3_column_bytes(stmt.stmt, 0));
    if (!usableName(name)) {
        r.error = QStringLiteral("AetherSDR's ActiveTheme value is not a usable theme name");
        return r;
    }
    r.status = ThemeNameRead::Status::Ok;
    r.name = name;
    return r;
}

}  // namespace asb::gui
