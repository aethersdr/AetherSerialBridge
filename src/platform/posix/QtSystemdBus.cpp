#include "platform/posix/QtSystemdBus.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QEventLoop>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <deque>

namespace asb::posix {
namespace {

constexpr char kService[] = "org.freedesktop.systemd1";
constexpr char kPath[] = "/org/freedesktop/systemd1";
constexpr char kManager[] = "org.freedesktop.systemd1.Manager";

int toMs(std::chrono::milliseconds t)
{
    if (t.count() < 1) return 1;
    if (t.count() > 0x7fffffff) return 0x7fffffff;
    return int(t.count());
}

struct Event {
    bool owner = false;
    quint32 id = 0;
    QString job, unit, result;
};

}  // namespace

// The QObject that receives the signals. It lives on the thread that installed the receivers; its slots only
// queue the arrival, and pump() hands the queue to the receiver functions.
class SystemdSignalSink : public QObject {
    Q_OBJECT
public:
    std::deque<Event> queue;
    QEventLoop* waiting = nullptr;

public slots:
    void onJobRemoved(uint id, const QDBusObjectPath& job, const QString& unit, const QString& result)
    {
        queue.push_back(Event{false, quint32(id), job.path(), unit, result});
        if (waiting) waiting->quit();
    }
    void onNameOwnerChanged(const QString& name, const QString& oldOwner, const QString& newOwner)
    {
        Q_UNUSED(oldOwner);
        Q_UNUSED(newOwner);
        if (name != QLatin1String(kService)) return;
        queue.push_back(Event{true, 0, {}, {}, {}});
        if (waiting) waiting->quit();
    }
};

struct QtSystemdBus::Impl {
    QDBusConnection bus = QDBusConnection::systemBus();
    std::unique_ptr<SystemdSignalSink> sink;
    Receivers receivers;

    QDBusMessage call(const char* method, const QVariantList& args, std::chrono::milliseconds timeout,
                      const QString& path = QString::fromLatin1(kPath), const char* iface = kManager)
    {
        if (!bus.isConnected())
            throw BusError(QStringLiteral("org.freedesktop.DBus.Error.Disconnected"),
                           QStringLiteral("the system bus is not available: %1").arg(bus.lastError().message()));
        QDBusMessage m = QDBusMessage::createMethodCall(QString::fromLatin1(kService), path, QString::fromLatin1(iface),
                                                        QString::fromLatin1(method));
        m.setArguments(args);
        const QDBusMessage r = bus.call(m, QDBus::Block, toMs(timeout));
        if (r.type() == QDBusMessage::ErrorMessage)
            throw BusError(r.errorName(), QStringLiteral("%1: %2").arg(QString::fromLatin1(method), r.errorMessage()));
        if (r.type() != QDBusMessage::ReplyMessage)
            throw BusError(QStringLiteral("org.freedesktop.DBus.Error.Failed"),
                           QStringLiteral("%1: no reply").arg(QString::fromLatin1(method)));
        return r;
    }
    static QString objectPath(const QDBusMessage& r)
    {
        const QVariant v = r.arguments().value(0);
        return v.value<QDBusObjectPath>().path();
    }
};

QtSystemdBus::QtSystemdBus() : d(std::make_unique<Impl>()) {}

QtSystemdBus::~QtSystemdBus()
{
    if (d->sink) d->bus.disconnect(QString(), QString(), QString(), QString(), d->sink.get(), nullptr);
}

void QtSystemdBus::setReceivers(Receivers r)
{
    d->receivers = std::move(r);
    if (d->sink) {
        d->bus.disconnect(QString::fromLatin1(kService), QString::fromLatin1(kPath), QString::fromLatin1(kManager),
                          QStringLiteral("JobRemoved"), d->sink.get(),
                          SLOT(onJobRemoved(uint, QDBusObjectPath, QString, QString)));
        d->bus.disconnect(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                          QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameOwnerChanged"), d->sink.get(),
                          SLOT(onNameOwnerChanged(QString, QString, QString)));
    }
    // A fresh sink on THIS thread: arrivals are delivered through this thread's event loop, which pump() runs.
    d->sink = std::make_unique<SystemdSignalSink>();
    const bool a = d->bus.connect(QString::fromLatin1(kService), QString::fromLatin1(kPath), QString::fromLatin1(kManager),
                                  QStringLiteral("JobRemoved"), d->sink.get(),
                                  SLOT(onJobRemoved(uint, QDBusObjectPath, QString, QString)));
    const bool b = d->bus.connect(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                                  QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameOwnerChanged"),
                                  QStringList{QString::fromLatin1(kService)}, QString(), d->sink.get(),
                                  SLOT(onNameOwnerChanged(QString, QString, QString)));
    if (!a || !b)
        throw BusError(QStringLiteral("org.freedesktop.DBus.Error.Failed"),
                       QStringLiteral("cannot subscribe to systemd's signals: %1").arg(d->bus.lastError().message()));
}

void QtSystemdBus::pump(Ms timeout)
{
    if (!d->sink) return;
    if (d->sink->queue.empty()) {
        QEventLoop loop;
        QTimer t;
        t.setSingleShot(true);
        QObject::connect(&t, &QTimer::timeout, &loop, &QEventLoop::quit);
        d->sink->waiting = &loop;
        t.start(toMs(timeout));
        loop.exec();
        d->sink->waiting = nullptr;
    }
    // Anything else already queued for this thread.
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    while (!d->sink->queue.empty()) {
        const Event e = d->sink->queue.front();
        d->sink->queue.pop_front();
        if (e.owner) {
            if (d->receivers.managerOwnerChanged) d->receivers.managerOwnerChanged();
        } else if (d->receivers.jobRemoved) {
            d->receivers.jobRemoved(e.id, e.job, e.unit, e.result);
        }
    }
}

void QtSystemdBus::subscribe(Ms t)
{
    try {
        d->call("Subscribe", {}, t);
    } catch (const BusError& e) {
        // Already subscribed on this connection is fine.
        if (e.name() != QLatin1String("org.freedesktop.systemd1.AlreadySubscribed")) throw;
    }
}

QString QtSystemdBus::loadUnit(const QString& name, Ms t)
{
    return Impl::objectPath(d->call("LoadUnit", {name}, t));
}

QVariant QtSystemdBus::unitProperty(const QString& unitPath, const QString& interface, const QString& property, Ms t)
{
    const QDBusMessage r = d->call("Get", {interface, property}, t, unitPath, "org.freedesktop.DBus.Properties");
    return r.arguments().value(0).value<QDBusVariant>().variant();
}

QList<SystemdUnitProcess> QtSystemdBus::unitProcesses(const QString& name, Ms t)
{
    const QDBusMessage r = d->call("GetUnitProcesses", {name}, t);
    QList<SystemdUnitProcess> out;
    const QDBusArgument arg = r.arguments().value(0).value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        SystemdUnitProcess p;
        uint pid = 0;
        arg.beginStructure();
        arg >> p.cgroup >> pid >> p.command;
        arg.endStructure();
        p.pid = quint32(pid);
        out << p;
    }
    arg.endArray();
    return out;
}

