#include "gui/ThemeFollower.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QList>
#include <QLoggingCategory>
#include <QPointer>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "gui/ThemeDiscovery.h"
#include "gui/ThemeValidator.h"

#if defined(Q_OS_LINUX)
#  include <pthread.h>
#endif

Q_LOGGING_CATEGORY(lcTheme, "asb.gui.theme")

namespace asb::gui {

namespace {

using Clock = std::chrono::steady_clock;

const QString kDefaultDark = QStringLiteral("Default Dark");
const QString kDefaultLight = QStringLiteral("Default Light");
const QString kChoiceKey = QStringLiteral("ThemeChoice");

// A bundled theme: the compiled-in bytes, validated like any other theme (B5), once.
struct BuiltIn {
    QByteArray bytes;
    bool valid = false;
};

BuiltIn loadBuiltIn(const QString& resource) {
    BuiltIn b;
    QFile f(resource);
    if (f.open(QIODevice::ReadOnly)) {
        b.bytes = f.read(kMaxThemeBytes + 1);
    }
    const ThemeValidation v = validateTheme(b.bytes);
    b.valid = v.ok;
    if (!v.ok) {
        qCCritical(lcTheme) << "bundled theme" << resource << "failed validation:" << v.describe();
    }
    return b;
}

const BuiltIn& builtIn(const QString& name) {
    static const BuiltIn dark = loadBuiltIn(QStringLiteral(":/themes/default-dark.json"));
    static const BuiltIn light = loadBuiltIn(QStringLiteral(":/themes/default-light.json"));
    return name == kDefaultLight ? light : dark;
}

QString choiceName(ThemeFollower::Choice c) {
    switch (c) {
    case ThemeFollower::Choice::Dark:
        return QStringLiteral("Dark");
    case ThemeFollower::Choice::Light:
        return QStringLiteral("Light");
    case ThemeFollower::Choice::Follow:
        break;
    }
    return QStringLiteral("Follow");
}

}  // namespace

// What a read produced. Built on the worker; consumed on the UI thread.
struct ThemeReadResult {
    enum class Kind {
        Activate,     // `name` + `bytes` passed validation
        DefaultDark,  // nothing to follow; `reason` says why
    };
    Kind kind = Kind::DefaultDark;
    QString name;
    QByteArray bytes;
    QString path;  // the user theme file the bytes came from (empty for built-ins)
    QString reason;
};

namespace {

struct ReadParams {
    QString databasePath;
    QString themesDir;
    SqliteOpenFn open;
    std::function<void(const QString&)> afterOpen;
    std::function<bool(int)> failListing;
};

ThemeReadResult runRead(const ReadParams& p) {
    using Result = ThemeReadResult;
    Result r;
    const ThemeNameRead read = readActiveThemeName(p.databasePath, p.open);
    if (read.status == ThemeNameRead::Status::Missing) {
        r.reason = QStringLiteral("AetherSDR has no theme recorded; using Default Dark");
        return r;
    }
    if (read.status != ThemeNameRead::Status::Ok) {
        r.reason = QStringLiteral("Could not read AetherSDR's theme: %1; using Default Dark").arg(read.error);
        return r;
    }
    const QString name = read.name;
    const QString following = QStringLiteral("Following AetherSDR's theme");

    if (isBuiltInThemeName(name)) {
        const BuiltIn& b = builtIn(name);
        if (!b.valid) {
            r.reason = QStringLiteral("The bundled theme \"%1\" failed validation; using Default Dark").arg(name);
            return r;
        }
        r.kind = Result::Kind::Activate;
        r.name = name;
        r.bytes = b.bytes;
        r.reason = following;
        return r;
    }

    const ThemeDiscovery found = findUserTheme(p.themesDir, name, ThemeDiscoveryHooks{p.afterOpen, p.failListing});
    switch (found.status) {
    case ThemeDiscovery::Status::Found:
        break;
    case ThemeDiscovery::Status::TooManyEntries:
        r.reason = QStringLiteral("Not following \"%1\": too many entries in the theme folder to check safely; "
                                  "using Default Dark")
                       .arg(name);
        return r;
    case ThemeDiscovery::Status::TooManyJson:
        r.reason = QStringLiteral("Not following \"%1\": more than %2 theme files in the theme folder to check "
                                  "safely; using Default Dark")
                       .arg(name)
                       .arg(kMaxThemeJsonEntries);
        return r;
    case ThemeDiscovery::Status::Ambiguous:
        r.reason = QStringLiteral("Not following \"%1\": ambiguous: %2; using Default Dark").arg(name, found.detail);
        return r;
    case ThemeDiscovery::Status::NotFound:
    case ThemeDiscovery::Status::NoDirectory:
        r.reason = QStringLiteral("AetherSDR's theme \"%1\" was not found in its theme folder; using Default Dark")
                       .arg(name);
        if (!found.detail.isEmpty()) {
            qCInfo(lcTheme) << "theme files skipped:" << found.detail;
        }
        return r;
    case ThemeDiscovery::Status::Error:
        r.reason = QStringLiteral("Could not read AetherSDR's theme folder (%1); using Default Dark").arg(found.detail);
        return r;
    }

    const ThemeValidation v = validateTheme(found.bytes);
    if (!v.ok) {
        r.reason = QStringLiteral("AetherSDR's theme \"%1\" failed validation (%2 in %3); using Default Dark")
                       .arg(name, v.describe(), found.path);
        return r;
    }
    r.kind = Result::Kind::Activate;
    r.name = name;
    r.bytes = found.bytes;  // exactly the validated bytes; the path is not read again
    r.path = found.path;
    r.reason = following;
    return r;
}

}  // namespace

// Process-wide single-flight state (INV-95) and the route from the worker to the UI thread.
// Heap-allocated and never destroyed: a detached worker may still finish while the process exits.
struct ThemeFollowerRouter {
    std::mutex mutex;
    bool outstanding = false;  // guarded by mutex
    std::uint64_t currentJob = 0;  // guarded by mutex
    Clock::time_point startedAt;  // guarded by mutex
    std::atomic<int> started{0};
    std::atomic<int> running{0};
    std::atomic<std::uint64_t> delivered{0};
    QObject* receiver = nullptr;               // lives on the UI thread
    QList<QPointer<ThemeFollower>> followers;  // UI thread only

