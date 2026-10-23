#include "engine/QtSerialDevice.h"

namespace asb::engine {

QString enumName(SerialError e)
{
    switch (e) {
    case SerialError::None: return QStringLiteral("none");
    case SerialError::NotFound: return QStringLiteral("not found");
    case SerialError::Permission: return QStringLiteral("permission denied");
    case SerialError::Busy: return QStringLiteral("busy");
    case SerialError::Resource: return QStringLiteral("resource error");
    case SerialError::Write: return QStringLiteral("write error");
    case SerialError::Read: return QStringLiteral("read error");
    case SerialError::Config: return QStringLiteral("configuration error");
    case SerialError::Timeout: return QStringLiteral("timeout");
    case SerialError::Other: return QStringLiteral("error");
    }
    return QStringLiteral("error");
}

std::optional<QSerialPort::Parity> toQt(asb::Parity p)
{
    switch (p) {
    case asb::Parity::None: return QSerialPort::NoParity;
    case asb::Parity::Odd: return QSerialPort::OddParity;
    case asb::Parity::Even: return QSerialPort::EvenParity;
    case asb::Parity::Mark: return QSerialPort::MarkParity;
    case asb::Parity::Space: return QSerialPort::SpaceParity;
    }
    return std::nullopt;
}

std::optional<QSerialPort::StopBits> toQt(asb::StopBits s)
{
    switch (s) {
    case asb::StopBits::One: return QSerialPort::OneStop;
    case asb::StopBits::Two: return QSerialPort::TwoStop;
    case asb::StopBits::OnePointFive: return QSerialPort::OneAndHalfStop;
    case asb::StopBits::None: return std::nullopt;   // no serial line has zero stop bits
    }
    return std::nullopt;
}

std::optional<QSerialPort::FlowControl> toQt(asb::Handshake h)
{
    switch (h) {
    case asb::Handshake::None: return QSerialPort::NoFlowControl;
    case asb::Handshake::XOnXOff: return QSerialPort::SoftwareControl;
    case asb::Handshake::RequestToSend: return QSerialPort::HardwareControl;
    case asb::Handshake::RequestToSendXOnXOff: return std::nullopt;   // refused by validate() (§4.2)
    }
    return std::nullopt;
}

std::optional<QSerialPort::DataBits> toQtDataBits(int bits)
{
    switch (bits) {
    case 5: return QSerialPort::Data5;
    case 6: return QSerialPort::Data6;
    case 7: return QSerialPort::Data7;
    case 8: return QSerialPort::Data8;
    default: return std::nullopt;
    }
}

SerialError toSerialError(QSerialPort::SerialPortError e)
{
    switch (e) {
    case QSerialPort::NoError: return SerialError::None;
    case QSerialPort::DeviceNotFoundError: return SerialError::NotFound;
    case QSerialPort::PermissionError: return SerialError::Permission;
    case QSerialPort::OpenError: return SerialError::Busy;
    case QSerialPort::WriteError: return SerialError::Write;
    case QSerialPort::ReadError: return SerialError::Read;
    case QSerialPort::ResourceError: return SerialError::Resource;
    case QSerialPort::UnsupportedOperationError: return SerialError::Config;
    case QSerialPort::TimeoutError: return SerialError::Timeout;
    case QSerialPort::NotOpenError:
    case QSerialPort::UnknownError: return SerialError::Other;
    }
    return SerialError::Other;
}

QtSerialDevice::QtSerialDevice(std::unique_ptr<ISerialBackend> backend, QObject* parent)
    : ISerialDevice(parent), m_backend(std::move(backend))
{
    // Forwarded directly; the engine connects to THESE signals with queued connections (INV-80).
    connect(m_backend.get(), &ISerialBackend::readyRead, this, &ISerialDevice::readyRead);
    connect(m_backend.get(), &ISerialBackend::bytesWritten, this, &ISerialDevice::bytesWritten);
    connect(m_backend.get(), &ISerialBackend::errorOccurred, this, [this](QSerialPort::SerialPortError e) {
        if (e != QSerialPort::NoError) emit errorOccurred(toSerialError(e), m_backend->errorString());
    });
}

QtSerialDevice::~QtSerialDevice()
{
    m_backend->disconnect(this);
    m_backend->close();
}

bool QtSerialDevice::fail(QString* why, const QString& what)
{
    if (why) *why = what;
    m_backend->close();
    m_backend->clearError();
    return false;
}

// DTR / RTS. A false return fails the open (D14), with one exception: a device that has no modem
// control lines at all (the driver refuses the ioctl: ENOTTY → UnsupportedOperationError, e.g. a
// pseudo-terminal) already satisfies a request to DEASSERT the line, so that one case is accepted.
// Asserting a line such a device does not have still fails the open.
bool QtSerialDevice::setModemLine(bool (ISerialBackend::*setter)(bool), bool on, const char* name, QString* why)
{
    if ((m_backend.get()->*setter)(on)) return true;
    if (!on && m_backend->error() == QSerialPort::UnsupportedOperationError) {
        m_backend->clearError();
        return true;
    }
    return fail(why, QStringLiteral("could not set %1 %2: %3")
                         .arg(QLatin1String(name), on ? QStringLiteral("on") : QStringLiteral("off"),
                              m_backend->errorString()));
}

bool QtSerialDevice::open(const asb::PortMapping& m, QString* why)
{
    if (m_backend->isOpen()) m_backend->close();
    m_backend->clearError();
    m_openNotice.clear();

    const auto parity = toQt(m.parity);
    const auto stopBits = toQt(m.stopBits);
    const auto flow = toQt(m.handshake);
    const auto dataBits = toQtDataBits(m.dataBits);
    if (!parity) return fail(why, QStringLiteral("unsupported parity"));
    if (!stopBits) return fail(why, QStringLiteral("unsupported stop bits"));
    if (!flow) return fail(why, QStringLiteral("unsupported handshake"));
    if (!dataBits) return fail(why, QStringLiteral("unsupported data bits %1").arg(m.dataBits));

    // 1. Every parameter, before the open (QSerialPort applies them at open).
    m_backend->setPortName(m.comPort);
    if (!m_backend->setBaudRate(m.baudRate))
        return fail(why, QStringLiteral("could not set baud rate %1: %2").arg(m.baudRate).arg(m_backend->errorString()));
    if (!m_backend->setDataBits(*dataBits))
        return fail(why, QStringLiteral("could not set data bits %1: %2").arg(m.dataBits).arg(m_backend->errorString()));
    if (!m_backend->setParity(*parity))
        return fail(why, QStringLiteral("could not set parity: %1").arg(m_backend->errorString()));
    if (!m_backend->setStopBits(*stopBits))
        return fail(why, QStringLiteral("could not set stop bits: %1").arg(m_backend->errorString()));
    if (!m_backend->setFlowControl(*flow))
        return fail(why, QStringLiteral("could not set handshake: %1").arg(m_backend->errorString()));

    // 2. Open.
    if (!m_backend->open(QIODevice::ReadWrite)) return fail(why, m_backend->errorString());

    // 3. Read every parameter back from the OS (tcgetattr / TCGETS2 / GetCommState on the open handle;
    //    QSerialPort's getters only return what its setters cached): a driver that silently kept another
    //    value fails the open, and so does a read-back the OS refuses (D14, HW 5). The rate may differ by
    //    up to ±2 % (a USB bridge reports the rate it programmed); that is noted, not refused.
    QString readErr;
    const auto native = m_backend->nativeLineSettings(&readErr);
    if (!native) return fail(why, QStringLiteral("could not read the port settings back: %1").arg(readErr));
    const LineSettings wanted{m.baudRate, *dataBits, *parity, *stopBits, *flow};
    if (const QString diff = lineSettingsMismatch(wanted, *native); !diff.isEmpty()) return fail(why, diff);
    const QString note = lineSettingsNote(wanted, *native);

    // 4. Modem lines. With hardware flow control the driver owns RTS.
    if (!setModemLine(&ISerialBackend::setDataTerminalReady, m.dtrEnable, "DTR", why)) return false;
    if (*flow != QSerialPort::HardwareControl
        && !setModemLine(&ISerialBackend::setRequestToSend, m.rtsEnable, "RTS", why))
        return false;

    // 5. Stale input from before this client is discarded, then the read cap (INV-27).
    if (!m_backend->clear(QSerialPort::Input))
        return fail(why, QStringLiteral("could not discard stale input: %1").arg(m_backend->errorString()));
    m_backend->setReadBufferSize(kCap);

    // 6. Nothing may be left on the port.
    if (m_backend->error() != QSerialPort::NoError) return fail(why, m_backend->errorString());
    m_openNotice = note;
    return true;
}

void QtSerialDevice::close()
{
    // D-WS4-1 (WS-7, INV-81): a tty or COM close can wait for a non-empty output queue (Linux closing_wait,
    // 30 s by default). When bytes are still queued — Qt's buffer or the driver's (TIOCOUTQ / cbOutQue) — the
    // end is a stalled, pre-empted or failed one (an orderly ClientGone end has already waited for both to
    // drain), so the output is discarded first with a non-blocking PurgeComm(PURGE_TXCLEAR) / tcflush(TCOFLUSH)
    // and close() cannot block on it. An empty queue is never flushed: on a pty the driver reports 0 while the
    // other end has not yet read the data, and a flush would discard it.
    if (m_backend->isOpen()) {
        const std::optional<qint64> native = m_backend->nativeOutputPending();
        if (m_backend->bytesToWrite() > 0 || native.value_or(0) > 0) m_backend->clear(QSerialPort::Output);
    }
    m_backend->close();
}

qint64 QtSerialDevice::read(char* data, qint64 max)
{
    if (!m_backend->isOpen()) return -1;
    return m_backend->read(data, max);
}

qint64 QtSerialDevice::write(const char* data, qint64 len)
{
    if (!m_backend->isOpen()) return -1;
    return m_backend->write(data, len);
}

qint64 QtSerialDevice::bytesAvailable() const { return m_backend->bytesAvailable(); }
qint64 QtSerialDevice::bytesToWrite() const { return m_backend->bytesToWrite(); }
std::optional<qint64> QtSerialDevice::nativeOutputPending() const { return m_backend->nativeOutputPending(); }
void QtSerialDevice::setReadBufferSize(qint64 size) { m_backend->setReadBufferSize(size); }

// ---------------------------------------------------------------------------------------------
// The production backend.
// ---------------------------------------------------------------------------------------------
namespace {

class QSerialPortBackend final : public ISerialBackend {
public:
    QSerialPortBackend()
    {
        connect(&m_port, &QSerialPort::readyRead, this, &ISerialBackend::readyRead);
        connect(&m_port, &QSerialPort::bytesWritten, this, &ISerialBackend::bytesWritten);
        connect(&m_port, &QSerialPort::errorOccurred, this, &ISerialBackend::errorOccurred);
    }

