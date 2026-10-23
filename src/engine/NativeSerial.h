#pragma once

// What the operating system reports for an open serial handle (D14, HW 5): the read-back
// QtSerialDevice::open compares with what it asked for. QSerialPort's own getters (baudRate(),
// parity(), ...) return the values its setters cached, so they cannot show a driver that kept another
// value; this asks the OS: tcgetattr (and, on Linux, TCGETS2 for the rate) or GetCommState.

#include <QSerialPort>
#include <QString>
#include <QtGlobal>

#include <optional>

#if !defined(Q_OS_WIN)
struct termios;
#endif

namespace asb::engine {

// One field is nullopt when the device reports something QSerialPort never sets (e.g. both hardware
// and software flow control, or a data size it has no enum for): that never matches a request.
struct LineSettings {
    std::optional<qint32> baudRate;
    std::optional<QSerialPort::DataBits> dataBits;
    std::optional<QSerialPort::Parity> parity;
    std::optional<QSerialPort::StopBits> stopBits;
    std::optional<QSerialPort::FlowControl> flowControl;
};

// QSerialPort::handle()'s type (QSerialPort::Handle is private in Qt 6).
#if defined(Q_OS_WIN)
using NativeSerialHandle = void*;
#else
using NativeSerialHandle = int;
#endif

// The settings of an open serial handle (QSerialPort::handle()); nullopt with *err when the OS refuses.
std::optional<LineSettings> readNativeLineSettings(NativeSerialHandle, QString* err);

// The bytes the OS still holds for transmission on an open serial handle, without waiting for them:
// TIOCOUTQ on POSIX, ClearCommError's COMSTAT.cbOutQue on Windows (ClearCommError also clears the
// port's error flags; the engine only asks while a session is ending). nullopt with *err when the OS
// refuses the query. A Linux pty answers 0: what the slave writes goes straight to the master's input.
std::optional<qint64> readNativeOutputPending(NativeSerialHandle, QString* err);

// The read-back baud rate may differ from the request by up to ±2 % (inclusive), the usual UART
// framing tolerance: USB-serial bridges such as cp210x and pl2303 report the rate they actually
// programmed, so a non-standard request can come back slightly different, and such an open is accepted.
// Data bits, parity, stop bits and flow control must match exactly.
inline constexpr qint64 kBaudTolerancePercent = 2;
bool baudRateWithinTolerance(std::optional<qint32> wanted, std::optional<qint32> got);

// The first field where `got` differs from `wanted` beyond the tolerance ("baud rate 38400 not applied
// (device reports 9600)"), or an empty string when they match.
QString lineSettingsMismatch(const LineSettings& wanted, const LineSettings& got);

// For settings that match: "baud 250000 requested, device reports 249600" when the rate differs within
// the tolerance, otherwise an empty string. Logged once per open, at Info (PortBridge).
QString lineSettingsNote(const LineSettings& wanted, const LineSettings& got);

#if defined(Q_OS_WIN)
// The DCB fields the decoding reads, as plain values, so the decoding is testable without a COM port.
struct DcbFields {
    quint32 baudRate = 0;
    quint8 byteSize = 0;
    quint8 parity = 0;      // NOPARITY 0, ODDPARITY 1, EVENPARITY 2, MARKPARITY 3, SPACEPARITY 4
    quint8 stopBits = 0;    // ONESTOPBIT 0, ONE5STOPBITS 1, TWOSTOPBITS 2
    bool outxCtsFlow = false;
    bool outX = false;
    bool inX = false;
    quint32 rtsControl = 0; // RTS_CONTROL_HANDSHAKE is 2
};
LineSettings decodeDcb(const DcbFields&);
#else
// c_cflag / c_iflag as QSerialPort sets them; `rate` comes from the caller (TCGETS2 on Linux,
// cfgetospeed elsewhere).
LineSettings decodeTermios(const struct termios&, std::optional<qint32> rate);
// A Bnnn speed constant → its rate (nullopt for B0 and unknown values).
std::optional<qint32> rateFromSpeed(unsigned long speed);
#endif

}  // namespace asb::engine
