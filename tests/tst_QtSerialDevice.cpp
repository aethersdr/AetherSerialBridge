// QtSerialDevice over FakeSerialBackend (WS-4, master §4.3, D14, INV-58): the real open sequence and
// its setter checks; the config → QSerialPort mapping (WS-3 handoff).

#include "engine/NativeSerial.h"
#include "engine/QtSerialDevice.h"
#include "support/FakeSerialBackend.h"

#include <QSignalSpy>
#include <QTest>

#if !defined(Q_OS_WIN)
#include <termios.h>
#endif

using namespace asb;
using namespace asb::engine;
using asb::test::FakeSerialBackend;

namespace {

PortMapping mapping()
{
    PortMapping m;
    m.name = QStringLiteral("KAT500");
    m.comPort = QStringLiteral("COM7");
    m.baudRate = 38400;
    m.tcpPort = 4532;
    m.bindAddress = QStringLiteral("0.0.0.0");
    return m;
}

struct Dut {
    FakeSerialBackend* backend = new FakeSerialBackend;
    QtSerialDevice device{std::unique_ptr<ISerialBackend>(backend)};
};

}  // namespace

class tst_QtSerialDevice : public QObject {
    Q_OBJECT

private slots:
    // D-WS4-1 (WS-7, INV-81 stop_with_blocked_device_close): a close with bytes still queued in the driver
    // discards them first, without waiting, so it never blocks on bytes nobody will receive; an empty queue is
    // never flushed (a pty reports 0 while its other end has not read yet); a closed device is left alone.
    void closeDiscardsQueuedOutputFirst()
    {
        Dut d;
        QString why;
        QVERIFY2(d.device.open(mapping(), &why), qPrintable(why));
        d.backend->nativeQueue = 7;
        d.backend->log.clear();
        d.device.close();
        QCOMPARE(d.backend->log, (QStringList{QStringLiteral("clear %1").arg(int(QSerialPort::Output)), QStringLiteral("close")}));
        d.backend->log.clear();
        d.device.close();
        QCOMPARE(d.backend->log, QStringList{QStringLiteral("close")});
        QVERIFY2(d.device.open(mapping(), &why), qPrintable(why));
        d.backend->nativeQueue = 0;
        d.backend->log.clear();
        d.device.close();
        QCOMPARE(d.backend->log, QStringList{QStringLiteral("close")});
    }

    void openSetsEveryParameterBeforeOpeningThenTheLines()
    {
        Dut d;
        QString why;
        QVERIFY2(d.device.open(mapping(), &why), qPrintable(why));
        const QStringList expected = {
            QStringLiteral("setPortName COM7"),
            QStringLiteral("setBaudRate 38400"),
            QStringLiteral("setDataBits 8"),
            QStringLiteral("setParity %1").arg(int(QSerialPort::NoParity)),
            QStringLiteral("setStopBits %1").arg(int(QSerialPort::OneStop)),
            QStringLiteral("setFlowControl %1").arg(int(QSerialPort::NoFlowControl)),
            QStringLiteral("open"),
            QStringLiteral("nativeLineSettings"),
            QStringLiteral("setDataTerminalReady 0"),
            QStringLiteral("setRequestToSend 0"),
            QStringLiteral("clear %1").arg(int(QSerialPort::Input)),
            QStringLiteral("setReadBufferSize %1").arg(kCap),
        };
        QCOMPARE(d.backend->log, expected);
    }

    void linesFollowTheConfig()
    {
        Dut d;
        PortMapping m = mapping();
        m.dtrEnable = true;
        m.rtsEnable = true;
        QVERIFY(d.device.open(m, nullptr));
        QCOMPARE(d.backend->calls(QStringLiteral("setDataTerminalReady")), QStringList{QStringLiteral("setDataTerminalReady 1")});
        QCOMPARE(d.backend->calls(QStringLiteral("setRequestToSend")), QStringList{QStringLiteral("setRequestToSend 1")});
    }

    void rtsNotTouchedUnderHardwareFlow()
    {
        Dut d;
        PortMapping m = mapping();
        m.handshake = Handshake::RequestToSend;
        m.rtsEnable = true;
        QVERIFY(d.device.open(m, nullptr));
        QVERIFY(d.backend->calls(QStringLiteral("setRequestToSend")).isEmpty());
        QCOMPARE(d.backend->calls(QStringLiteral("setFlowControl")),
                 QStringList{QStringLiteral("setFlowControl %1").arg(int(QSerialPort::HardwareControl))});
    }