    void setPortName(const QString& n) override { m_port.setPortName(n); }
    bool setBaudRate(qint32 b) override { return m_port.setBaudRate(b); }
    bool setDataBits(QSerialPort::DataBits d) override { return m_port.setDataBits(d); }
    bool setParity(QSerialPort::Parity p) override { return m_port.setParity(p); }
    bool setStopBits(QSerialPort::StopBits s) override { return m_port.setStopBits(s); }
    bool setFlowControl(QSerialPort::FlowControl f) override { return m_port.setFlowControl(f); }
    std::optional<LineSettings> nativeLineSettings(QString* err) const override
    {
        return readNativeLineSettings(m_port.handle(), err);
    }

    bool open(QIODevice::OpenMode mode) override { return m_port.open(mode); }
    void close() override
    {
        if (m_port.isOpen()) m_port.close();
    }
    bool isOpen() const override { return m_port.isOpen(); }
    bool setDataTerminalReady(bool on) override { return m_port.setDataTerminalReady(on); }
    bool setRequestToSend(bool on) override { return m_port.setRequestToSend(on); }
    bool clear(QSerialPort::Directions d) override { return m_port.clear(d); }
    void setReadBufferSize(qint64 s) override { m_port.setReadBufferSize(s); }

    QSerialPort::SerialPortError error() const override { return m_port.error(); }
    QString errorString() const override { return m_port.errorString(); }
    void clearError() override { m_port.clearError(); }

    qint64 read(char* d, qint64 n) override { return m_port.read(d, n); }
    qint64 write(const char* d, qint64 n) override { return m_port.write(d, n); }
    qint64 bytesAvailable() const override { return m_port.bytesAvailable(); }
    qint64 bytesToWrite() const override { return m_port.bytesToWrite(); }
    std::optional<qint64> nativeOutputPending() const override
    {
        if (!m_port.isOpen()) return std::nullopt;
        return readNativeOutputPending(m_port.handle(), nullptr);
    }

private:
    QSerialPort m_port;
};

}  // namespace

std::unique_ptr<ISerialBackend> makeQSerialPortBackend() { return std::make_unique<QSerialPortBackend>(); }

std::unique_ptr<ISerialDevice> makeQtSerialDevice()
{
    return std::make_unique<QtSerialDevice>(makeQSerialPortBackend());
}

}  // namespace asb::engine
