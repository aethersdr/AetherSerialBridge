#pragma once

// A recording service control manager behind ScmSeams (WS-7 tests): one service at most, its configuration and
// state, the registry flushes, and a file table for the read-only evidence reads. Every call is logged
// ("open", "create", "startType 4", "flush", "control 1", ...) so the order is assertable.

#include "platform/win/ScmSeams.h"

#include <QMap>
#include <QStringList>

#include <functional>

namespace asb::test {

class FakeScm final : public asb::win::ScmSeams {
public:
    struct Service {
        QString name, displayName, binaryPath, account, description;
        quint32 startType = 0;
        QList<ServicePlan::FailureAction> failureActions;
        int resetSeconds = 0;
        bool failureFlag = false;
        win::ScmStatus status{1, 0, 0, 0};
    };
    std::optional<Service> service;
    QMap<QString, QByteArray> files;     // path (lower case) -> bytes
    QStringList log;
    QMap<QString, quint32> fail;         // call name -> Win32 error
    std::function<quint32(quint32 control)> onControl;   // default: STOP -> STOP_PENDING

    quint32 openService(const QString& name, quint32 access, Handle* out) override;
    quint32 createService(const QString& name, const QString& displayName, const QString& binaryPath, quint32 startType,
                          const QString& account, Handle* out) override;
    quint32 changeConfig(Handle, quint32 startType, const QString& binaryPath, const QString& displayName,
                         const QString& account) override;
    quint32 changeStartType(Handle, quint32 startType) override;
    quint32 setDescription(Handle, const QString&) override;
    quint32 setFailureActions(Handle, const QList<ServicePlan::FailureAction>&, int resetSeconds) override;
    quint32 setFailureFlag(Handle, bool) override;
    quint32 queryConfig(Handle, win::ScmServiceConfig*) override;
    quint32 queryFailureActions(Handle, QList<ServicePlan::FailureAction>*, int* resetSeconds, bool* flag) override;
    quint32 queryStatus(Handle, win::ScmStatus*) override;
    quint32 control(Handle, quint32 control) override;
    quint32 start(Handle) override;
    quint32 remove(Handle) override;
    void close(Handle) override;
    quint32 flushServiceKey(const QString& name) override;
    quint32 readFileHead(const QString& path, qint64 max, QByteArray* head, qint64* size) override;
    bool fileExists(const QString& path) override;
    quint32 processesOfImage(const QString& path, QList<quint32>* out) override;   // fail key "processes"
    QMap<QString, QList<quint32>> processes;   // image path (lower case) -> live session-0 pids
    // Called after every logged call (e.g. to make it return after a deadline: advance the clock).
    std::function<void(const QString& call)> onCall;

    int openHandles = 0;

private:
    quint32 step(const QString& call);
};

}  // namespace asb::test