    // INV-58: the DTR setter result is not ignored.
    void dtrFailureFailsOpen()
    {
        Dut d;
        d.backend->fail[QStringLiteral("setDataTerminalReady")] = QSerialPort::PermissionError;
        QString why;
        QVERIFY(!d.device.open(mapping(), &why));
        QVERIFY2(why.startsWith(QStringLiteral("could not set DTR off")), qPrintable(why));
        QCOMPARE(d.backend->log.last(), QStringLiteral("close"));
        QVERIFY(!d.backend->isOpen());
        QVERIFY(d.backend->calls(QStringLiteral("clear")).isEmpty());
    }

    void rtsFailureFailsOpen()
    {
        Dut d;
        d.backend->fail[QStringLiteral("setRequestToSend")] = QSerialPort::UnknownError;
        QString why;
        QVERIFY(!d.device.open(mapping(), &why));
        QVERIFY2(why.startsWith(QStringLiteral("could not set RTS off")), qPrintable(why));
        QVERIFY(!d.backend->isOpen());
    }

    // A device without modem lines (ENOTTY → UnsupportedOperationError, e.g. a pty) satisfies a request
    // to DEASSERT them; asserting one it does not have still fails the open (vetoable call, WS-4).
    void deassertOnDeviceWithoutModemLinesAccepted()
    {
        Dut d;
        d.backend->fail[QStringLiteral("setDataTerminalReady")] = QSerialPort::UnsupportedOperationError;
        d.backend->fail[QStringLiteral("setRequestToSend")] = QSerialPort::UnsupportedOperationError;
        QString why;
        QVERIFY2(d.device.open(mapping(), &why), qPrintable(why));
        QCOMPARE(d.backend->error(), QSerialPort::NoError);
    }

    void assertOnDeviceWithoutModemLinesFails()
    {
        Dut d;
        d.backend->fail[QStringLiteral("setDataTerminalReady")] = QSerialPort::UnsupportedOperationError;
        PortMapping m = mapping();
        m.dtrEnable = true;
        QString why;
        QVERIFY(!d.device.open(m, &why));
        QVERIFY2(why.startsWith(QStringLiteral("could not set DTR on")), qPrintable(why));
    }

    void baudErrorFailsOpen()
    {
        Dut d;
        d.backend->fail[QStringLiteral("setBaudRate")] = QSerialPort::UnsupportedOperationError;
        QString why;
        QVERIFY(!d.device.open(mapping(), &why));
        QVERIFY2(why.startsWith(QStringLiteral("could not set baud rate 38400")), qPrintable(why));
        QVERIFY(d.backend->calls(QStringLiteral("open")).isEmpty());
    }

    void baudReadBackMismatchFailsOpen()
    {
        Dut d;
        d.backend->reportedBaudRate = 9600;
        QString why;
        QVERIFY(!d.device.open(mapping(), &why));
        QCOMPARE(why, QStringLiteral("baud rate 38400 not applied (device reports 9600)"));
        QVERIFY(!d.backend->isOpen());
    }

    // A USB bridge reports the rate it programmed: within ±2 % the open succeeds and says so once
    // (openNotice, logged by the engine); an exact read-back says nothing.
    void baudReadBackWithinToleranceOpensWithANotice()
    {
        Dut d;
        d.backend->reportedBaudRate = 38600;
        QString why;
        QVERIFY2(d.device.open(mapping(), &why), qPrintable(why));
        QVERIFY(d.backend->isOpen());
        QCOMPARE(d.device.openNotice(), QStringLiteral("baud 38400 requested, device reports 38600"));

        Dut exact;
        QVERIFY(exact.device.open(mapping(), &why));
        QCOMPARE(exact.device.openNotice(), QString());

        d.backend->reportedBaudRate.reset();   // a later open with an exact read-back clears the notice
        QVERIFY(d.device.open(mapping(), &why));
        QCOMPARE(d.device.openNotice(), QString());
    }

    // The read-back is the OS's (D14, HW 5): a read-back the OS refuses fails the open too.
    void nativeReadBackFailureFailsOpen()
    {
        Dut d;
        d.backend->nativeReadError = QStringLiteral("tcgetattr failed: Input/output error");
        QString why;
        QVERIFY(!d.device.open(mapping(), &why));
        QCOMPARE(why, QStringLiteral("could not read the port settings back: tcgetattr failed: Input/output error"));
        QVERIFY(!d.backend->isOpen());
        QVERIFY(d.backend->calls(QStringLiteral("setDataTerminalReady")).isEmpty());
    }

