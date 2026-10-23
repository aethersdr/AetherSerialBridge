#pragma once

// systemd's manager (org.freedesktop.systemd1, /org/freedesktop/systemd1, org.freedesktop.systemd1.Manager)
// as the Linux service adapter uses it (WS-8; D51, INV-115). All service control is over D-Bus: no
// systemctl, no child process. Production: QtSystemdBus (QtDBus on the system bus). Tests: a fake that can
// deliver JobRemoved before the method's reply, report the same unit under another job path, change the
// manager's owner and record every call's timeout.
//
// Signal delivery model (QtDBus): a signal reaches a receiver only if the receiver was connected when the
// signal arrived (QDBusConnection::connect adds the match rule); arrivals are queued and handed to the
// receivers inside pump(), on the thread that calls pump(). So an early JobRemoved — one that arrives
// before the reply carrying its job path — is kept only if the receivers were installed BEFORE the call.

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <chrono>
#include <functional>
#include <stdexcept>

namespace asb::posix {

// A D-Bus error reply: `name` is the error name ("org.freedesktop.systemd1.NoSuchUnit"), what() the message.
class BusError : public std::runtime_error {
public:
    BusError(QString name, const QString& message)
        : std::runtime_error(message.toStdString()), m_name(std::move(name))
    {
    }
    const QString& name() const { return m_name; }
    // NoSuchUnit, or the "not loaded" form some calls use for a unit systemd does not know.
    bool noSuchUnit() const;

private:
    QString m_name;
};

struct SystemdJob {
    quint32 id = 0;
    QString unit, type, state, jobPath, unitPath;
};

struct SystemdUnitProcess {
    QString cgroup;
    quint32 pid = 0;
    QString command;
};

class ISystemdBus {
public:
    using Ms = std::chrono::milliseconds;
    virtual ~ISystemdBus() = default;

    struct Receivers {
        std::function<void(quint32 id, const QString& job, const QString& unit, const QString& result)> jobRemoved;
        std::function<void()> managerOwnerChanged;   // org.freedesktop.systemd1 lost or changed its owner
    };
    // Connects the receivers (replacing any earlier ones). Signals arriving from now on are queued and
    // delivered in pump().
    virtual void setReceivers(Receivers) = 0;
    // Delivers queued signals; when none is queued, waits at most `timeout` for one to arrive.
    virtual void pump(Ms timeout) = 0;

    // Manager methods. Every one takes the call's timeout (the remaining phase budget) and throws BusError
    // on an error reply or a timeout.
    virtual void subscribe(Ms) = 0;
    virtual QString loadUnit(const QString& name, Ms) = 0;                              // unit object path
    virtual QVariant unitProperty(const QString& unitPath, const QString& interface, const QString& property, Ms) = 0;
    virtual QList<SystemdUnitProcess> unitProcesses(const QString& name, Ms) = 0;       // GetUnitProcesses
    virtual void maskUnitFiles(const QStringList& files, bool runtime, bool force, Ms) = 0;
    virtual void unmaskUnitFiles(const QStringList& files, bool runtime, Ms) = 0;
    virtual void enableUnitFiles(const QStringList& files, bool runtime, bool force, Ms) = 0;
    virtual void disableUnitFiles(const QStringList& files, bool runtime, Ms) = 0;
    virtual void reload(Ms) = 0;
    virtual QString startUnit(const QString& name, const QString& mode, Ms) = 0;       // job object path
    virtual QString stopUnit(const QString& name, const QString& mode, Ms) = 0;        // job object path
    virtual QList<SystemdJob> listJobs(Ms) = 0;
    virtual void cancelJob(quint32 id, Ms) = 0;
};

}  // namespace asb::posix
