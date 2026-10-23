#pragma once

// The thin seam under QtSerialDevice (master §4.3): exactly the QSerialPort calls QtSerialDevice
// makes, with QSerialPort's own types and return values. Production: QSerialPortBackend (one
// QSerialPort). Tests: FakeSerialBackend, so the real setter checks of QtSerialDevice::open are what
// INV-58 tests, not a stand-in.

#include "engine/NativeSerial.h"

#include <QObject>
#include <QSerialPort>
#include <QString>
#include <QtGlobal>

#include <memory>
#include <optional>

namespace asb::engine {

class ISerialBackend : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~ISerialBackend() override = default;

    virtual void setPortName(const QString&) = 0;
    virtual bool setBaudRate(qint32) = 0;
    virtual bool setDataBits(QSerialPort::DataBits) = 0;
    virtual bool setParity(QSerialPort::Parity) = 0;
    virtual bool setStopBits(QSerialPort::StopBits) = 0;
    virtual bool setFlowControl(QSerialPort::FlowControl) = 0;
    // What the OS reports for the open port (readNativeLineSettings on QSerialPort::handle()), not
    // QSerialPort's cached getters. nullopt with *err when it cannot be read.
    virtual std::optional<LineSettings> nativeLineSettings(QString* err) const = 0;

    virtual bool open(QIODevice::OpenMode) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual bool setDataTerminalReady(bool) = 0;
    virtual bool setRequestToSend(bool) = 0;
    virtual bool clear(QSerialPort::Directions) = 0;
    virtual void setReadBufferSize(qint64) = 0;

    virtual QSerialPort::SerialPortError error() const = 0;
    virtual QString errorString() const = 0;
    virtual void clearError() = 0;

    virtual qint64 read(char*, qint64) = 0;
    virtual qint64 write(const char*, qint64) = 0;
    virtual qint64 bytesAvailable() const = 0;
    virtual qint64 bytesToWrite() const = 0;
    // The OS output queue of the open port (readNativeOutputPending on QSerialPort::handle()); nullopt
    // when the port is closed or the OS refuses the query.
    virtual std::optional<qint64> nativeOutputPending() const = 0;

signals:
    void readyRead();
    void bytesWritten(qint64);
    void errorOccurred(QSerialPort::SerialPortError);
};

// The production backend: one QSerialPort.
std::unique_ptr<ISerialBackend> makeQSerialPortBackend();

}  // namespace asb::engine
