#pragma once

// The serial end of a bridge (master §4.3, WS-4). The engine talks only to this interface:
// production uses QtSerialDevice (over QSerialPort, behind ISerialBackend), the engine tests use
// FakeSerialDevice. All methods are called on the engine (main) thread.

#include "core/BridgeConfig.h"

#include <QObject>
#include <QString>
#include <QtGlobal>

#include <functional>
#include <memory>
#include <optional>

namespace asb::engine {

// Per-direction buffering limits (INV-27). A pump reads at most kChunk bytes at a time, and only
// while the destination holds less than kCap unwritten bytes; every source read buffer (the socket's
// and the device's) is capped at kCap. So one direction holds at most
//   source buffer (kCap) + read chunk (kChunk) + pending suffix (kChunk) + destination buffer (kCap)
//   = 2·kCap + 2·kChunk = 136 KiB of user-space memory, 272 KiB per session (kernel buffers excluded).
// kChunk is the pump's 4096-byte buffer.
inline constexpr qint64 kChunk = 4 * 1024;
inline constexpr qint64 kCap = 64 * 1024;

enum class SerialError { None, NotFound, Permission, Busy, Resource, Write, Read, Config, Timeout, Other };
QString enumName(SerialError);

class ISerialDevice : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~ISerialDevice() override = default;

    // Applies every parameter of the mapping and opens the device. On any failure the device is left
    // closed and `why` says what failed. May be called again after a failure (open retries, INV-3).
    virtual bool open(const asb::PortMapping&, QString* why) = 0;
    // Closes the device; bytes not yet transmitted (Qt's buffer, and on some drivers the OS queue too)
    // are discarded. Idempotent.
    virtual void close() = 0;
    // Like QIODevice: the number of bytes read (0 = nothing available now) or -1 on an error.
    virtual qint64 read(char*, qint64) = 0;
    // Like QIODevice: the number of bytes accepted (possibly fewer than given) or -1 on an error.
    virtual qint64 write(const char*, qint64) = 0;
    virtual qint64 bytesAvailable() const = 0;
    // Qt's own write buffer: bytes not yet handed to the OS.
    virtual qint64 bytesToWrite() const = 0;
    // Bytes the OS driver or the adapter still holds for transmission, below bytesToWrite(): an empty Qt
    // buffer does not mean the bytes left the port. QtSerialDevice asks TIOCOUTQ (POSIX) or
    // ClearCommError's COMSTAT.cbOutQue (Windows); both return at once. The engine polls this from the
    // event loop and never waits for the queue (tcdrain and FlushFileBuffers block the calling thread,
    // INV-83). nullopt when the device cannot tell (closed, or the driver refuses the query): the engine
    // then has only bytesToWrite() to go by. Pure on purpose: a wrapper device must forward it.
    virtual std::optional<qint64> nativeOutputPending() const = 0;
    virtual void setReadBufferSize(qint64) = 0;
    // After a successful open: something about it worth one Info log line (e.g. "baud 250000
    // requested, device reports 249600"), or empty. The engine logs it; devices never log.
    virtual QString openNotice() const { return {}; }

signals:
    void readyRead();
    void bytesWritten(qint64);
    void errorOccurred(asb::engine::SerialError, const QString& message);
};

// Makes one device per session. Registered only by the service code paths (INV-1, WS-5/WS-9).
using SerialDeviceFactory = std::function<std::unique_ptr<ISerialDevice>()>;

}  // namespace asb::engine
