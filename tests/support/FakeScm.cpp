#include "support/FakeScm.h"

#include "platform/win/ServiceStatus.h"

namespace asb::test {

using namespace asb::win;

quint32 FakeScm::step(const QString& call)
{
    log << call;
    if (onCall) onCall(call);
    return fail.value(call.section(QLatin1Char(' '), 0, 0), 0);
}

quint32 FakeScm::openService(const QString& name, quint32 access, Handle* out)
{
    if (const quint32 e = step(QStringLiteral("open %1 0x%2").arg(name).arg(access, 0, 16))) return e;
    if (!service || service->name != name) return scm::kErrorServiceDoesNotExist;
    ++openHandles;
    *out = 1;
    return 0;
}

quint32 FakeScm::createService(const QString& name, const QString& displayName, const QString& binaryPath,
                               quint32 startType, const QString& account, Handle* out)
{
    if (const quint32 e = step(QStringLiteral("create %1 startType %2").arg(name).arg(startType))) return e;
    if (service) return scm::kErrorServiceExists;
    service = Service{name, displayName, binaryPath, account, {}, startType, {}, 0, false, {1, 0, 0, 0}};
    ++openHandles;
    *out = 1;
    return 0;
}

quint32 FakeScm::changeConfig(Handle, quint32 startType, const QString& binaryPath, const QString& displayName,
                              const QString& account)
{
    if (const quint32 e = step(QStringLiteral("config startType %1").arg(startType))) return e;
    service->startType = startType;
    service->binaryPath = binaryPath;
    service->displayName = displayName;
    service->account = account;
    return 0;
}

quint32 FakeScm::changeStartType(Handle, quint32 startType)
{
    if (const quint32 e = step(QStringLiteral("startType %1").arg(startType))) return e;
    service->startType = startType;
    return 0;
}

quint32 FakeScm::setDescription(Handle, const QString& d)
{
    if (const quint32 e = step(QStringLiteral("description"))) return e;
    service->description = d;
    return 0;
}

quint32 FakeScm::setFailureActions(Handle, const QList<ServicePlan::FailureAction>& a, int reset)
{
    if (const quint32 e = step(QStringLiteral("failureActions"))) return e;
    service->failureActions = a;
    service->resetSeconds = reset;
    return 0;
}

quint32 FakeScm::setFailureFlag(Handle, bool f)
{
    if (const quint32 e = step(QStringLiteral("failureFlag"))) return e;
    service->failureFlag = f;
    return 0;
}

quint32 FakeScm::queryConfig(Handle, ScmServiceConfig* c)
{
    if (const quint32 e = step(QStringLiteral("queryConfig"))) return e;
    c->binaryPath = service->binaryPath;
    c->startType = service->startType;
    c->account = service->account;
    c->displayName = service->displayName;
    return 0;
}

quint32 FakeScm::queryFailureActions(Handle, QList<ServicePlan::FailureAction>* a, int* reset, bool* flag)
{
    if (const quint32 e = step(QStringLiteral("queryFailureActions"))) return e;
    *a = service->failureActions;
    *reset = service->resetSeconds;
    *flag = service->failureFlag;
    return 0;
}

quint32 FakeScm::queryStatus(Handle, ScmStatus* s)
{
    if (const quint32 e = step(QStringLiteral("queryStatus"))) return e;
    *s = service->status;
    return 0;
}

quint32 FakeScm::control(Handle, quint32 c)
{
    if (const quint32 e = step(QStringLiteral("control %1").arg(c))) return e;
    if (onControl) return onControl(c);
    if (service->status.state == svc::kStopped) return scm::kErrorServiceNotActive;
    if (service->status.state == svc::kStartPending) return scm::kErrorInvalidServiceControl;   // as measured
    service->status.state = svc::kStopPending;
    return 0;
}

quint32 FakeScm::start(Handle)
{
    if (const quint32 e = step(QStringLiteral("start"))) return e;
    if (service->startType == scm::kDisabled) return scm::kErrorServiceDisabled;
    if (service->status.state != svc::kStopped) return scm::kErrorServiceAlreadyRunning;
    service->status = {svc::kRunning, 4242, 0, 0};
    return 0;
}

quint32 FakeScm::remove(Handle)
{
    if (const quint32 e = step(QStringLiteral("delete"))) return e;
    service.reset();
    return 0;
}

void FakeScm::close(Handle)
{
    log << QStringLiteral("close");
    --openHandles;
}

quint32 FakeScm::flushServiceKey(const QString& name) { return step(QStringLiteral("flush %1").arg(name)); }

quint32 FakeScm::readFileHead(const QString& path, qint64 max, QByteArray* head, qint64* size)
{
    if (const quint32 e = step(QStringLiteral("read %1").arg(path))) return e;
    const auto it = files.constFind(path.toLower());
    if (it == files.constEnd()) return scm::kErrorFileNotFound;
    *head = it->left(max);
    *size = it->size();
    return 0;
}

bool FakeScm::fileExists(const QString& path)
{
    log << QStringLiteral("exists %1").arg(path);
    return files.contains(path.toLower());
}

quint32 FakeScm::processesOfImage(const QString& path, QList<quint32>* out)
{
    out->clear();
    if (const quint32 e = step(QStringLiteral("processes %1").arg(path))) return e;   // unknown: `out` incomplete
    *out = processes.value(path.toLower());
    return 0;
}

}  // namespace asb::test
