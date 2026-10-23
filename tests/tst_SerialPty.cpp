// The real QSerialPort path on a pseudo-terminal (WS-4; plus hangup and replacement): QtSerialDevice over QSerialPort on /dev/pts/N, the test playing the device on the
// master side. Linux only; never skipped there (the pty pair is made in-process).

#include "engine/NativeSerial.h"
#include "engine/PortBridge.h"
#include "engine/QtSerialDevice.h"
#include "support/EngineTestSupport.h"
#include "support/PtyPair.h"

#include <QElapsedTimer>
#include <QTcpSocket>
#include <QTest>

#include <memory>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

using namespace asb;
using namespace asb::engine;
using namespace asb::test;

namespace {

PortMapping mapping(const QString& path)
{
    PortMapping m;
    m.name = QStringLiteral("pty");
    m.comPort = path;
    m.tcpPort = 0;
    m.bindAddress = QStringLiteral("127.0.0.1");
    return m;
}

QByteArray readExactly(QTcpSocket& s, qsizetype n, int ms = 5000)
{
    QByteArray out;
    QElapsedTimer t;
    t.start();
    while (out.size() < n && t.elapsed() < ms) {
        if (s.bytesAvailable() > 0) {
            out += s.read(n - out.size());
        } else {
            QTest::qWait(5);
        }
    }
    return out;
}

// The master side, read while the event loop keeps running (the bridge lives on this thread).
QByteArray readMaster(PtyPair& pty, int want, int ms = 5000)
{
    QByteArray out;
    QElapsedTimer t;
    t.start();
    while (out.size() < want && t.elapsed() < ms) {
        out += pty.readMaster(want - int(out.size()), 5);
        QTest::qWait(5);
    }
    return out;
}

struct PtyRig {
    PtyPair pty;
    RecordingSideChannel side;
    std::unique_ptr<PortBridge> bridge;
    PtyRig() { bridge = std::make_unique<PortBridge>(mapping(pty.slavePath()), &makeQtSerialDevice, side); }
    quint16 port() { return bridge->server().serverPort(); }
};

// The real QtSerialDevice behind a first open that is refused, as Windows refuses a COM port for a
// moment after a replaced client closed it; `attempts` counts the open() calls.
class FirstOpenRefused final : public ISerialDevice {
public:
    explicit FirstOpenRefused(std::shared_ptr<int> attempts) : m_attempts(std::move(attempts)), m_inner(makeQtSerialDevice())
    {
        connect(m_inner.get(), &ISerialDevice::readyRead, this, &ISerialDevice::readyRead);
        connect(m_inner.get(), &ISerialDevice::bytesWritten, this, &ISerialDevice::bytesWritten);
        connect(m_inner.get(), &ISerialDevice::errorOccurred, this, &ISerialDevice::errorOccurred);
    }
    bool open(const PortMapping& m, QString* why) override
    {
        if (++*m_attempts == 1) {
            if (why) *why = QStringLiteral("refused once");
            return false;
        }
        return m_inner->open(m, why);
    }
    void close() override { m_inner->close(); }
    qint64 read(char* d, qint64 n) override { return m_inner->read(d, n); }
    qint64 write(const char* d, qint64 n) override { return m_inner->write(d, n); }
    qint64 bytesAvailable() const override { return m_inner->bytesAvailable(); }
    qint64 bytesToWrite() const override { return m_inner->bytesToWrite(); }
    std::optional<qint64> nativeOutputPending() const override { return m_inner->nativeOutputPending(); }
    void setReadBufferSize(qint64 n) override { m_inner->setReadBufferSize(n); }
    QString openNotice() const override { return m_inner->openNotice(); }

private:
    std::shared_ptr<int> m_attempts;
    std::unique_ptr<ISerialDevice> m_inner;
};

std::unique_ptr<QTcpSocket> connectTo(quint16 port)
{
    auto s = std::make_unique<QTcpSocket>();
    s->connectToHost(QHostAddress::LocalHost, port);
    return s;
}

}  // namespace