    void lineSettingsMismatchNamesTheFirstDifference()
    {
        const LineSettings want{38400, QSerialPort::Data8, QSerialPort::NoParity, QSerialPort::OneStop,
                                QSerialPort::NoFlowControl};
        QCOMPARE(lineSettingsMismatch(want, want), QString());
        LineSettings got = want;
        got.baudRate = 9600;
        QCOMPARE(lineSettingsMismatch(want, got), QStringLiteral("baud rate 38400 not applied (device reports 9600)"));
        got = want;
        got.baudRate.reset();
        QCOMPARE(lineSettingsMismatch(want, got), QStringLiteral("baud rate 38400 not applied (device reports an unknown rate)"));
        QCOMPARE(lineSettingsNote(want, got), QString());

        // The rate's tolerance is ±2 % inclusive (UART framing; USB bridges report the rate they
        // programmed). 2 % of 38400 is exactly 768.
        QCOMPARE(kBaudTolerancePercent, qint64(2));
        QCOMPARE(lineSettingsNote(want, want), QString());
        got = want;
        got.baudRate = 38400 + 300;   // inside
        QCOMPARE(lineSettingsMismatch(want, got), QString());
        QCOMPARE(lineSettingsNote(want, got), QStringLiteral("baud 38400 requested, device reports 38700"));
        got.baudRate = 38400 - 300;
        QCOMPARE(lineSettingsMismatch(want, got), QString());
        QCOMPARE(lineSettingsNote(want, got), QStringLiteral("baud 38400 requested, device reports 38100"));
        got.baudRate = 38400 + 768;   // at the edge
        QCOMPARE(lineSettingsMismatch(want, got), QString());
        QCOMPARE(lineSettingsNote(want, got), QStringLiteral("baud 38400 requested, device reports 39168"));
        got.baudRate = 38400 - 768;
        QCOMPARE(lineSettingsMismatch(want, got), QString());
        got.baudRate = 38400 + 769;   // just outside
        QCOMPARE(lineSettingsMismatch(want, got), QStringLiteral("baud rate 38400 not applied (device reports 39169)"));
        QCOMPARE(lineSettingsNote(want, got), QString());
        got.baudRate = 38400 - 769;
        QCOMPARE(lineSettingsMismatch(want, got), QStringLiteral("baud rate 38400 not applied (device reports 37631)"));
        // The tolerance is the rate's only: within it, the other fields still compare exactly.
        got = want;
        got.baudRate = 38700;
        got.parity = QSerialPort::EvenParity;
        QCOMPARE(lineSettingsMismatch(want, got), QStringLiteral("parity not applied"));
        got = want;
        got.dataBits = QSerialPort::Data7;
        QCOMPARE(lineSettingsMismatch(want, got), QStringLiteral("data bits 8 not applied"));
        got = want;
        got.parity = QSerialPort::MarkParity;
        QCOMPARE(lineSettingsMismatch(want, got), QStringLiteral("parity not applied"));
        got = want;
        got.stopBits = QSerialPort::TwoStop;
        QCOMPARE(lineSettingsMismatch(want, got), QStringLiteral("stop bits not applied"));
        got = want;
        got.flowControl.reset();   // the device reports a mode QSerialPort never sets
        QCOMPARE(lineSettingsMismatch(want, got), QStringLiteral("handshake not applied"));
    }

#if defined(Q_OS_WIN)
    // GetCommState's DCB as QSerialPort sets it (qserialport_win.cpp) → the settings.
    void decodeDcbReadsWhatQSerialPortSets()
    {
        DcbFields f;
        f.baudRate = 38400;
        f.byteSize = 7;
        f.parity = 3;     // MARKPARITY
        f.stopBits = 2;   // TWOSTOPBITS
        LineSettings s = decodeDcb(f);
        QCOMPARE(*s.baudRate, 38400);
        QCOMPARE(*s.dataBits, QSerialPort::Data7);
        QCOMPARE(*s.parity, QSerialPort::MarkParity);
        QCOMPARE(*s.stopBits, QSerialPort::TwoStop);
        QCOMPARE(*s.flowControl, QSerialPort::NoFlowControl);
        f.rtsControl = 1;   // RTS_CONTROL_ENABLE: a line level, not flow control
        QCOMPARE(*decodeDcb(f).flowControl, QSerialPort::NoFlowControl);
        f.outxCtsFlow = true;
        f.rtsControl = 2;   // RTS_CONTROL_HANDSHAKE
        QCOMPARE(*decodeDcb(f).flowControl, QSerialPort::HardwareControl);
        f.outX = f.inX = true;   // both: nothing QSerialPort sets
        QVERIFY(!decodeDcb(f).flowControl);
        f.outxCtsFlow = false;
        f.rtsControl = 0;
        QCOMPARE(*decodeDcb(f).flowControl, QSerialPort::SoftwareControl);
        f.byteSize = 9;
        f.parity = 7;
        f.stopBits = 5;
        s = decodeDcb(f);
        QVERIFY(!s.dataBits && !s.parity && !s.stopBits);
        f.parity = 1;
        QCOMPARE(*decodeDcb(f).parity, QSerialPort::OddParity);
        f.parity = 2;
        QCOMPARE(*decodeDcb(f).parity, QSerialPort::EvenParity);
        f.parity = 4;
        QCOMPARE(*decodeDcb(f).parity, QSerialPort::SpaceParity);
        f.stopBits = 1;
        QCOMPARE(*decodeDcb(f).stopBits, QSerialPort::OneAndHalfStop);
    }
#else
    // termios as QSerialPort sets it (qserialport_unix.cpp) → the settings.
    void decodeTermiosReadsWhatQSerialPortSets()
    {
        struct termios t {};
        t.c_cflag = CS7 | PARENB | PARODD | CSTOPB;
        LineSettings s = decodeTermios(t, 38400);
        QCOMPARE(*s.baudRate, 38400);
        QCOMPARE(*s.dataBits, QSerialPort::Data7);
        QCOMPARE(*s.parity, QSerialPort::OddParity);
        QCOMPARE(*s.stopBits, QSerialPort::TwoStop);
        QCOMPARE(*s.flowControl, QSerialPort::NoFlowControl);
        t.c_cflag = CS8 | PARENB;
        s = decodeTermios(t, std::nullopt);
        QVERIFY(!s.baudRate);
        QCOMPARE(*s.dataBits, QSerialPort::Data8);
        QCOMPARE(*s.parity, QSerialPort::EvenParity);
        QCOMPARE(*s.stopBits, QSerialPort::OneStop);
        t.c_cflag = CS5;
        QCOMPARE(*decodeTermios(t, 9600).dataBits, QSerialPort::Data5);
        QCOMPARE(*decodeTermios(t, 9600).parity, QSerialPort::NoParity);
#ifdef CMSPAR
        t.c_cflag = CS8 | PARENB | CMSPAR | PARODD;
        QCOMPARE(*decodeTermios(t, 9600).parity, QSerialPort::MarkParity);
        t.c_cflag = CS8 | PARENB | CMSPAR;
        QCOMPARE(*decodeTermios(t, 9600).parity, QSerialPort::SpaceParity);
#endif
        t.c_cflag = CS8 | CRTSCTS;
        QCOMPARE(*decodeTermios(t, 9600).flowControl, QSerialPort::HardwareControl);
        t.c_cflag = CS8;
        t.c_iflag = IXON | IXOFF | IXANY;
        QCOMPARE(*decodeTermios(t, 9600).flowControl, QSerialPort::SoftwareControl);
        t.c_iflag = IXON;   // half of software flow control: nothing QSerialPort sets
        QVERIFY(!decodeTermios(t, 9600).flowControl);
        t.c_cflag = CS8 | CRTSCTS;
        t.c_iflag = IXON | IXOFF;
        QVERIFY(!decodeTermios(t, 9600).flowControl);
    }

