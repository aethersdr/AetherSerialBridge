#include "core/BridgeRunner.h"

#include "core/ConfigJson.h"
#include "core/LockIdentity.h"
#include "core/TrustedConfig.h"

#include <QCoreApplication>
#include <QDateTime>

#include <exception>
#include <thread>

namespace asb {

int exitCode(RunnerStart s)
{
    switch (s) {
    case RunnerStart::Ok:
    case RunnerStart::ConfigError: return 0;
    case RunnerStart::IdentityRefused: return 2;
    case RunnerStart::Untrusted:
    case RunnerStart::DataFolderUnknown: return 3;
    case RunnerStart::EngineLockBusy: return 4;
    case RunnerStart::NotActivated: return 6;
    case RunnerStart::StaleFirewall: return 7;
    }
    return 1;
}

QString notActivatedLine(const QString& why)
{
    return QStringLiteral("Not activated (%1): the bridge was stopped by Stop, or an apply did not finish. It stays "
                          "stopped until Start or Save & apply; this start is not a failure, so the service manager "
                          "does not restart it.")
        .arg(why.isEmpty() ? QStringLiteral("no valid start token") : why);
}

namespace {
// The steady_clock deadline that carries a running-time deadline over to a seam taking steady time (the
// inverse of RunningTime::fromSteady, which the watchdog applies at the call). Steady time is sampled FIRST:
// a pause between the samples then makes runningNow later, which shortens the carried budget, never
// lengthens it.
Deadline toSteady(RunningTime::Duration runningDeadline, const RunningTime::Clocks& clocks)
{
    const Deadline steadyNow = clocks.steady();
    const RunningTime::Duration runningNow = clocks.running();
    return steadyNow + std::chrono::duration_cast<Deadline::duration>(runningDeadline - runningNow);
}

ServiceLocations resolved(const std::optional<AppLocations>& l)
{
    ServiceLocations out;
    if (!l) {
        out.failure = RunnerStart::DataFolderUnknown;
        out.error = QString::fromLatin1(kWindowsDataFolderError);
        return out;
    }
    out.locations = l;
    return out;
}
}  // namespace

ServiceLocations serviceLocations(bool windows, const std::function<QString()>& programData)
{
    return resolved(AppLocations::forOs(windows, programData));
}

ServiceLocations serviceLocations()
{
    return resolved(AppLocations::forThisOs());
}

BridgeRunner::BridgeRunner(IEngineLock& lock, IStartToken& token, IDataFolder& folder, IBridgeEngine& engine,
                           ISideChannel& side, IFirewallMonitor& monitor, IDeadlineWatchdog& watchdog,
                           QByteArray instanceId, IClock& clock, Spawn spawn, SleepUntil sleepUntil,
                           RunningTime::Clocks running)
    : m_lock(lock), m_token(token), m_folder(folder), m_engine(engine), m_side(side), m_monitor(monitor),
      m_watchdog(watchdog), m_instanceId(std::move(instanceId)), m_clock(clock), m_running(std::move(running)),
      m_spawn(std::move(spawn)), m_sleepUntil(std::move(sleepUntil))
{
    if (!m_running.running) m_running.running = [] { return RunningTime::now(); };
    if (!m_running.steady) m_running.steady = [&clock] { return clock.now(); };
    if (!m_spawn) m_spawn = [](std::function<void()> f) { std::thread(std::move(f)).detach(); };
    if (!m_sleepUntil)
        m_sleepUntil = [&clock](Deadline d) {
            const auto now = clock.now();
            if (d > now) std::this_thread::sleep_for(d - now);
            return true;
        };
    m_statusTimer.setInterval(1000);
    QObject::connect(&m_statusTimer, &QTimer::timeout, &m_context, [this] { publishNow(); });
}

BridgeRunner::~BridgeRunner()
{
    std::lock_guard g(m_shared->m);
    m_shared->abandoned = true;
    m_shared->cv.notify_all();
}

RunnerStart BridgeRunner::start()
{
    // 1. engine.lock before anything is read (INV-103), then its identity (D58).
    LockResult why = LockResult::Busy;
    m_engineHold = m_lock.tryAcquire(&why);
    if (!m_engineHold) return RunnerStart::EngineLockBusy;
    if (LockIdentity::classify(m_lock.readHeldLock(), m_lock.readRecord()) != LockIdentity::PairState::Matching)
        return RunnerStart::EngineLockBusy;
    // 2. Service-mode trust: never mutates (INV-22).
    const OpenedFolder folder = TrustedConfig::open(m_folder, TrustMode::Service);
    if (!folder.usable) return RunnerStart::Untrusted;
    // 3. The activation fence, before any config read (INV-113).
    QString tokenWhy;
    const std::optional<IStartToken::Token> token = m_token.read(&tokenWhy);
    if (!token) {
        m_tokenRefusal = tokenWhy.isEmpty() ? QStringLiteral("no valid start token") : tokenWhy;
        return RunnerStart::NotActivated;
    }
    m_startToken = token->token;
    m_generation = token->generation;
    // 4. config.json only — never the pending file (INV-43).
    const ConfigFiles::Loaded loaded = TrustedConfig::readCommitted(m_folder, folder.inspection);
    m_configSha256 = sha256Hex(loaded.bytes);
    m_startedAt = QDateTime::currentDateTimeUtc();
    if (!loaded.config) {
        m_configError = loaded.error;
    } else if (const QStringList errors = loaded.config->validate(); !errors.isEmpty()) {
        m_configError = errors.join(QLatin1Char(' '));
    } else {
        m_config = *loaded.config;
    }
    if (m_configError) {
        // Zero ports: no listener can open, so there is no exposure to check (INV-34).
        publishNow();
        m_statusTimer.start();
        return RunnerStart::ConfigError;
    }
    // 5. The firewall monitor, armed before any listener (INV-119).
    if (m_monitor.enabled() && !startupChecks()) return RunnerStart::StaleFirewall;
    // 6. Listeners and serial ports.
    m_engine.start(m_config);
    publishNow();
    m_statusTimer.start();
    return RunnerStart::Ok;
}

namespace {
QString checkName(IFirewallMonitor::Check c)
{
    switch (c) {
    case IFirewallMonitor::Check::Match: return QStringLiteral("matched");
    case IFirewallMonitor::Check::Mismatch: return QStringLiteral("found a mismatch");
    case IFirewallMonitor::Check::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

qint64 ms(Deadline::duration d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); }

// Exit 7's one line (v1.1 station fix). Never throws: the caller ends the process next.
void sayLastWords(const BridgeRunner::LastWords& f, const QString& text)
{
    if (!f) return;
    try {
        f(text + QStringLiteral(" The bridge stops (exit 7)."));
    } catch (...) {
    }
}
}  // namespace

bool BridgeRunner::startupChecks()
{
    m_watchdog.arm(m_clock.now() + kCheckBudget, kExitStaleFirewall);
    m_shared->monitor = &m_monitor;
    m_shared->watchdog = &m_watchdog;
    m_shared->clock = &m_clock;
    m_shared->running = m_running;
    m_shared->sleepUntil = m_sleepUntil;
    m_shared->config = m_config;
    m_shared->generation = m_generation;
    m_spawn([shared = m_shared] { monitorMain(shared); });
    QStringList took;   // each startup check's duration, for the stale line
    for (int attempt = 0; attempt < kStaleLimit; ++attempt) {
        Deadline t0, t1;
        IFirewallMonitor::Check result;
        QString why;
        {
            std::unique_lock lk(m_shared->m);
            ++m_shared->requested;
            m_shared->cv.notify_all();
            const int want = m_shared->requested;
            // Bounded by the watchdog in production (it ends the process 5 s after the check began);
            // the runner gives up at the same point, so a test's runner returns instead of hanging. Both
            // count running time (N9-1): a suspend while the check is outstanding is not part of its 5 s,
            // so the runner never expires a check the watchdog would still let finish.
            const RunningTime::Duration giveUp = m_running.running() + kCheckBudget;
            while (m_shared->done < want) {
                if (m_running.running() > giveUp) {
                    m_shared->abandoned = true;
                    m_shared->cv.notify_all();
                    const LastWords say = m_shared->lastWords;
                    lk.unlock();
                    // A stale success may have left its start + 35 s armed (the retry has not tightened yet
                    // P2): bring it to now + 5 s before speaking. `abandoned` (set above) stops a late Match extending it.
                    m_watchdog.tighten(m_clock.now() + kCheckBudget, kExitStaleFirewall);
                    sayLastWords(say, QStringLiteral("The startup firewall check did not finish within %1 s.")
                                          .arg(qint64(kCheckBudget.count())));
                    m_watchdog.expireNow();
                    return false;
                }
                m_shared->cv.wait_for(lk, std::chrono::milliseconds(10));
            }
            t0 = m_shared->lastStart;
            t1 = m_shared->lastEnd;
            result = m_shared->last;
            why = m_shared->lastWhy;
        }
        if (result != IFirewallMonitor::Check::Match) {
            // At startup the monitor leaves the expiry to the runner (no listener is open yet), so the reason is
            // said first; the deadline armed for this check (its start + 5 s) still bounds the line.
            sayLastWords(m_shared->lastWords, QStringLiteral("The startup firewall check %1: %2.")
                                                  .arg(checkName(result),
                                                       why.isEmpty() ? QStringLiteral("no reason was given") : why));
            m_watchdog.expireNow();
            return false;
        }
        // Fresh: the check ENDED at most 1 s ago (a pause between check and listen re-checks), and it spanned no
        // suspend (steady time inside it within the 5 s budget; a check that slept is checked again). Measured from
        // its end, not its start: the time a check itself takes is not a pause (v1.1 station fix: a station's check
        // takes about 1.3 s, so it was stale three times and the service exited 7 on every start).
        const Deadline::duration age = m_clock.now() - t1;
        if (age <= kFreshness && t1 - t0 <= kCheckBudget) {
            std::lock_guard g(m_shared->m);
            m_shared->periodic = true;
            m_shared->cv.notify_all();
            return true;
        }
        // Stale: the check spanned a suspend, or more than 1 s passed between its end and now; check again.
        took << (t1 - t0 > kCheckBudget
                     ? QStringLiteral("took %1 ms, spanning a suspend").arg(ms(t1 - t0))
                     : QStringLiteral("took %1 ms, ended %2 ms before listen").arg(ms(t1 - t0)).arg(ms(age)));
    }
    {
        std::lock_guard g(m_shared->m);
        m_shared->abandoned = true;
        m_shared->cv.notify_all();
    }
    // Each stale success extended the deadline to its start + 35 s: bring it back to 5 s before speaking,
    // so this line too is bounded by a 5 s budget like every other exit-7 line.
    m_watchdog.tighten(m_clock.now() + kCheckBudget, kExitStaleFirewall);
    sayLastWords(m_shared->lastWords,
                 QStringLiteral("The startup firewall check matched %1 times, but no result was fresh when the listeners "
                                "were due to open (at most %2 s after the check ended, and no suspend inside it): %3.")
                     .arg(kStaleLimit)
                     .arg(qint64(kFreshness.count()))
                     .arg(took.join(QStringLiteral("; "))));
    m_watchdog.expireNow();
    return false;
}

void BridgeRunner::monitorMain(const std::shared_ptr<Shared>& shared)
{
    // Runs on its own thread: no logging through the side channel, no status, no event loop (D56). It uses only
    // `shared` (which it co-owns) and the host's monitor, watchdog and clock, never the runner object. Its one
    // write is a failed periodic check's last words, made while that check's 5 s deadline is armed.
    IFirewallMonitor& monitor = *shared->monitor;
    IDeadlineWatchdog& watchdog = *shared->watchdog;
    IClock& clock = *shared->clock;
    monitor.threadInit();
    const RunningTime::Clocks& running = shared->running;
    // A failed check does not expire here: its caller says why first (the runner at startup, the periodic loop below),
    // then expires. Only a successful check extends the deadline, so this check's start + 5 s still bounds both.
    auto check = [&](Deadline& t0, Deadline& t1, QString& why) {
        // t0 (steady) dates the check for the 30 s schedule; r0 (running time) dates it for the 35 s it grants;
        // t1 (steady) is when it returned, for the startup freshness. tighten converts at once, before anything
        // can suspend; extend comes after checkOnce, so its budget is carried over in running time: a suspend
        // inside the check does not shorten it (N9-1).
        t0 = clock.now();
        const RunningTime::Duration r0 = running.running();
        watchdog.tighten(t0 + kCheckBudget, kExitStaleFirewall);   // each check has its own 5 s budget
        const IFirewallMonitor::Check r = monitor.checkOnce(shared->config, shared->generation);
        t1 = clock.now();
        why = r == IFirewallMonitor::Check::Match ? QString() : monitor.lastWhy();
        if (r == IFirewallMonitor::Check::Match) {
            // Never after the runner gave up at startup: it has tightened the deadline for its exit-7
            // line, and a late success must not raise it again. Checked and extended under the lock the runner sets it
            // under. Once periodic, `abandoned` only means the runner object was destroyed: the check stays valid.
            std::lock_guard g(shared->m);
            if (shared->periodic || !shared->abandoned) watchdog.extend(toSteady(r0 + kCheckPeriod + kCheckBudget, running));
        }
        return r;
    };
    // Startup: one check per request from the runner.
    int served = 0;
    for (;;) {
        {
            std::unique_lock lk(shared->m);
            shared->cv.wait(lk, [&] { return shared->abandoned || shared->periodic || shared->requested > served; });
            if (shared->abandoned) return;
            if (shared->periodic) break;
        }
        Deadline t0, t1;
        QString why;
        const IFirewallMonitor::Check r = check(t0, t1, why);
        {
            std::lock_guard g(shared->m);
            ++served;
            shared->lastStart = t0;
            shared->lastEnd = t1;
            shared->lastWhy = why;
            shared->last = r;
            shared->done = served;
            shared->cv.notify_all();
            if (r != IFirewallMonitor::Check::Match) return;
        }
    }
    // Periodic: next check 30 s after the last good one began.
    Deadline lastGood;
    {
        std::lock_guard g(shared->m);
        lastGood = shared->lastStart;
    }
    for (;;) {
        if (!shared->sleepUntil(lastGood + kCheckPeriod)) return;
        {
            std::lock_guard g(shared->m);
            if (shared->abandoned) return;
        }
        Deadline t0, t1;
        QString why;
        const IFirewallMonitor::Check r = check(t0, t1, why);
        if (r != IFirewallMonitor::Check::Match) {
            // The listeners are open: the line is bounded by this check's deadline (t0 + 5 s, tightened above),
            // which the watchdog enforces whatever the write does; then the process ends at once.
            sayLastWords(shared->lastWords, QStringLiteral("The periodic firewall check %1: %2.")
                                                .arg(checkName(r), why.isEmpty() ? QStringLiteral("no reason was given") : why));
            watchdog.expireNow();
            return;
        }
        lastGood = t0;
    }
}

StatusReport BridgeRunner::report() const
{
    StatusReport r;
    r.updated = QDateTime::currentDateTimeUtc();
    r.startedAt = m_startedAt;
    r.pid = QCoreApplication::applicationPid();
    r.instanceId = m_instanceId;
    r.startToken = m_startToken;
    r.configSha256 = m_configSha256;
    r.configError = m_configError;
    r.ports = m_engine.snapshot();
    return r;
}

void BridgeRunner::publishNow()
{
    // Bridging matters more than the status file (INV-8): nothing a status write does escapes.
    try {
        m_side.publishStatus(report());
    } catch (const std::exception&) {
    } catch (...) {
    }
}

void BridgeRunner::stop(std::function<void()> done)
{
    m_statusTimer.stop();
    // The engine-lock hold is NOT released here: only process death releases it (INV-103).
    m_engine.stop([this, done = std::move(done)] {
        publishNow();
        if (done) done();
    });
}

}  // namespace asb
