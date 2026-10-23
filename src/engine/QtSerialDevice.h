#pragma once

// ISerialDevice over ISerialBackend (master §4.3, D14, INV-58). open() sets every parameter, opens,
// reads every parameter back from the OS (not QSerialPort's cached getters) and compares, sets DTR and
// (unless hardware flow control) RTS, discards stale input and caps the read buffer; any setter that
// returns false, any read-back mismatch or failure and any error left on the port closes it again and
// fails the open.

#include "engine/ISerialBackend.h"
#include "engine/ISerialDevice.h"

#include <QSerialPort>

#include <memory>
#include <optional>

namespace asb::engine {

// The config enums mapped to QSerialPort's (nullopt = no QSerialPort equivalent: the open fails).
std::optional<QSerialPort::Parity> toQt(asb::Parity);
std::optional<QSerialPort::StopBits> toQt(asb::StopBits);
std::optional<QSerialPort::FlowControl> toQt(asb::Handshake);
std::optional<QSerialPort::DataBits> toQtDataBits(int);
SerialError toSerialError(QSerialPort::SerialPortError);

class QtSerialDevice final : public ISerialDevice {
    Q_OBJECT

public:
    explicit QtSerialDevice(std::unique_ptr<ISerialBackend> backend, QObject* parent = nullptr);
    ~QtSerialDevice() override;

    bool open(const asb::PortMapping&, QString* why) override;
    void close() override;
    qint64 read(char*, qint64) override;
    qint64 write(const char*, qint64) override;
    qint64 bytesAvailable() const override;
    qint64 bytesToWrite() const override;
    std::optional<qint64> nativeOutputPending() const override;
    void setReadBufferSize(qint64) override;
    QString openNotice() const override { return m_openNotice; }

    ISerialBackend& backend() { return *m_backend; }

private:
    bool fail(QString* why, const QString& what);
    bool setModemLine(bool (ISerialBackend::*setter)(bool), bool on, const char* name, QString* why);

    std::unique_ptr<ISerialBackend> m_backend;
    QString m_openNotice;
};

// The production factory: a QtSerialDevice over a QSerialPort.
std::unique_ptr<ISerialDevice> makeQtSerialDevice();

}  // namespace asb::engine