    void rateFromSpeedMapsTheConstants()
    {
        QCOMPARE(*rateFromSpeed(B9600), 9600);
        QCOMPARE(*rateFromSpeed(B38400), 38400);
        QCOMPARE(*rateFromSpeed(B115200), 115200);
        QVERIFY(!rateFromSpeed(B0));
    }
#endif

    void errorLeftOnThePortFailsOpen()
    {
        Dut d;
        d.backend->errorAfterOpen = QSerialPort::ResourceError;
        QVERIFY(!d.device.open(mapping(), nullptr));
        QVERIFY(!d.backend->isOpen());
    }

    void openFailureReportsTheReason()
    {
        Dut d;
        d.backend->fail[QStringLiteral("open")] = QSerialPort::DeviceNotFoundError;
        QString why;
        QVERIFY(!d.device.open(mapping(), &why));
        QCOMPARE(why, QStringLiteral("injected error %1").arg(int(QSerialPort::DeviceNotFoundError)));
        // A later attempt starts clean and can succeed (open retries, INV-3).
        d.backend->fail.clear();
        QVERIFY(d.device.open(mapping(), &why));
    }

    void clearFailureFailsOpen()
    {
        Dut d;
        d.backend->fail[QStringLiteral("clear")] = QSerialPort::UnknownError;
        QVERIFY(!d.device.open(mapping(), nullptr));
        QVERIFY(!d.backend->isOpen());
    }