    static ThemeFollowerRouter& get() {
        static ThemeFollowerRouter* r = new ThemeFollowerRouter;
        return *r;
    }

    // UI thread.
    void deliver(std::uint64_t jobId, const ThemeReadResult& result) {
        const QList<QPointer<ThemeFollower>> targets = followers;
        for (const QPointer<ThemeFollower>& f : targets) {
            if (f) {
                f->onReadFinished(jobId, result);
            }
        }
        delivered.store(jobId);
    }

    void spawn(std::uint64_t jobId, ReadParams params) {
        started.fetch_add(1);
        running.fetch_add(1);
        QObject* to = receiver;
        std::thread([this, jobId, params = std::move(params), to]() {
#if defined(Q_OS_LINUX)
            pthread_setname_np(pthread_self(), "asb-theme-read");
#endif
            ThemeReadResult result = runRead(params);
            {
                std::lock_guard<std::mutex> lock(mutex);
                outstanding = false;
            }
            QMetaObject::invokeMethod(
                to, [this, jobId, result = std::move(result)]() { deliver(jobId, result); }, Qt::QueuedConnection);
            running.fetch_sub(1);
        }).detach();
    }
};

ThemeFollower::Options ThemeFollower::defaultOptions() {
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/AetherSDR");
    Options o;
    o.databasePath = base + QStringLiteral("/AetherSDR.db");
    o.themesDir = base + QStringLiteral("/themes");
    o.openDatabase = defaultSqliteOpen();
    return o;
}

ThemeFollower::ThemeFollower(QObject* parent) : ThemeFollower(defaultOptions(), parent) {}

ThemeFollower::ThemeFollower(Options options, QObject* parent) : QObject(parent), m_options(std::move(options)) {
    if (!m_options.openDatabase) {
        m_options.openDatabase = defaultSqliteOpen();
    }
    auto& router = ThemeFollowerRouter::get();
    if (!router.receiver) {
        router.receiver = new QObject;  // on this (the UI) thread; never deleted
    }
    router.followers.append(this);
}

ThemeFollower::~ThemeFollower() {
    auto& router = ThemeFollowerRouter::get();
    router.followers.removeAll(QPointer<ThemeFollower>(this));
    router.followers.removeAll(QPointer<ThemeFollower>());
}

ThemeFollower::Choice ThemeFollower::savedChoice() {
    const QString v = AetherSDR::AppSettings::instance().value(kChoiceKey).toString();
    if (v == QLatin1String("Dark")) {
        return Choice::Dark;
    }
    if (v == QLatin1String("Light")) {
        return Choice::Light;
    }
    return Choice::Follow;
}

void ThemeFollower::start(Choice choice) {
    m_choice = choice;
    m_pending.reset();  // a read still outstanding for an earlier request is discarded
    auto& settings = AetherSDR::AppSettings::instance();
    settings.setValue(kChoiceKey, choiceName(choice));
    settings.save();

    if (choice == Choice::Follow) {
        refresh();
        return;
    }
    const QString name = choice == Choice::Light ? kDefaultLight : kDefaultDark;
    const BuiltIn& b = builtIn(name);
    if (b.valid) {
        activate(Validated{name, b.bytes}, QStringLiteral("Chosen in the bridge"));
    }
}

void ThemeFollower::refresh() {
    if (m_choice != Choice::Follow) {
        return;
    }
    auto& router = ThemeFollowerRouter::get();
    std::uint64_t jobId = 0;
    bool stalled = false;
    {
        std::lock_guard<std::mutex> lock(router.mutex);
        if (router.outstanding) {
            if (Clock::now() - router.startedAt > std::chrono::milliseconds(kStalledAfterMs)) {
                stalled = true;
            } else {
                jobId = router.currentJob;  // join it: no second read
            }
        } else {
            router.outstanding = true;
            router.startedAt = Clock::now();
            jobId = ++router.currentJob;
            router.spawn(jobId, ReadParams{m_options.databasePath, m_options.themesDir, m_options.openDatabase,
                                           m_options.afterThemeFileOpened, m_options.failThemeListing});
        }
    }
    if (stalled) {
        m_pending.reset();
        applyFallback(QStringLiteral("AetherSDR's settings have not answered for more than %1 s")
                          .arg(kStalledAfterMs / 1000));
        return;
    }

    const std::uint64_t requestId = ++m_nextRequestId;
    m_pending = Pending{requestId, jobId, Clock::now() + std::chrono::milliseconds(uiDeadlineMs())};
    // The timer is how an unanswered request gets its fallback; it is not what decides whether a
    // result is late. Its event can be delivered after the result's (a busy UI thread), so
    // onReadFinished checks the request's own deadline too.
    QTimer::singleShot(uiDeadlineMs(), Qt::PreciseTimer, this, [this, requestId]() {
        if (m_pending && m_pending->requestId == requestId) {
            expirePending();
        }
    });
}

void ThemeFollower::expirePending() {
    m_pending.reset();  // its result, when it comes, is discarded
    applyFallback(QStringLiteral("AetherSDR's settings did not answer within %1 ms").arg(uiDeadlineMs()));
}

void ThemeFollower::onReadFinished(std::uint64_t jobId, const ThemeReadResult& result) {
    // Accepted only for the request now waiting, on this read, before that request's deadline;
    // whichever of this and the deadline timer runs first, a late result is discarded.
    if (!m_pending || m_pending->jobId != jobId) {
        ++m_discardedLate;
        qCInfo(lcTheme) << "discarded a late theme read result:" << result.name << result.reason;
        return;
    }
    if (Clock::now() >= m_pending->deadline) {
        // The deadline passed before this result got to the UI thread, and the timer has not
        // applied the fallback yet: discard the result and apply it now (the timer then finds
        // nothing pending).
        ++m_discardedLate;
        qCInfo(lcTheme) << "discarded a theme read result that arrived after its deadline:" << result.name
                        << result.reason;
        expirePending();
        return;
    }
    m_pending.reset();
    applyResult(result);
}

void ThemeFollower::applyResult(const ThemeReadResult& result) {
    if (result.kind != ThemeReadResult::Kind::Activate) {
        applyDefaultDark(result.reason);
        if (const BuiltIn& b = builtIn(kDefaultDark); b.valid) {
            m_lastGood = Validated{kDefaultDark, b.bytes};  // a definitive answer: nothing to follow
        }
        return;
    }
    if (m_options.beforeActivate && !result.path.isEmpty()) {
        m_options.beforeActivate(result.path);
    }
    const Validated theme{result.name, result.bytes};
    if (activate(theme, result.reason)) {
        m_lastGood = theme;
        return;
    }
    applyDefaultDark(QStringLiteral("AetherSDR's theme \"%1\" could not be activated; using Default Dark")
                         .arg(result.name));
}

void ThemeFollower::applyFallback(const QString& why) {
    if (m_lastGood) {
        const Validated last = *m_lastGood;
        activate(last, why + QStringLiteral("; keeping \"%1\"").arg(last.name));
        return;
    }
    applyDefaultDark(why + QStringLiteral("; using Default Dark"));
}

void ThemeFollower::applyDefaultDark(const QString& reason) {
    const BuiltIn& b = builtIn(kDefaultDark);
    if (b.valid) {
        activate(Validated{kDefaultDark, b.bytes}, reason);
    }
}

bool ThemeFollower::activate(const Validated& theme, const QString& reason) {
    // The one activation route (B5, INV-86): these bytes, exactly as validated.
    if (!AetherSDR::ThemeManager::instance().activateValidatedTheme(theme.name, theme.bytes)) {
        qCWarning(lcTheme) << "activation of" << theme.name << "was refused; the previous theme stays";
        return false;
    }
    qCInfo(lcTheme) << "theme" << theme.name << "applied:" << reason;
    emit applied(theme.name, reason);
    return true;
}

int ThemeFollower::workersStarted() { return ThemeFollowerRouter::get().started.load(); }

int ThemeFollower::workersRunning() { return ThemeFollowerRouter::get().running.load(); }

bool ThemeFollower::waitUntilIdle(int timeoutMs) {
    auto& router = ThemeFollowerRouter::get();
    QDeadlineTimer deadline(timeoutMs);
    for (;;) {
        bool outstanding = false;
        std::uint64_t job = 0;
        {
            std::lock_guard<std::mutex> lock(router.mutex);
            outstanding = router.outstanding;
            job = router.currentJob;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (!outstanding && router.running.load() == 0 && router.delivered.load() >= job) {
            return true;
        }
        if (deadline.hasExpired()) {
            return false;
        }
        QThread::msleep(5);
    }
}

}  // namespace asb::gui
