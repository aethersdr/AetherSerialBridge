#include "core/Interfaces.h"

#include <QRandomGenerator>

#include <thread>

namespace asb {
namespace {

class SystemClock final : public IClock {
public:
    Deadline now() const override { return std::chrono::steady_clock::now(); }
    void sleepFor(std::chrono::milliseconds d) override { std::this_thread::sleep_for(d); }
};

}  // namespace

IClock& systemClock()
{
    static SystemClock clock;
    return clock;
}

FirewallGeneration FirewallGeneration::fresh()
{
    quint32 words[2];
    QRandomGenerator::system()->fillRange(words);
    QByteArray hex;
    for (quint32 w : words) hex += QByteArray::number(w, 16).rightJustified(8, '0');
    return FirewallGeneration{hex};
}

bool FirewallGeneration::isValidId(const QByteArray& id)
{
    if (id.size() != 16) return false;
    for (char c : id)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

QString enumName(Outcome o)
{
    switch (o) {
    case Outcome::NothingChanged: return QStringLiteral("NothingChanged");
    case Outcome::Uncommitted: return QStringLiteral("Uncommitted");
    case Outcome::CommittedNotRunning: return QStringLiteral("CommittedNotRunning");
    case Outcome::StateUnknown: return QStringLiteral("StateUnknown");
    case Outcome::SavedNotApplied: return QStringLiteral("SavedNotApplied");
    case Outcome::Ok: return QStringLiteral("Ok");
    }
    return {};
}

QString enumName(Persisted p)
{
    switch (p) {
    case Persisted::None: return QStringLiteral("None");
    case Persisted::Staged: return QStringLiteral("Staged");
    case Persisted::Committed: return QStringLiteral("Committed");
    }
    return {};
}

QString describeServiceExit(const ServiceQuery& q)
{
    if (q.state == ServiceQuery::Running || q.state == ServiceQuery::StartPending) return {};
    if (q.endedUnreported)
        return QStringLiteral("the service ended without reporting why (Windows error 1067). At start that can be, for "
                              "example, a fault, or the firewall check finding rules that do not match the saved "
                              "settings (exit 7); it can also have another cause. The System event log (Service Control "
                              "Manager) has the time");
    // Stopped with no code (v1.1 station fix): Windows reports a start that found no valid start token as
    // a normal stop (NO_ERROR, so SCM never restarts it); the bridge's own line says which it was. Linux fills the
    // code from ExecMainStatus, so there this is only a signal or a unit that never ran: the journal has the reason.
    if (!q.exitCode)
        return q.state == ServiceQuery::Stopped
                   ? QStringLiteral("the service stopped without an error code. On Windows that is a stop, or a start "
                                    "that found no valid start token (not activated; Start writes one), and bridge.log "
                                    "says which; on Linux the journal has the reason")
                   : QString();
    switch (*q.exitCode) {
    case 0: return {};
    case 1: return QStringLiteral("the service stopped after a fault (exit 1)");
    case 2: return QStringLiteral("the service refused to run: it was not started by the service manager, or its identity "
                                  "was refused (exit 2)");
    case 3: return QStringLiteral("the service stopped: the data folder is not trusted or cannot be found (exit 3); "
                                  "Repair fixes it");
    case 4: return QStringLiteral("the service stopped: engine.lock is held by another process or is not secured "
                                  "(exit 4)");
    case 5: return QStringLiteral("the service stopped: its process guard could not be installed (exit 5)");
    // Linux (the process still exits 6), and a Windows service of a build before v1.1's station fix.
    case 6: return QStringLiteral("the service stopped: it is not activated, there is no valid start token (exit 6); "
                                  "Start writes one");
    case 7: return QStringLiteral("the service stopped: the firewall rules do not match the saved settings (exit 7)");
    default: return QStringLiteral("the service stopped with exit code %1").arg(*q.exitCode);
    }
}

}  // namespace asb
