#pragma once

// An in-memory systemd manager behind ISystemdBus (WS-8 seam tests; INV-79, INV-115).
//
// Signal model, as QtDBus delivers it: a JobRemoved (or a NameOwnerChanged) reaches the receivers only if they
// were installed when it was emitted; it is queued and handed over in pump(). A completion planned
// BeforeReply is emitted while the method call is still running (before its reply); AfterReply is emitted
// once the call has returned (at the next pump). pump() advances the fake clock by its timeout when nothing
// is queued, so waits cost no real time.

#include "platform/posix/SystemdBus.h"
#include "support/Fakes.h"

#include <QMap>
#include <QStringList>

#include <deque>
#include <functional>

namespace asb::test {

class FakeSystemdBus final : public asb::posix::ISystemdBus {
public:
    explicit FakeSystemdBus(FakeClock& clock) : m_clock(clock) {}

    struct Unit {
        bool masked = false, enabled = false;
        QString active = QStringLiteral("inactive");
        quint32 mainPid = 0;
        int execMainCode = 0, execMainStatus = 0;   // how the main process last ended (CLD_EXITED = 1)
        QList<asb::posix::SystemdUnitProcess> processes;
        QString load = QStringLiteral("not-found");   // what LoadState reports (recomputed by Reload)
    };
    QMap<QString, Unit> units;
    // Whether the unit file exists on disk (Reload computes LoadState from it).
    std::function<bool(const QString& unit)> unitFileExists = [](const QString&) { return false; };

    enum class When { BeforeReply, AfterReply, Never };
    struct JobPlan {
        When when = When::AfterReply;
        QString result = QStringLiteral("done");
        // Completions emitted first, for the same unit under OTHER job paths: {path, result}.
        QList<QPair<QString, QString>> others;
        bool ownerChange = false;   // the manager's bus name changes owner instead of completing
        // The owner change AND the completion, both queued for the same pump: which the
        // adapter looks at first must not matter.
        bool ownerChangeWithCompletion = false;
    };
    QList<JobPlan> plans;           // consumed by startUnit/stopUnit in order; default: AfterReply, done
    QList<asb::posix::SystemdJob> jobs;   // ListJobs; a "running" job finishes after runningPumps pumps
    int runningPumps = 2;
    bool ownerChangeWhenJobsFinish = false;   // the pump that finishes a running job also changes the owner
    QMap<QString, QString> failures;      // method -> D-Bus error name to throw

    struct Call {
        QString method;
        QStringList args;
        std::chrono::milliseconds timeout{0};
        bool receiversInstalled = false;
    };
    QList<Call> calls;
    QList<std::chrono::milliseconds> pumpTimeouts;
    std::function<void(const QString& method)> onCall;
    int receiverInstalls = 0;
    QStringList methods() const;

    void setReceivers(Receivers r) override;
    void pump(Ms timeout) override;
    void subscribe(Ms) override;
    QString loadUnit(const QString& name, Ms) override;
    QVariant unitProperty(const QString& unitPath, const QString& interface, const QString& property, Ms) override;
    QList<asb::posix::SystemdUnitProcess> unitProcesses(const QString& name, Ms) override;
    void maskUnitFiles(const QStringList& files, bool runtime, bool force, Ms) override;
    void unmaskUnitFiles(const QStringList& files, bool runtime, Ms) override;
    void enableUnitFiles(const QStringList& files, bool runtime, bool force, Ms) override;
    void disableUnitFiles(const QStringList& files, bool runtime, Ms) override;
    void reload(Ms) override;
    QString startUnit(const QString& name, const QString& mode, Ms) override;
    QString stopUnit(const QString& name, const QString& mode, Ms) override;
    QList<asb::posix::SystemdJob> listJobs(Ms) override;
    void cancelJob(quint32 id, Ms) override;

private:
    struct Event {
        bool owner = false;
        quint32 id = 0;
        QString job, unit, result;
    };
    void record(const QString& method, QStringList args, Ms timeout);
    void emitEvent(const Event& e);   // dropped unless receivers are installed
    QString job(const QString& unit, bool start);
    static QString unitPath(const QString& name) { return QStringLiteral("/org/freedesktop/systemd1/unit/") + name; }

    FakeClock& m_clock;
    Receivers m_receivers;
    bool m_installed = false;
    std::deque<Event> m_queue;
    std::deque<Event> m_afterReply;
    QMap<quint32, int> m_runningPumps;
    quint32 m_nextJob = 100;
};

}  // namespace asb::test
