#pragma once

// ISystemdBus over QtDBus on the system bus (WS-8, D51). Static Qt is configured with -dbus-runtime, which
// loads libdbus-1 with dlopen: no process is created. Methods are blocking QDBus::Block calls with the given
// timeout; the receivers are a QObject living on the thread that installed them, connected to the manager's
// JobRemoved signal and to the bus daemon's NameOwnerChanged for org.freedesktop.systemd1. Their arrivals
// are queued and handed to the receiver functions inside pump(), which runs a local event loop on that
// thread (the Applier's operation worker), so an early completion is captured even while the worker is
// blocked in the method call that produced it.
//
// Needs a QCoreApplication (QtDBus does).

#include "platform/posix/SystemdBus.h"

#include <memory>

namespace asb::posix {

class QtSystemdBus final : public ISystemdBus {
public:
    QtSystemdBus();
    ~QtSystemdBus() override;

    void setReceivers(Receivers) override;
    void pump(Ms timeout) override;

    void subscribe(Ms) override;
    QString loadUnit(const QString& name, Ms) override;
    QVariant unitProperty(const QString& unitPath, const QString& interface, const QString& property, Ms) override;
    QList<SystemdUnitProcess> unitProcesses(const QString& name, Ms) override;
    void maskUnitFiles(const QStringList& files, bool runtime, bool force, Ms) override;
    void unmaskUnitFiles(const QStringList& files, bool runtime, Ms) override;
    void enableUnitFiles(const QStringList& files, bool runtime, bool force, Ms) override;
    void disableUnitFiles(const QStringList& files, bool runtime, Ms) override;
    void reload(Ms) override;
    QString startUnit(const QString& name, const QString& mode, Ms) override;
    QString stopUnit(const QString& name, const QString& mode, Ms) override;
    QList<SystemdJob> listJobs(Ms) override;
    void cancelJob(quint32 id, Ms) override;

    struct Impl;

private:
    std::unique_ptr<Impl> d;
};

}  // namespace asb::posix
