#include "engine/NativeSerial.h"

#include <cstring>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <termios.h>

#include <cerrno>
#endif

namespace asb::engine {

#if defined(Q_OS_LINUX)
namespace detail {
// NativeSerial_linux.cpp: the output rate from TCGETS2 (struct termios2 cannot share a translation
// unit with <termios.h>).
bool linuxOutputRate(int fd, qint32* rate, int* error);
}  // namespace detail
#endif

bool baudRateWithinTolerance(std::optional<qint32> wanted, std::optional<qint32> got)
{
    if (!wanted || !got || *wanted <= 0) return wanted == got;
    const qint64 w = *wanted;
    const qint64 diff = qAbs(qint64(*got) - w);
    return diff * 100 <= w * kBaudTolerancePercent;   // inclusive: 38400 accepts 37632..39168
}

QString lineSettingsMismatch(const LineSettings& want, const LineSettings& got)
{
    if (!baudRateWithinTolerance(want.baudRate, got.baudRate))
        return QStringLiteral("baud rate %1 not applied (device reports %2)")
            .arg(want.baudRate.value_or(0))
            .arg(got.baudRate ? QString::number(*got.baudRate) : QStringLiteral("an unknown rate"));
    if (want.dataBits != got.dataBits)
        return QStringLiteral("data bits %1 not applied").arg(int(want.dataBits.value_or(QSerialPort::Data8)));
    if (want.parity != got.parity) return QStringLiteral("parity not applied");
    if (want.stopBits != got.stopBits) return QStringLiteral("stop bits not applied");
    if (want.flowControl != got.flowControl) return QStringLiteral("handshake not applied");
    return {};
}

QString lineSettingsNote(const LineSettings& want, const LineSettings& got)
{
    if (!want.baudRate || !got.baudRate || *want.baudRate == *got.baudRate) return {};
    if (!baudRateWithinTolerance(want.baudRate, got.baudRate)) return {};
    return QStringLiteral("baud %1 requested, device reports %2").arg(*want.baudRate).arg(*got.baudRate);
}

#if defined(Q_OS_WIN)

LineSettings decodeDcb(const DcbFields& d)
{
    LineSettings s;
    s.baudRate = qint32(d.baudRate);
    switch (d.byteSize) {
    case 5: s.dataBits = QSerialPort::Data5; break;
    case 6: s.dataBits = QSerialPort::Data6; break;
    case 7: s.dataBits = QSerialPort::Data7; break;
    case 8: s.dataBits = QSerialPort::Data8; break;
    default: break;
    }
    switch (d.parity) {
    case 0: s.parity = QSerialPort::NoParity; break;
    case 1: s.parity = QSerialPort::OddParity; break;
    case 2: s.parity = QSerialPort::EvenParity; break;
    case 3: s.parity = QSerialPort::MarkParity; break;
    case 4: s.parity = QSerialPort::SpaceParity; break;
    default: break;
    }
    switch (d.stopBits) {
    case 0: s.stopBits = QSerialPort::OneStop; break;
    case 1: s.stopBits = QSerialPort::OneAndHalfStop; break;
    case 2: s.stopBits = QSerialPort::TwoStop; break;
    default: break;
    }
    const bool rtsHandshake = d.rtsControl == 2;   // RTS_CONTROL_HANDSHAKE
    const bool hardware = d.outxCtsFlow || rtsHandshake;
    const bool software = d.outX || d.inX;
    if (!hardware && !software) {
        s.flowControl = QSerialPort::NoFlowControl;
    } else if (d.outxCtsFlow && rtsHandshake && !software) {
        s.flowControl = QSerialPort::HardwareControl;
    } else if (d.outX && d.inX && !hardware) {
        s.flowControl = QSerialPort::SoftwareControl;
    }
    return s;
}

std::optional<LineSettings> readNativeLineSettings(NativeSerialHandle handle, QString* err)
{
    DCB dcb;
    std::memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!::GetCommState(HANDLE(handle), &dcb)) {
        if (err) *err = QStringLiteral("GetCommState failed (error %1)").arg(::GetLastError());
        return std::nullopt;
    }
    DcbFields f;
    f.baudRate = dcb.BaudRate;
    f.byteSize = dcb.ByteSize;
    f.parity = dcb.Parity;
    f.stopBits = dcb.StopBits;
    f.outxCtsFlow = dcb.fOutxCtsFlow != 0;
    f.outX = dcb.fOutX != 0;
    f.inX = dcb.fInX != 0;
    f.rtsControl = dcb.fRtsControl;
    return decodeDcb(f);
}

std::optional<qint64> readNativeOutputPending(NativeSerialHandle handle, QString* err)
{
    DWORD errors = 0;
    COMSTAT stat;
    std::memset(&stat, 0, sizeof stat);
    if (!::ClearCommError(HANDLE(handle), &errors, &stat)) {
        if (err) *err = QStringLiteral("ClearCommError failed (error %1)").arg(::GetLastError());
        return std::nullopt;
    }
    return qint64(stat.cbOutQue);
}