class tst_SerialPty : public QObject {
    Q_OBJECT

private slots:
    void realSerialPortOnAPtyRoundTripsThroughTheBridge()
    {
        PtyRig r;
        QVERIFY2(r.pty.ok(), qPrintable(r.pty.error()));
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_VERIFY2_WITH_TIMEOUT(r.bridge->snapshot().state == BridgeState::Connected,
                                  qPrintable(r.bridge->snapshot().lastError.value_or(QString())), 5000);
        client->write("RV;");
        QCOMPARE(readMaster(r.pty, 3), QByteArray("RV;"));
        QVERIFY(r.pty.writeMaster("RV02.16;"));
        QCOMPARE(readExactly(*client, 8), QByteArray("RV02.16;"));
        r.bridge->stop();
        QCOMPARE(r.bridge->snapshot().state, BridgeState::Stopped);
    }

    // HW 7 on a pty: every byte value, both ways, unchanged (ACOM's 0xFF, raw mode).
    void everyByteValueUnchanged()
    {
        PtyRig r;
        QVERIFY2(r.pty.ok(), qPrintable(r.pty.error()));
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.bridge->snapshot().state, BridgeState::Connected, 5000);
        QByteArray all(256, Qt::Uninitialized);
        for (int i = 0; i < 256; ++i) all[i] = char(i);
        client->write(all);
        QCOMPARE(readMaster(r.pty, 256), all);
        QVERIFY(r.pty.writeMaster(all));
        QCOMPARE(readExactly(*client, 256), all);
    }

    // INV-59: unplugging the device mid-session → Listening with "device error: …".
    void ptyHangupRecordsDeviceError()
    {
        PtyRig r;
        QVERIFY2(r.pty.ok(), qPrintable(r.pty.error()));
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.bridge->snapshot().state, BridgeState::Connected, 5000);
        client->write("x");
        QCOMPARE(readMaster(r.pty, 1), QByteArray("x"));
        r.pty.closeMaster();
        QTRY_COMPARE_WITH_TIMEOUT(r.bridge->snapshot().state, BridgeState::Listening, 5000);
        const QString err = r.bridge->snapshot().lastError.value_or(QString());
        QVERIFY2(err.startsWith(QStringLiteral("device error: ")), qPrintable(err));
        QTRY_COMPARE_WITH_TIMEOUT(client->state(), QAbstractSocket::UnconnectedState, 5000);
    }

    // INV-2 on the real device: the replacement reopens the same tty.
    void replacementReopensThePty()
    {
        PtyRig r;
        QVERIFY2(r.pty.ok(), qPrintable(r.pty.error()));
        r.bridge->start();
        auto first = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.bridge->snapshot().state, BridgeState::Connected, 5000);
        auto second = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(first->state(), QAbstractSocket::UnconnectedState, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(r.bridge->snapshot().state, BridgeState::Connected, 5000);
        second->write("AN;");
        QCOMPARE(readMaster(r.pty, 3), QByteArray("AN;"));
        QVERIFY(r.pty.writeMaster("AN1;"));
        QCOMPARE(readExactly(*second, 4), QByteArray("AN1;"));
    }

    // Client EOF on the real device (`printf 'PS0;' | nc -q0 host port`): the bytes reach
    // the tty although the client closed right after sending.
    void sendAndCloseReachesThePty()
    {
        PtyRig r;
        QVERIFY2(r.pty.ok(), qPrintable(r.pty.error()));
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.bridge->snapshot().state, BridgeState::Connected, 5000);
        client->write("PS0;");
        client->disconnectFromHost();
        QCOMPARE(readMaster(r.pty, 4), QByteArray("PS0;"));
        QTRY_COMPARE_WITH_TIMEOUT(r.bridge->snapshot().state, BridgeState::Listening, 5000);
        QVERIFY(!r.bridge->snapshot().lastError.has_value());
    }

    // More than the kernel and the buffers hold, then the close: the master side reads it all, in
    // order, after the client is gone (the drain waits on the real Qt buffer and the tty's queue).
    void bulkSendAndCloseReachesThePtyWhole()
    {
        PtyRig r;
        QVERIFY2(r.pty.ok(), qPrintable(r.pty.error()));
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.bridge->snapshot().state, BridgeState::Connected, 5000);
        QByteArray payload(200 * 1024, Qt::Uninitialized);
        for (qsizetype i = 0; i < payload.size(); ++i) payload[i] = char((i * 13 + i / 509) & 0xFF);
        client->write(payload);
        client->disconnectFromHost();
        const QByteArray got = readMaster(r.pty, int(payload.size()), 20000);
        QCOMPARE(got.size(), payload.size());
        QVERIFY(got == payload);
        QTRY_COMPARE_WITH_TIMEOUT(r.bridge->snapshot().state, BridgeState::Listening, 5000);
        QVERIFY(!r.bridge->snapshot().lastError.has_value());
    }

    // The production query on a real tty (TIOCOUTQ): an open pty answers (a value, not "cannot
    // tell"), and the answer is 0 once Qt's buffer is empty, the bytes unread on the master side
    // included: a pty has no output queue of its own. A closed device answers nothing.
    void openPtyReportsItsOutputQueue()
    {
        PtyPair pty;
        QVERIFY2(pty.ok(), qPrintable(pty.error()));
        auto dev = makeQtSerialDevice();
        QCOMPARE(dev->nativeOutputPending(), std::optional<qint64>());
        QString why;
        QVERIFY2(dev->open(mapping(pty.slavePath()), &why), qPrintable(why));
        QCOMPARE(dev->nativeOutputPending(), std::optional<qint64>(0));
        QCOMPARE(dev->write("PS0;", 4), qint64(4));
        QTRY_COMPARE_WITH_TIMEOUT(dev->bytesToWrite(), qint64(0), 5000);
        QCOMPARE(dev->nativeOutputPending(), std::optional<qint64>(0));   // still unread on the master
        QCOMPARE(readMaster(pty, 4), QByteArray("PS0;"));
        dev->close();
        QCOMPARE(dev->nativeOutputPending(), std::optional<qint64>());
    }

    // The same while the session is still Opening (its first open refused, the retry pending): the
    // retry goes on, and the bytes reach the tty once it opens.
    void sendAndCloseDuringTheOpenRetryReachesThePty()
    {
        PtyPair pty;
        QVERIFY2(pty.ok(), qPrintable(pty.error()));
        RecordingSideChannel side;
        auto attempts = std::make_shared<int>(0);
        EngineTimings t;
        t.openRetryDelay = std::chrono::milliseconds(1000);
        PortBridge bridge(mapping(pty.slavePath()),
                          [attempts] { return std::make_unique<FirstOpenRefused>(attempts); }, side, t);
        bridge.start();
        auto client = connectTo(bridge.server().serverPort());
        QTRY_COMPARE_WITH_TIMEOUT(*attempts, 1, 5000);
        client->write("PS0;");
        client->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(bridge.currentSession() && bridge.currentSession()->clientClosed, 5000);
        QCOMPARE(*attempts, 1);
        QCOMPARE(readMaster(pty, 4), QByteArray("PS0;"));
        QTRY_COMPARE_WITH_TIMEOUT(bridge.snapshot().state, BridgeState::Listening, 5000);
        QCOMPARE(*attempts, 2);
        QVERIFY(!bridge.snapshot().lastError.has_value());
    }

    // D14 / HW 5 on the real path: QtSerialDevice::open reads the settings back from the tty
    // (tcgetattr, TCGETS2) and they are what was asked for; the test also reads the tty's termios
    // itself (through the master). A custom rate (250000) takes the BOTHER path.
    void openedSettingsAreWhatTheTtyReports_data()
    {
        QTest::addColumn<int>("baud");
        QTest::addColumn<int>("stopBits");
        QTest::addColumn<int>("handshake");
        QTest::newRow("9600 8N1") << 9600 << int(StopBits::One) << int(Handshake::None);
        QTest::newRow("38400 8N2") << 38400 << int(StopBits::Two) << int(Handshake::None);
        QTest::newRow("4800 8N1 xon/xoff") << 4800 << int(StopBits::One) << int(Handshake::XOnXOff);
        QTest::newRow("115200 8N1 rts/cts") << 115200 << int(StopBits::One) << int(Handshake::RequestToSend);
        QTest::newRow("250000 8N1 custom rate") << 250000 << int(StopBits::One) << int(Handshake::None);
    }
    void openedSettingsAreWhatTheTtyReports()
    {
        QFETCH(int, baud);
        QFETCH(int, stopBits);
        QFETCH(int, handshake);
        PtyPair pty;
        QVERIFY2(pty.ok(), qPrintable(pty.error()));
        PortMapping m = mapping(pty.slavePath());
        m.baudRate = baud;
        m.stopBits = StopBits(stopBits);
        m.handshake = Handshake(handshake);
        auto dev = makeQtSerialDevice();
        QString why;
        QVERIFY2(dev->open(m, &why), qPrintable(why));

        struct termios t {};
        QCOMPARE(::tcgetattr(pty.masterFd(), &t), 0);
        QCOMPARE(t.c_cflag & CSIZE, tcflag_t(CS8));
        QVERIFY(!(t.c_cflag & PARENB));
        QCOMPARE(bool(t.c_cflag & CSTOPB), stopBits == int(StopBits::Two));
        QCOMPARE(bool(t.c_cflag & CRTSCTS), handshake == int(Handshake::RequestToSend));
        QCOMPARE(bool(t.c_iflag & IXON), handshake == int(Handshake::XOnXOff));
        if (baud == 250000) {
            QCOMPARE(t.c_cflag & CBAUD, tcflag_t(0010000));   // BOTHER: the rate itself is only in termios2
        } else {
            QCOMPARE(rateFromSpeed(::cfgetospeed(&t)), std::optional<qint32>(baud));
        }
        dev->close();
    }

    // The read-back is not a formality: a Linux pty forces CS8 and no parity whatever it is asked
    // (drivers/tty/pty.c), and QSerialPort's own getters would still report the requested values. The
    // open fails, names the setting, and leaves the port closed (it opens again at once).
    void settingsTheTtyDoesNotKeepFailTheOpen_data()
    {
        QTest::addColumn<int>("dataBits");
        QTest::addColumn<int>("parity");
        QTest::addColumn<QString>("why");
        QTest::newRow("7N1") << 7 << int(Parity::None) << QStringLiteral("data bits 7 not applied");
        QTest::newRow("8E1") << 8 << int(Parity::Even) << QStringLiteral("parity not applied");
        QTest::newRow("8M1") << 8 << int(Parity::Mark) << QStringLiteral("parity not applied");
    }
    void settingsTheTtyDoesNotKeepFailTheOpen()
    {
        QFETCH(int, dataBits);
        QFETCH(int, parity);
        QFETCH(QString, why);
        PtyPair pty;
        QVERIFY2(pty.ok(), qPrintable(pty.error()));
        PortMapping m = mapping(pty.slavePath());
        m.dataBits = dataBits;
        m.parity = Parity(parity);
        auto dev = makeQtSerialDevice();
        QString got;
        QVERIFY(!dev->open(m, &got));
        QCOMPARE(got, why);
        QVERIFY2(dev->open(mapping(pty.slavePath()), &got), qPrintable(got));
        dev->close();
    }

    // A missing device refuses the client with the reason (INV-9 on the real path).
    void missingDeviceRefusesTheClient()
    {
        RecordingSideChannel side;
        PortBridge bridge(mapping(QStringLiteral("/dev/serial/by-id/asb-test-does-not-exist")), &makeQtSerialDevice, side);
        bridge.start();
        auto client = connectTo(bridge.server().serverPort());
        QTRY_COMPARE_WITH_TIMEOUT(client->state(), QAbstractSocket::UnconnectedState, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(bridge.snapshot().lastError.has_value(), 5000);
        QVERIFY(bridge.snapshot().lastError->startsWith(QStringLiteral("Could not open /dev/serial/by-id/asb-test-does-not-exist: ")));
    }
};

QTEST_GUILESS_MAIN(tst_SerialPty)
#include "tst_SerialPty.moc"
