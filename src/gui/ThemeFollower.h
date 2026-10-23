#pragma once

// Theme following (master §5.6, D4, D17, D30; WS-11; INV-31, 36, 53, 85, 86, 95).
//
// The bridge's window wears AetherSDR's active theme, read safely:
//  - Follow: a worker thread reads AetherSDR's ActiveTheme (ThemeNameReader), finds the theme's
//    bytes (built-in: the compiled-in :/themes/default-*.json; user theme: ThemeDiscovery's
//    bounded read) and validates them (ThemeValidator). The UI thread activates the result only
//    if it reaches the UI thread within 750 ms of the request (checked when the result is
//    accepted, not only by the deadline timer); otherwise it applies the last good result (or
//    Default Dark), and the late result is discarded when it comes.
//  - Dark / Light: the bundled theme, chosen in the bridge.
//  - Every activation, built-in or user, is ThemeManager::activateValidatedTheme(name, bytes)
//    (P2) with the exact bytes that were validated: the theme file is never read again after
//    validation (INV-52), nothing calls setActiveTheme, and no theme state is persisted through
//    AppSettings (INV-86). The choice itself is the bridge's own gui.ini key "ThemeChoice".
//  - Single flight (INV-95): at most one read is outstanding per process. A refresh while one
//    is outstanding starts no other; it waits for that one's result under its own 750 ms
//    deadline. Once the outstanding read has been stalled for more than 5 s, refreshes stop
//    waiting and apply the last good result (or Default Dark) until it returns.
//  - Nothing here writes to AetherSDR's folder (INV-31). What makes that so is the open flags:
//    the database is opened SQLITE_OPEN_READONLY with ?mode=ro (tst_ThemeFollower::
//    openFlagsReadOnly), the theme files with O_RDONLY / GENERIC_READ only
//    (::themeFileOpenedReadOnly). The writer-symbol link test (cmake/ThemeWriterCheck.cmake) is a
//    tripwire on top: the object files reference none of a list of known writer APIs (ThemeManager's
//    mutators among them). It cannot catch a write through a call this code needs anyway, such as
//    an open given write flags.

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

#include <QByteArray>
#include <QObject>
#include <QString>

#include "gui/ThemeNameReader.h"

namespace asb::gui {

struct ThemeReadResult;
struct ThemeFollowerRouter;

class ThemeFollower : public QObject {
    Q_OBJECT

public:
    enum class Choice { Follow, Dark, Light };

    struct Options {
        // AetherSDR's settings database and user theme folder.
        QString databasePath;
        QString themesDir;
        // The SQLite open call (test seam; default sqlite3_open_v2 with the default VFS).
        SqliteOpenFn openDatabase;
        // Test seam, worker thread: a candidate theme file was opened and is a regular file.
        std::function<void(const QString& path)> afterThemeFileOpened;
        // Test seam, worker thread: make the theme-folder listing fail midway (ThemeDiscoveryHooks).
        std::function<bool(int jsonListed)> failThemeListing;
        // Test seam, UI thread: a user theme validated from `path` is about to be activated.
        std::function<void(const QString& path)> beforeActivate;
        // Test seam: the UI-thread deadline in milliseconds. 0 (the default, and what the product
        // always uses) means kUiDeadlineMs. A test that is not about the deadline sets it high so a
        // loaded host cannot turn "the worker answered in time" into a race.
        int uiDeadlineMs = 0;
    };

    // <GenericConfigLocation>/AetherSDR/AetherSDR.db and <GenericConfigLocation>/AetherSDR/themes
    // (AetherSDR's SettingsPaths and ThemeManager locations), the real sqlite3_open_v2.
    static Options defaultOptions();

    static constexpr int kUiDeadlineMs = 750;
    static constexpr int kStalledAfterMs = 5000;

    explicit ThemeFollower(QObject* parent = nullptr);
    explicit ThemeFollower(Options options, QObject* parent = nullptr);
    ~ThemeFollower() override;

    // Records the choice in gui.ini and applies it (Follow: starts a read).
    void start(Choice choice);
    // Follow only: read AetherSDR's theme again (live switching, D17).
    void refresh();

    Choice choice() const { return m_choice; }

    // Test seam: changes Options::uiDeadlineMs for the requests made from now on.
    void setUiDeadlineMs(int ms) { m_options.uiDeadlineMs = ms; }

    // The choice stored in gui.ini ("ThemeChoice"); Follow when absent (D4).
    static Choice savedChoice();

    // Diagnostics (tests): results that arrived after their request's deadline (or for no
    // request) and were dropped.
    int discardedLateResults() const { return m_discardedLate; }
    // Worker threads ever started / running now, process-wide.
    static int workersStarted();
    static int workersRunning();
    // Processes events until no read is outstanding and its result has been delivered.
    static bool waitUntilIdle(int timeoutMs);

signals:
    // A theme was activated: `name` is now ThemeManager's active theme; `reason` says why
    // (following AetherSDR, chosen in the bridge, or the fallback and what caused it).
    void applied(const QString& name, const QString& reason);

private:
    struct Validated {
        QString name;
        QByteArray bytes;
    };
    friend struct ThemeFollowerRouter;

    int uiDeadlineMs() const { return m_options.uiDeadlineMs > 0 ? m_options.uiDeadlineMs : kUiDeadlineMs; }
    void onReadFinished(std::uint64_t jobId, const ThemeReadResult& result);
    void applyResult(const ThemeReadResult& result);
    void expirePending();
    void applyFallback(const QString& why);
    void applyDefaultDark(const QString& reason);
    bool activate(const Validated& theme, const QString& reason);

    Options m_options;
    Choice m_choice = Choice::Follow;
    struct Pending {
        std::uint64_t requestId;
        std::uint64_t jobId;
        std::chrono::steady_clock::time_point deadline;  // request time + kUiDeadlineMs
    };
    std::optional<Pending> m_pending;
    std::uint64_t m_nextRequestId = 0;
    std::optional<Validated> m_lastGood;
    int m_discardedLate = 0;
};

}  // namespace asb::gui
