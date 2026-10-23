#pragma once

// One client session of a PortBridge (master §4.3): one accepted socket, one serial device, two
// pumps. States Opening → Active → [Draining →] Closing → Closed. Draining: the client closed its side
// while client → serial bytes were still on their way; only the up pump runs until they reach the
// device (`printf 'PS0;' | nc -q0 host port` reaches the amp). A client that closes that way
// while the session is still Opening sets clientClosed: the open retries go on, and the session goes
// straight from Opening to Draining when the device opens. A Session never acts on its own: every
// callback is a PortBridge or Pump slot guarded by PortBridge::isLive (INV-80).

#include "engine/ISerialDevice.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTcpSocket>

#include <memory>

namespace asb::engine {

class Pump;

// Why a session ended. Each has its own log line; the failures also set PortStatus::lastError.
enum class EndReason { Replaced, Shutdown, ClientGone, OpenRefused, ReadFailed, WriteFailed, DeviceError, Stalled };
QString enumName(EndReason);

// The two directions: up = client → serial (bytesToSerial), down = serial → client (bytesToClient).
enum class Direction { Up, Down };

// One end of a pump: the same three calls on a socket and on a serial device.
class ByteEnd {
public:
    virtual ~ByteEnd() = default;
    virtual qint64 read(char*, qint64) = 0;
    virtual qint64 write(const char*, qint64) = 0;
    virtual qint64 bytesToWrite() const = 0;
    virtual QString name() const = 0;          // "client" / "serial", for messages
    virtual QString lastErrorText() const = 0; // why the last read/write returned -1
};

// The client end. A client that closed its side is not a read or write error here: once the peer's
// FIN (or reset) has been seen, QAbstractSocket returns -1 from read() as soon as its buffer is empty
// and from every write(). Reading then gives what is still buffered and then 0 (end of input), and a
// write accepts nothing; the socket's own disconnected / errorOccurred delivery, queued behind,
// decides what happens to the session (drain or end).
class SocketEnd final : public ByteEnd {
public:
    explicit SocketEnd(QTcpSocket* s) : m_socket(s) {}
    qint64 read(char* d, qint64 n) override
    {
        if (m_socket->state() != QAbstractSocket::ConnectedState && m_socket->bytesAvailable() == 0) return 0;
        return m_socket->read(d, n);
    }
    qint64 write(const char* d, qint64 n) override
    {
        if (m_socket->state() != QAbstractSocket::ConnectedState) return 0;
        return m_socket->write(d, n);
    }
    qint64 bytesToWrite() const override { return m_socket->bytesToWrite(); }
    QString name() const override { return QStringLiteral("client"); }
    QString lastErrorText() const override { return m_socket->errorString(); }

private:
    QTcpSocket* m_socket;
};

class DeviceEnd final : public ByteEnd {
public:
    explicit DeviceEnd(ISerialDevice* d) : m_device(d) {}
    qint64 read(char* d, qint64 n) override { return m_device->read(d, n); }
    qint64 write(const char* d, qint64 n) override { return m_device->write(d, n); }
    qint64 bytesToWrite() const override { return m_device->bytesToWrite(); }
    QString name() const override { return QStringLiteral("serial"); }
    QString lastErrorText() const override { return QStringLiteral("the device returned an error"); }

private:
    ISerialDevice* m_device;
};

class Session final : public QObject {
    Q_OBJECT

public:
    enum State { Opening, Active, Draining, Closing, Closed };

    // Takes the socket (re-parented to the session) and the device.
    Session(QTcpSocket* socket, std::unique_ptr<ISerialDevice> device, QObject* parent = nullptr);
    ~Session() override;

    State state = Opening;
    int openAttempts = 0;
    bool clientClosed = false;   // Opening: the client closed its side with bytes to deliver
    bool upWrote = false;        // the up pump handed the device bytes this session (the native queue is then ours)
    const QString endpoint;   // the client's address:port, fixed at accept

    QTcpSocket* socket() const { return m_socket; }
    ISerialDevice* device() const { return m_device.get(); }
    SocketEnd& socketEnd() { return m_socketEnd; }
    DeviceEnd& deviceEnd() { return m_deviceEnd; }

    // Created when the session becomes Active.
    Pump* up = nullptr;     // socket → device
    Pump* down = nullptr;   // device → socket

private:
    QTcpSocket* m_socket;
    std::unique_ptr<ISerialDevice> m_device;
    SocketEnd m_socketEnd;
    DeviceEnd m_deviceEnd;
};

// "a.b.c.d:port" / "[v6]:port".
QString endpointText(const QTcpSocket&);

}  // namespace asb::engine
