#pragma once

// A scriptable ISerialBackend for tst_QtSerialDevice (WS-4, INV-58): below QtSerialDevice, so the real
// open sequence and its setter checks run. Every call is logged ("setBaudRate 38400", "open",
// "setDataTerminalReady 0", ...). A call named in `fail` returns false and leaves that error on the
// port, as QSerialPort does.

#include "engine/ISerialBackend.h"

#include <QHash>
#include <QStringList>

#include <optional>

namespace asb::test {

class FakeSerialBackend final : public engine::ISerialBackend {
    Q_OBJECT

public:
    mutable QStringList log;
    QHash<QString, QSerialPort::SerialPortError> fail;   // call name → the error it leaves
    std::optional<qint32> reportedBaudRate;              // native read-back override (a driver that ignored it)
    std::optional<QString> nativeReadError;              // the native read-back fails with this text
    std::optional<QSerialPort::SerialPortError> errorAfterOpen;   // left on the port by open()
    std::optional<qint64> nativeQueue = 0;               // the open port's output queue (nullopt: cannot tell)

    void setPortName(const QString& n) override { log << QStringLiteral("setPortName %1").arg(n); }
    bool setBaudRate(qint32 b) override { return set(QStringLiteral("setBaudRate"), QString::number(b), [&] { m_baud = b; }); }
    bool setDataBits(QSerialPort::DataBits d) override { return set(QStringLiteral("setDataBits"), QString::number(int(d)), [&] { m_dataBits = d; }); }
    bool setParity(QSerialPort::Parity p) override { return set(QStringLiteral("setParity"), QString::number(int(p)), [&] { m_parity = p; }); }
    bool setStopBits(QSerialPort::StopBits s) override { return set(QStringLiteral("setStopBits"), QString::number(int(s)), [&] { m_stop = s; }); }
    bool setFlowControl(QSerialPort::FlowControl f) override { return set(QStringLiteral("setFlowControl"), QString::number(int(f)), [&] { m_flow = f; }); }
    // A driver that applied everything it was given (reportedBaudRate aside).
    std::optional<engine::LineSettings> nativeLineSettings(QString* err) const override
    {
        log << QStringLiteral("nativeLineSettings");
        if (nativeReadError) {
            if (err) *err = *nativeReadError;
            return std::nullopt;
        }
        return engine::LineSettings{reportedBaudRate.value_or(m_baud), m_dataBits, m_parity, m_stop, m_flow};
    }

    bool open(QIODevice::OpenMode) override
    {
        if (!set(QStringLiteral("open"), QString(), [&] { m_open = true; })) return false;
        if (errorAfterOpen) m_error = *errorAfterOpen;
        return true;
    }
    void close() override
    {
        log << QStringLiteral("close");
        m_open = false;
    }
    bool isOpen() const override { return m_open; }
    bool setDataTerminalReady(bool on) override { return set(QStringLiteral("setDataTerminalReady"), QString::number(int(on)), [] {}); }
    bool setRequestToSend(bool on) override { return set(QStringLiteral("setRequestToSend"), QString::number(int(on)), [] {}); }
    bool clear(QSerialPort::Directions d) override { return set(QStringLiteral("clear"), QString::number(int(d)), [] {}); }
    void setReadBufferSize(qint64 s) override { log << QStringLiteral("setReadBufferSize %1").arg(s); }

    QSerialPort::SerialPortError error() const override { return m_error; }
    QString errorString() const override { return m_error == QSerialPort::NoError ? QStringLiteral("no error") : QStringLiteral("injected error %1").arg(int(m_error)); }
    void clearError() override { m_error = QSerialPort::NoError; }

    qint64 read(char*, qint64) override { return 0; }
    qint64 write(const char*, qint64 n) override { return n; }
    qint64 bytesAvailable() const override { return 0; }
    qint64 bytesToWrite() const override { return 0; }
    std::optional<qint64> nativeOutputPending() const override
    {
        if (!m_open) return std::nullopt;
        return nativeQueue;
    }

    void emitError(QSerialPort::SerialPortError e)
    {
        m_error = e;
        emit errorOccurred(e);
    }

    // The calls whose name starts with `prefix`, in order.
    QStringList calls(const QString& prefix) const
    {
        QStringList out;
        for (const QString& l : log)
            if (l.startsWith(prefix)) out << l;
        return out;
    }

private:
    template <typename F>
    bool set(const QString& name, const QString& arg, F apply)
    {
        log << (arg.isEmpty() ? name : name + QLatin1Char(' ') + arg);
        if (const auto it = fail.constFind(name); it != fail.constEnd()) {
            m_error = *it;
            return false;
        }
        apply();
        return true;
    }

    bool m_open = false;
    qint32 m_baud = 9600;
    QSerialPort::DataBits m_dataBits = QSerialPort::Data8;
    QSerialPort::Parity m_parity = QSerialPort::NoParity;
    QSerialPort::StopBits m_stop = QSerialPort::OneStop;
    QSerialPort::FlowControl m_flow = QSerialPort::NoFlowControl;
    QSerialPort::SerialPortError m_error = QSerialPort::NoError;
};

}  // namespace asb::test
