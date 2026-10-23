#include "engine/PortBridge.h"

#include "engine/Pump.h"

#include <QDateTime>
#include <QHostAddress>
#include <QMetaObject>

namespace asb::engine {

PortBridge::PortBridge(const asb::PortMapping& mapping, SerialDeviceFactory factory, asb::ISideChannel& side,
                       EngineTimings timings, QObject* parent)
    : QObject(parent), m_mapping(mapping), m_factory(std::move(factory)), m_side(side), m_timings(timings)
{
    m_status.name = mapping.displayName();
    m_status.comPort = mapping.comPort;
    m_status.bindAddress = mapping.bindAddress;
    m_status.tcpPort = mapping.tcpPort;
    m_status.state = asb::BridgeState::Stopped;
    m_status.since = QDateTime::currentDateTimeUtc();

    m_listenRetry.setSingleShot(true);
    connect(&m_listenRetry, &QTimer::timeout, this, &PortBridge::tryListen);
    connect(&m_server, &QTcpServer::newConnection, this, &PortBridge::onNewConnection);
}

PortBridge::~PortBridge()
{
    // Sessions are children of the bridge and go with it; nothing may call back into it any more.
    m_stopped = true;
    m_listenRetry.stop();
    m_server.close();
    if (m_current) {
        m_current->state = Session::Closed;
        m_current = nullptr;
    }
}

void PortBridge::log(QtMsgType type, const QString& line) { m_side.log(type, line); }

void PortBridge::setState(asb::BridgeState state, const std::optional<QString>& client)
{
    m_status.state = state;
    m_status.client = client;
    m_status.since = QDateTime::currentDateTimeUtc();
}

void PortBridge::start()
{
    if (m_stopped) return;
    tryListen();
}

void PortBridge::stop()
{
    if (m_stopped) return;
    m_stopped = true;
    m_listenRetry.stop();   // INV-5: stop cancels a listen retry
    m_server.close();
    if (m_current) endSession(m_current, EndReason::Shutdown);
    setState(asb::BridgeState::Stopped, std::nullopt);
}

void PortBridge::tryListen()
{
    if (m_stopped) return;
    ++m_listenAttempts;
    const QString where = QStringLiteral("%1:%2").arg(m_mapping.bindAddress).arg(m_mapping.tcpPort);
    // Bind through the validator's parser, never QHostAddress(bindAddress): "010.0.0.1" is 8.0.0.1 (WS-3
    // handoff).
    const auto ip = asb::parseIpAddress(m_mapping.bindAddress);
    QString err;
    if (!ip) {
        err = QStringLiteral("not an IP address");
    } else if (m_server.listen(QHostAddress(asb::toString(*ip)), quint16(m_mapping.tcpPort))) {
        // A listener-level lastError stays visible while Listening.
        setState(asb::BridgeState::Listening, std::nullopt);
        log(QtInfoMsg, QStringLiteral("%1: %2 listening on %3").arg(name(), m_mapping.comPort, where));
        return;
    } else {
        err = m_server.errorString();
    }
    m_status.lastError = QStringLiteral("Listener on %1 failed: %2").arg(where, err);
    setState(asb::BridgeState::Error, std::nullopt);
    log(QtCriticalMsg, QStringLiteral("%1: listener on %2 failed, retrying in %3s: %4")
                           .arg(name(), where, QString::number(double(m_timings.listenRetryDelay.count()) / 1000.0), err));
    m_listenRetry.start(m_timings.listenRetryDelay);   // isolated: other ports are untouched (INV-5)
}

void PortBridge::onNewConnection()
{
    while (m_server.hasPendingConnections()) {
        QTcpSocket* sock = m_server.nextPendingConnection();
        if (!sock) break;
        if (m_stopped) {
            sock->abort();
            sock->deleteLater();
            continue;
        }
        accept(sock);
    }
}

void PortBridge::accept(QTcpSocket* sock)
{
    const QString ep = endpointText(*sock);
    // The current client goes first: its device must be closed before the new open (INV-2).
    if (m_current) endSession(m_current, EndReason::Replaced, ep);

    // Socket options on every accepted socket (INV-28) and the source read cap (INV-27).
    sock->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    sock->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    sock->setReadBufferSize(kCap);

    auto* s = new Session(sock, m_factory ? m_factory() : nullptr, this);
    m_current = s;
    const QPointer<Session> sp(s);
    // A client that closes its side with bytes still on their way to the device is drained first: in
    // Active at once (Draining); in Opening the open retries continue and the drain starts when the
    // device opens. A close with nothing to deliver, or a socket error, ends the session (and with it
    // the open retries, INV-3).
    connect(sock, &QAbstractSocket::disconnected, this, [this, sp] { onSocketGone(sp, QString(), true); },
            Qt::QueuedConnection);
    connect(sock, &QAbstractSocket::errorOccurred, this,
            [this, sp](QAbstractSocket::SocketError e) {
                if (isLive(sp))
                    onSocketGone(sp, sp->socket()->errorString(), e == QAbstractSocket::RemoteHostClosedError);
            },
            Qt::QueuedConnection);
    // The open runs from the event loop, never inside the accept (INV-80).
    QTimer::singleShot(0, this, [this, sp] { attemptOpen(sp); });
}

void PortBridge::attemptOpen(const QPointer<Session>& sp)
{
    if (!isLive(sp)) return;   // replaced, disconnected or stopped: the retries are cancelled (INV-3)
    Session* const s = sp.data();
    ++s->openAttempts;
    QString why;
    bool opened = false;
    if (!s->device()) {
        why = QStringLiteral("no serial device in this mode");
    } else {
        opened = s->device()->open(m_mapping, &why);
    }
    if (!isLive(s)) return;
    if (opened) {
        activate(s);
        return;
    }
    // Windows can refuse to reopen a COM port for a moment after it closes, which is exactly what
    // happens when a new client replaces an old one.
    if (s->device() && s->openAttempts < kOpenAttempts) {
        QTimer::singleShot(m_timings.openRetryDelay, this, [this, sp] { attemptOpen(sp); });
        return;
    }
    endSession(s, EndReason::OpenRefused, why);
}

void PortBridge::activate(Session* s)
{
    s->state = Session::Active;
    m_status.lastError.reset();
    setState(asb::BridgeState::Connected, s->endpoint);
    // A client that already closed its side (while Opening) is not logged as connected: beginDrain
    // below logs that it closed its side and that what it sent is being delivered.
    if (!s->clientClosed)
        log(QtInfoMsg, QStringLiteral("%1: client %2 connected to %3").arg(name(), s->endpoint, m_mapping.comPort));

    ISerialDevice* dev = s->device();
    if (const QString notice = dev->openNotice(); !notice.isEmpty())
        log(QtInfoMsg, QStringLiteral("%1: %2: %3").arg(name(), m_mapping.comPort, notice));
    QTcpSocket* sock = s->socket();
    dev->setReadBufferSize(kCap);
    s->up = new Pump(*this, *s, Direction::Up, s->socketEnd(), s->deviceEnd(), m_timings.stallTimeout,
                     m_timings.nativeDrainPoll);
    s->down = new Pump(*this, *s, Direction::Down, s->deviceEnd(), s->socketEnd(), m_timings.stallTimeout,
                       m_timings.nativeDrainPoll);

    // Every pump entry is queued (INV-80): a signal emitted inside a write() or read() is delivered
    // after that call returned, from the event loop.
    connect(sock, &QIODevice::readyRead, s->up, &Pump::pump, Qt::QueuedConnection);
    connect(dev, &ISerialDevice::bytesWritten, s->up, &Pump::onBytesWritten, Qt::QueuedConnection);
    connect(dev, &ISerialDevice::readyRead, s->down, &Pump::pump, Qt::QueuedConnection);
    connect(sock, &QIODevice::bytesWritten, s->down, &Pump::onBytesWritten, Qt::QueuedConnection);
    const QPointer<Session> sp(s);
    connect(dev, &ISerialDevice::errorOccurred, this,
            [this, sp](SerialError e, const QString& message) { onDeviceError(sp, e, message); },
            Qt::QueuedConnection);

    // The client closed its side while the device was still opening, with bytes to deliver: deliver
    // them and end (the down direction stays stopped).
    if (s->clientClosed) {
        beginDrain(s);
        return;
    }
    // Bytes that arrived while Opening (either side) are drained now (INV-56).
    QMetaObject::invokeMethod(s->up, &Pump::pump, Qt::QueuedConnection);
    QMetaObject::invokeMethod(s->down, &Pump::pump, Qt::QueuedConnection);
}

void PortBridge::onSocketGone(const QPointer<Session>& sp, const QString& why, bool orderly)
{
    if (!isLive(sp)) return;
    Session* const s = sp.data();
    if (s->state == Session::Draining) return;   // already delivering the client's last bytes
    if (s->clientClosed) return;                  // already seen (Opening); the retries go on
    if (orderly && s->state == Session::Active && upstreamPending(s)) {
        beginDrain(s);
        return;
    }
    // Opening: the open retries (a replaced client's port can be refused for a moment, Windows) are
    // where a one-shot client's close usually lands. With bytes to deliver they continue; activate()
    // drains, a final OpenRefused ends the session as before, and replacement and stop() pre-empt.
    if (orderly && s->state == Session::Opening && upstreamPending(s)) {
        s->clientClosed = true;
        return;
    }
    endSession(s, EndReason::ClientGone, why);
}

// Client → serial bytes not yet transmitted. Qt's write buffer being empty is not enough: the OS driver
// or the adapter can still hold bytes, and closing the port can cancel them, so the native
// output queue counts too. In Opening the device is closed and reports nothing.
bool PortBridge::upstreamPending(const Session* s) const
{
    if (s->socket()->bytesAvailable() > 0 || (s->up && s->up->pendingBytes() > 0)
        || (s->device() && s->device()->bytesToWrite() > 0))
        return true;
    return nativePending(s) > 0;
}

qint64 PortBridge::nativePending(const Session* s) const
{
    // Only bytes this session handed to the device can be waited for. A client that sent nothing never
    // waits on the native queue: some drivers (virtual COM, Bluetooth SPP) misreport it, and a false
    // count would hold every orderly disconnect for the stall timeout and end it with an error.
    if (!s->upWrote) return 0;
    // nullopt: the device cannot report its queue; Qt's buffer is then the last thing the bridge sees.
    return s->device() ? s->device()->nativeOutputPending().value_or(0) : 0;
}

// The client closed its side while client → serial bytes were still on their way (the socket's read
// buffer, the up pump's pending suffix, the device's write buffer, the device's native output queue):
// deliver them, then end as ClientGone, so `printf 'PS0;' | nc -q0 host port`
// reaches the amp; closing the device here would discard them. No new deadline: the up direction's
// stall deadline (INV-57) bounds the drain, since every byte still pending (natively queued ones
// included, Pump::updateStall) is output pending for the up pump. Replacement and stop() end a
// Draining session like any other (it is still m_current).
void PortBridge::beginDrain(Session* s)
{
    s->state = Session::Draining;
    // The down direction stops: nothing more is read from the device, nothing is written to the client.
    QObject::disconnect(s->device(), &ISerialDevice::readyRead, s->down, nullptr);
    QObject::disconnect(s->socket(), &QIODevice::bytesWritten, s->down, nullptr);
    s->down->halt();
    log(QtInfoMsg, QStringLiteral("%1: client %2 closed its side; delivering what it sent to %3")
                       .arg(name(), s->endpoint, m_mapping.comPort));
    QMetaObject::invokeMethod(s->up, &Pump::pump, Qt::QueuedConnection);
}

void PortBridge::checkDrained(Session* s)
{
    if (!isLive(s) || s->state != Session::Draining) return;
    if (upstreamPending(s)) return;
    endSession(s, EndReason::ClientGone);
}

void PortBridge::onDeviceError(const QPointer<Session>& sp, SerialError e, const QString& message)
{
    if (!isLive(sp) || sp->state == Session::Opening) return;   // in Opening the open's result decides
    if (e == SerialError::None || e == SerialError::Timeout) return;
    endSession(sp.data(), EndReason::DeviceError, QStringLiteral("device error: %1").arg(message));
}

void PortBridge::addForwarded(Direction dir, qint64 k)
{
    if (k <= 0) return;
    if (dir == Direction::Up) {
        m_status.bytesToSerial += k;
    } else {
        m_status.bytesToClient += k;
    }
}

void PortBridge::endSession(Session* s, EndReason reason, const QString& detail)
{
    if (!s || s->state == Session::Closing || s->state == Session::Closed) return;   // INV-4
    s->state = Session::Closing;
    const bool wasCurrent = m_current == s;
    if (wasCurrent) m_current = nullptr;   // the owner is detached first (INV-80, B8)

    // No new deliveries; deliveries already queued are stopped by isLive.
    QTcpSocket* sock = s->socket();
    ISerialDevice* dev = s->device();
    QObject::disconnect(sock, nullptr, this, nullptr);
    if (dev) QObject::disconnect(dev, nullptr, this, nullptr);
    for (Pump* p : {s->up, s->down}) {
        if (!p) continue;
        QObject::disconnect(sock, nullptr, p, nullptr);
        if (dev) QObject::disconnect(dev, nullptr, p, nullptr);
        p->halt();
    }
    if (dev) dev->close();
    sock->abort();
    s->state = Session::Closed;

    const QString& ep = s->endpoint;
    switch (reason) {
    case EndReason::Replaced:
        log(QtInfoMsg, QStringLiteral("%1: new client %2, replacing client %3").arg(name(), detail, ep));
        break;
    case EndReason::Shutdown:
        log(QtInfoMsg, QStringLiteral("%1: bridge stopping, closing client %2").arg(name(), ep));
        break;
    case EndReason::ClientGone:
        log(QtInfoMsg, QStringLiteral("%1: client %2 disconnected").arg(name(), ep));
        break;
    case EndReason::OpenRefused:
        m_status.lastError = QStringLiteral("Could not open %1: %2").arg(m_mapping.comPort, detail);
        log(QtWarningMsg,
            QStringLiteral("%1: client %2 refused, could not open %3: %4").arg(name(), ep, m_mapping.comPort, detail));
        break;
    case EndReason::ReadFailed:
    case EndReason::WriteFailed:
    case EndReason::DeviceError:
    case EndReason::Stalled:
        m_status.lastError = detail;
        log(QtWarningMsg, QStringLiteral("%1: client %2 disconnected: %3").arg(name(), ep, detail));
        break;
    }
    // Back to Listening after a session or a refused client; the reason stays in lastError (INV-9,
    // INV-59). stop() sets Stopped right after a Shutdown.
    if (wasCurrent && !m_stopped) setState(asb::BridgeState::Listening, std::nullopt);
    s->deleteLater();
}

}  // namespace asb::engine