#else

std::optional<qint32> rateFromSpeed(unsigned long speed)
{
    struct Pair {
        unsigned long speed;
        qint32 rate;
    };
    static const Pair table[] = {
        {B50, 50}, {B75, 75}, {B110, 110}, {B134, 134}, {B150, 150}, {B200, 200}, {B300, 300},
        {B600, 600}, {B1200, 1200}, {B1800, 1800}, {B2400, 2400}, {B4800, 4800}, {B9600, 9600},
        {B19200, 19200}, {B38400, 38400},
#ifdef B57600
        {B57600, 57600},
#endif
#ifdef B115200
        {B115200, 115200},
#endif
#ifdef B230400
        {B230400, 230400},
#endif
#ifdef B460800
        {B460800, 460800},
#endif
#ifdef B500000
        {B500000, 500000},
#endif
#ifdef B576000
        {B576000, 576000},
#endif
#ifdef B921600
        {B921600, 921600},
#endif
#ifdef B1000000
        {B1000000, 1000000},
#endif
#ifdef B1152000
        {B1152000, 1152000},
#endif
#ifdef B1500000
        {B1500000, 1500000},
#endif
#ifdef B2000000
        {B2000000, 2000000},
#endif
#ifdef B2500000
        {B2500000, 2500000},
#endif
#ifdef B3000000
        {B3000000, 3000000},
#endif
#ifdef B3500000
        {B3500000, 3500000},
#endif
#ifdef B4000000
        {B4000000, 4000000},
#endif
    };
    for (const Pair& p : table)
        if (p.speed == speed) return p.rate;
    return std::nullopt;
}

LineSettings decodeTermios(const struct termios& t, std::optional<qint32> rate)
{
    LineSettings s;
    s.baudRate = rate;
    switch (t.c_cflag & CSIZE) {
    case CS5: s.dataBits = QSerialPort::Data5; break;
    case CS6: s.dataBits = QSerialPort::Data6; break;
    case CS7: s.dataBits = QSerialPort::Data7; break;
    case CS8: s.dataBits = QSerialPort::Data8; break;
    default: break;
    }
    if (!(t.c_cflag & PARENB)) {
        s.parity = QSerialPort::NoParity;
#ifdef CMSPAR
    } else if (t.c_cflag & CMSPAR) {
        s.parity = (t.c_cflag & PARODD) ? QSerialPort::MarkParity : QSerialPort::SpaceParity;
#endif
    } else {
        s.parity = (t.c_cflag & PARODD) ? QSerialPort::OddParity : QSerialPort::EvenParity;
    }
    s.stopBits = (t.c_cflag & CSTOPB) ? QSerialPort::TwoStop : QSerialPort::OneStop;
#ifdef CRTSCTS
    const bool hardware = (t.c_cflag & CRTSCTS) != 0;
#else
    const bool hardware = false;
#endif
    const bool ixon = (t.c_iflag & IXON) != 0;
    const bool ixoff = (t.c_iflag & IXOFF) != 0;
    if (!hardware && !ixon && !ixoff) {
        s.flowControl = QSerialPort::NoFlowControl;
    } else if (hardware && !ixon && !ixoff) {
        s.flowControl = QSerialPort::HardwareControl;
    } else if (!hardware && ixon && ixoff) {
        s.flowControl = QSerialPort::SoftwareControl;
    }
    return s;
}

std::optional<LineSettings> readNativeLineSettings(NativeSerialHandle fd, QString* err)
{
    struct termios t;
    std::memset(&t, 0, sizeof t);
    if (::tcgetattr(fd, &t) != 0) {
        if (err) *err = QStringLiteral("tcgetattr failed: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        return std::nullopt;
    }
    std::optional<qint32> rate;
#if defined(Q_OS_LINUX)
    // The kernel keeps the actual output rate in termios2 for standard and custom (BOTHER) rates alike.
    qint32 r = 0;
    int e = 0;
    if (!detail::linuxOutputRate(fd, &r, &e)) {
        if (err) *err = QStringLiteral("TCGETS2 failed: %1").arg(QString::fromLocal8Bit(std::strerror(e)));
        return std::nullopt;
    }
    rate = r;
#else
    rate = rateFromSpeed(static_cast<unsigned long>(::cfgetospeed(&t)));
#endif
    return decodeTermios(t, rate);
}

std::optional<qint64> readNativeOutputPending(NativeSerialHandle fd, QString* err)
{
#if defined(TIOCOUTQ)
    int queued = 0;
    if (::ioctl(fd, TIOCOUTQ, &queued) != 0) {
        if (err) *err = QStringLiteral("TIOCOUTQ failed: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        return std::nullopt;
    }
    return qint64(queued);
#else
    Q_UNUSED(fd);
    if (err) *err = QStringLiteral("TIOCOUTQ is not available on this system");
    return std::nullopt;
#endif
}

#endif

}  // namespace asb::engine