void QtSystemdBus::maskUnitFiles(const QStringList& files, bool runtime, bool force, Ms t)
{
    d->call("MaskUnitFiles", {files, runtime, force}, t);
}

void QtSystemdBus::unmaskUnitFiles(const QStringList& files, bool runtime, Ms t)
{
    d->call("UnmaskUnitFiles", {files, runtime}, t);
}

void QtSystemdBus::enableUnitFiles(const QStringList& files, bool runtime, bool force, Ms t)
{
    d->call("EnableUnitFiles", {files, runtime, force}, t);
}

void QtSystemdBus::disableUnitFiles(const QStringList& files, bool runtime, Ms t)
{
    d->call("DisableUnitFiles", {files, runtime}, t);
}

void QtSystemdBus::reload(Ms t)
{
    d->call("Reload", {}, t);
}

QString QtSystemdBus::startUnit(const QString& name, const QString& mode, Ms t)
{
    return Impl::objectPath(d->call("StartUnit", {name, mode}, t));
}

QString QtSystemdBus::stopUnit(const QString& name, const QString& mode, Ms t)
{
    return Impl::objectPath(d->call("StopUnit", {name, mode}, t));
}

QList<SystemdJob> QtSystemdBus::listJobs(Ms t)
{
    const QDBusMessage r = d->call("ListJobs", {}, t);
    QList<SystemdJob> out;
    const QDBusArgument arg = r.arguments().value(0).value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        SystemdJob j;
        uint id = 0;
        QDBusObjectPath job, unit;
        arg.beginStructure();
        arg >> id >> j.unit >> j.type >> j.state >> job >> unit;
        arg.endStructure();
        j.id = quint32(id);
        j.jobPath = job.path();
        j.unitPath = unit.path();
        out << j;
    }
    arg.endArray();
    return out;
}

void QtSystemdBus::cancelJob(quint32 id, Ms t)
{
    d->call("CancelJob", {QVariant::fromValue(uint(id))}, t);
}

}  // namespace asb::posix

#include "QtSystemdBus.moc"