    void unsupportedValuesFailBeforeOpening_data()
    {
        QTest::addColumn<int>("which");
        QTest::newRow("StopBits.None") << 0;
        QTest::newRow("RequestToSendXOnXOff") << 1;
        QTest::newRow("data bits 9") << 2;
        QTest::newRow("undefined parity") << 3;
    }
    void unsupportedValuesFailBeforeOpening()
    {
        QFETCH(int, which);
        PortMapping m = mapping();
        if (which == 0) m.stopBits = StopBits::None;
        if (which == 1) m.handshake = Handshake::RequestToSendXOnXOff;
        if (which == 2) m.dataBits = 9;
        if (which == 3) m.parity = Parity(17);
        Dut d;
        QString why;
        QVERIFY(!d.device.open(m, &why));
        QVERIFY(why.startsWith(QStringLiteral("unsupported")));
        QVERIFY(d.backend->calls(QStringLiteral("open")).isEmpty());
    }

    // WS-3 handoff: the config's enum values → QSerialPort.
    void mappingTable()
    {
        QCOMPARE(*toQt(Parity::None), QSerialPort::NoParity);
        QCOMPARE(*toQt(Parity::Odd), QSerialPort::OddParity);
        QCOMPARE(*toQt(Parity::Even), QSerialPort::EvenParity);
        QCOMPARE(*toQt(Parity::Mark), QSerialPort::MarkParity);
        QCOMPARE(*toQt(Parity::Space), QSerialPort::SpaceParity);
        QCOMPARE(*toQt(StopBits::One), QSerialPort::OneStop);
        QCOMPARE(*toQt(StopBits::Two), QSerialPort::TwoStop);
        QCOMPARE(*toQt(StopBits::OnePointFive), QSerialPort::OneAndHalfStop);
        QVERIFY(!toQt(StopBits::None));
        QCOMPARE(*toQt(Handshake::None), QSerialPort::NoFlowControl);
        QCOMPARE(*toQt(Handshake::XOnXOff), QSerialPort::SoftwareControl);
        QCOMPARE(*toQt(Handshake::RequestToSend), QSerialPort::HardwareControl);
        QVERIFY(!toQt(Handshake::RequestToSendXOnXOff));
        for (int b = 5; b <= 8; ++b) QCOMPARE(int(*toQtDataBits(b)), b);
        QVERIFY(!toQtDataBits(4));
    }

    void errorsAreForwardedAndMapped()
    {
        Dut d;
        QVERIFY(d.device.open(mapping(), nullptr));
        QSignalSpy spy(&d.device, &ISerialDevice::errorOccurred);
        d.backend->emitError(QSerialPort::NoError);   // QSerialPort resets with NoError: not an error
        QCOMPARE(spy.count(), 0);
        d.backend->emitError(QSerialPort::ResourceError);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).value<SerialError>(), SerialError::Resource);
        QCOMPARE(toSerialError(QSerialPort::DeviceNotFoundError), SerialError::NotFound);
        QCOMPARE(toSerialError(QSerialPort::PermissionError), SerialError::Permission);
        QCOMPARE(toSerialError(QSerialPort::TimeoutError), SerialError::Timeout);
    }

    void readAndWriteOnAClosedDeviceAreErrors()
    {
        Dut d;
        char buf[4];
        QCOMPARE(d.device.read(buf, 4), qint64(-1));
        QCOMPARE(d.device.write("x", 1), qint64(-1));
        QVERIFY(d.device.open(mapping(), nullptr));
        QCOMPARE(d.device.write("xy", 2), qint64(2));
        d.device.close();
        QCOMPARE(d.device.read(buf, 4), qint64(-1));
    }

    // The OS output queue is passed through as the backend reports it (nullopt: cannot tell), and a
    // closed port reports nothing.
    void nativeOutputPendingIsTheOpenPortsQueue()
    {
        Dut d;
        QCOMPARE(d.device.nativeOutputPending(), std::optional<qint64>());
        QVERIFY(d.device.open(mapping(), nullptr));
        QCOMPARE(d.device.nativeOutputPending(), std::optional<qint64>(0));
        d.backend->nativeQueue = 37;
        QCOMPARE(d.device.nativeOutputPending(), std::optional<qint64>(37));
        d.backend->nativeQueue = std::nullopt;
        QCOMPARE(d.device.nativeOutputPending(), std::optional<qint64>());
        d.backend->nativeQueue = 37;
        d.device.close();
        QCOMPARE(d.device.nativeOutputPending(), std::optional<qint64>());
    }
};

QTEST_GUILESS_MAIN(tst_QtSerialDevice)
#include "tst_QtSerialDevice.moc"
