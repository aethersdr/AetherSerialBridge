// The session engine (WS-4, master §4.3): PortBridge, Session, Pump and BridgeHost against
// FakeSerialDevice and real loopback TCP: the bridge's behaviour and its invariants: INV-1, 2, 3, 4, 5, 6, 9, 10, 27, 28, 55, 56, 57, 59, 80.

#include "core/SideChannel.h"
#include "engine/BridgeHost.h"
#include "engine/PortBridge.h"
#include "engine/Pump.h"
#include "support/EngineTestSupport.h"
#include "support/FakeSerialDevice.h"

#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#include <memory>

using namespace asb;
using namespace asb::engine;
using namespace asb::test;
using namespace std::chrono_literals;

namespace {

PortMapping mapping(int port = 0, const QString& com = QStringLiteral("COM3"))
{
    PortMapping m;
    m.name = QStringLiteral("test");
    m.comPort = com;
    m.tcpPort = port;   // 0: the OS picks one (read back from the bridge's server)
    m.bindAddress = QStringLiteral("127.0.0.1");
    return m;
}

// Buffer tests keep a device that never drains: no stall may end the session meanwhile.
EngineTimings noStall()
{
    EngineTimings t;
    t.stallTimeout = 60s;
    return t;
}

// The open retry is long enough that a client can write and close between two attempts on any CI leg.
EngineTimings slowRetry()
{
    EngineTimings t;
    t.openRetryDelay = 1000ms;
    t.listenRetryDelay = 100ms;
    t.stallTimeout = 600ms;
    return t;
}

EngineTimings fast()
{
    EngineTimings t;
    t.listenRetryDelay = 100ms;
    t.stallTimeout = 600ms;
    return t;   // openRetryDelay stays the real 250 ms
}

// One bridge on a fake device, logging into a RecordingSideChannel.
struct Rig {
    std::shared_ptr<FakeSerialWorld> world = std::make_shared<FakeSerialWorld>();
    RecordingSideChannel side;
    std::unique_ptr<PortBridge> bridge;

    explicit Rig(EngineTimings t = fast(), PortMapping m = mapping())
    {
        bridge = std::make_unique<PortBridge>(m, fakeSerialFactory(world), side, t);
    }
    quint16 port() { return bridge->server().serverPort(); }
    PortStatus status() const { return bridge->snapshot(); }
    FakeSerialDevice* dev(int i) const { return i <= world->devices.size() ? world->devices[i - 1].data() : nullptr; }
};

std::unique_ptr<QTcpSocket> connectTo(quint16 port)
{
    auto s = std::make_unique<QTcpSocket>();
    s->connectToHost(QHostAddress::LocalHost, port);
    return s;
}

bool waitConnected(QTcpSocket& s, int ms = 5000)
{
    QElapsedTimer t;
    t.start();
    while (s.state() != QAbstractSocket::ConnectedState && t.elapsed() < ms) QTest::qWait(10);
    return s.state() == QAbstractSocket::ConnectedState;
}

// Reads exactly `n` bytes from a client socket, spinning the event loop.
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

bool waitClosedByPeer(QTcpSocket& s, int ms = 5000)
{
    QElapsedTimer t;
    t.start();
    while (s.state() != QAbstractSocket::UnconnectedState && t.elapsed() < ms) QTest::qWait(10);
    return s.state() == QAbstractSocket::UnconnectedState;
}

QByteArray pattern(qsizetype n)
{
    QByteArray b(n, Qt::Uninitialized);
    for (qsizetype i = 0; i < n; ++i) b[i] = char((i * 7 + i / 251) & 0xFF);
    return b;
}

}  // namespace

class tst_PortBridge : public QObject {
    Q_OBJECT

private slots:
    // ----------------------------------------------------------------------------------------------
    // The basics
    // ----------------------------------------------------------------------------------------------
    void bytesFlowBothWays()
    {
        Rig r;
        r.bridge->start();
        QCOMPARE(r.status().state, BridgeState::Listening);
        auto client = connectTo(r.port());
        QVERIFY(waitConnected(*client));
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        FakeSerialDevice* dev = r.dev(1);
        QVERIFY(dev);

        client->write("RV;");
        QTRY_COMPARE_WITH_TIMEOUT(dev->wire, QByteArray("RV;"), 5000);
        dev->feed("RV02.16;");
        QCOMPARE(readExactly(*client, 8), QByteArray("RV02.16;"));

        QTRY_COMPARE_WITH_TIMEOUT(r.status().bytesToSerial, qint64(3), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(r.status().bytesToClient, qint64(8), 5000);
        QCOMPARE(r.status().client, std::optional<QString>(r.bridge->currentSession()->endpoint));

        r.bridge->stop();
        QCOMPARE(r.status().state, BridgeState::Stopped);
        QVERIFY(!dev->isOpen);
        QVERIFY(waitClosedByPeer(*client));
    }

    // INV-1: the device is opened only while a client is connected.
    void serialOpenOnlyWhileConnected()
    {
        Rig r;
        r.bridge->start();
        QTest::qWait(100);
        QCOMPARE(r.world->openCalls, 0);
        QCOMPARE(r.world->created, 0);

        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.world->opens, 1, 5000);
        QVERIFY(r.dev(1)->isOpen);

        client->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(!r.dev(1) || !r.dev(1)->isOpen, 5000);
        QVERIFY(r.world->events.contains(QStringLiteral("close 1")));
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Listening, 5000);
        QCOMPARE(r.world->opens, 1);
    }

    // INV-1: a host without a factory (every non-service mode) opens nothing and refuses clients.
    void noFactoryRefusesEveryClient()
    {
        RecordingSideChannel side;
        BridgeHost host({}, side, fast());
        BridgeConfig cfg;
        cfg.ports << mapping();
        host.start(cfg);
        auto client = connectTo(host.bridges().front()->server().serverPort());
        QVERIFY(waitClosedByPeer(*client));
        QTRY_VERIFY_WITH_TIMEOUT(host.snapshot().front().lastError.has_value(), 5000);
        QCOMPARE(*host.snapshot().front().lastError, QStringLiteral("Could not open COM3: no serial device in this mode"));
        QCOMPARE(host.snapshot().front().state, BridgeState::Listening);
    }

    // INV-2: the old client and its device are closed before the new device is opened.
    void newClientReplacesOldAndReopens()
    {
        Rig r;
        r.bridge->start();
        auto first = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.world->opens, 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);

        auto second = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.world->opens, 2, 5000);
        const QStringList& ev = r.world->events;
        QVERIFY2(ev.indexOf(QStringLiteral("close 1")) >= 0, qPrintable(ev.join(u',')));
        QVERIFY2(ev.indexOf(QStringLiteral("close 1")) < ev.indexOf(QStringLiteral("open 2")), qPrintable(ev.join(u',')));
        QVERIFY(waitClosedByPeer(*first));

        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        second->write("AN;");
        QTRY_COMPARE_WITH_TIMEOUT(r.dev(2)->wire, QByteArray("AN;"), 5000);
        r.dev(2)->feed("AN1;");
        QCOMPARE(readExactly(*second, 4), QByteArray("AN1;"));
        QVERIFY(r.dev(1) == nullptr || r.dev(1)->wire.isEmpty());
    }

    // INV-3: three attempts, 250 ms apart, then the client is refused.
    void openRetriesExactlyThreeAt250ms()
    {
        Rig r;
        r.world->failNextOpens = 100;
        r.bridge->start();
        auto client = connectTo(r.port());
        QVERIFY(waitClosedByPeer(*client));
        QTRY_VERIFY_WITH_TIMEOUT(r.status().lastError.has_value(), 5000);
        QTest::qWait(700);   // a fourth attempt would have come by now
        QCOMPARE(r.world->openCalls, 3);
        const QList<qint64>& at = r.world->openCallMs;
        QVERIFY2(at[1] - at[0] >= 240, qPrintable(QString::number(at[1] - at[0])));
        QVERIFY2(at[2] - at[1] >= 240, qPrintable(QString::number(at[2] - at[1])));
        QCOMPARE(kOpenAttempts, 3);
        QCOMPARE(EngineTimings{}.openRetryDelay, std::chrono::milliseconds(250));
    }

    // INV-3: replacement cancels the old session's retries (its third attempt would have succeeded).
    void replaceDuringRetryCancels()
    {
        Rig r;
        r.world->configure = [](FakeSerialDevice& d) {
            if (d.id == 1) d.failOpens = 2;
        };
        r.bridge->start();
        auto first = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.world->openCalls, 1, 5000);
        QPointer<FakeSerialDevice> d1 = r.dev(1);
        auto second = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.world->opens, 1, 5000);
        QTest::qWait(800);   // past the old session's attempts 2 and 3
        QVERIFY(!r.world->events.contains(QStringLiteral("open 1")));
        QCOMPARE(r.world->events.count(QStringLiteral("open-failed 1")), 1);
        QVERIFY(r.world->events.contains(QStringLiteral("open 2")));
        QCOMPARE(r.world->openCalls, 2);
        QCOMPARE(r.status().state, BridgeState::Connected);
    }

    // INV-3: the client leaving during the retries cancels them.
    void disconnectDuringRetryCancels()
    {
        Rig r;
        r.world->configure = [](FakeSerialDevice& d) { d.failOpens = 2; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.world->openCalls, 1, 5000);
        client->abort();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() == nullptr, 5000);
        QTest::qWait(800);
        QCOMPARE(r.world->openCalls, 1);
        QCOMPARE(r.world->opens, 0);
        QCOMPARE(r.status().state, BridgeState::Listening);
    }

    // INV-3: stop cancels the retries.
    void stopDuringRetryCancels()
    {
        Rig r;
        r.world->configure = [](FakeSerialDevice& d) { d.failOpens = 2; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.world->openCalls, 1, 5000);
        r.bridge->stop();
        QTest::qWait(800);
        QCOMPARE(r.world->openCalls, 1);
        QCOMPARE(r.world->opens, 0);
        QCOMPARE(r.status().state, BridgeState::Stopped);
        QVERIFY(waitClosedByPeer(*client));
    }

    // INV-4
    void endSessionIsIdempotentAndClosesBoth()
    {
        Rig r;
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        QPointer<Session> s = r.bridge->currentSession();
        FakeSerialDevice* dev = r.dev(1);
        r.bridge->endSession(s, EndReason::DeviceError, QStringLiteral("device error: first"));
        r.bridge->endSession(s, EndReason::DeviceError, QStringLiteral("device error: second"));
        QCOMPARE(dev->closeCalls, 1);
        QCOMPARE(r.side.count(QStringLiteral("disconnected")), 1);
        QCOMPARE(r.status().lastError, std::optional<QString>(QStringLiteral("device error: first")));
        QVERIFY(r.bridge->currentSession() == nullptr);
        QVERIFY(waitClosedByPeer(*client));
        QTRY_VERIFY_WITH_TIMEOUT(s.isNull(), 5000);   // deleted later
    }

    // INV-5
    void listenerRetriesAndBindsAfterRelease()
    {
        QTcpServer taken;
        QVERIFY(taken.listen(QHostAddress::LocalHost, 0));
        Rig r(fast(), mapping(taken.serverPort()));
        r.bridge->start();
        QCOMPARE(r.status().state, BridgeState::Error);
        QVERIFY(r.status().lastError->startsWith(QStringLiteral("Listener on 127.0.0.1:%1 failed:").arg(taken.serverPort())));
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->listenAttempts() >= 3, 5000);
        QCOMPARE(r.side.count(QStringLiteral("retrying in 0.1s")), r.bridge->listenAttempts());
        const quint16 port = taken.serverPort();
        taken.close();

        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Listening, 5000);
        auto client = connectTo(port);
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        client->write("SN;");
        QTRY_COMPARE_WITH_TIMEOUT(r.dev(1)->wire, QByteArray("SN;"), 5000);
        QCOMPARE(EngineTimings{}.listenRetryDelay, std::chrono::milliseconds(10s));
    }

    // INV-5
    void stopCancelsListenRetry()
    {
        QTcpServer taken;
        QVERIFY(taken.listen(QHostAddress::LocalHost, 0));
        Rig r(fast(), mapping(taken.serverPort()));
        r.bridge->start();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->listenAttempts() >= 2, 5000);
        r.bridge->stop();
        const int n = r.bridge->listenAttempts();
        taken.close();
        QTest::qWait(400);
        QCOMPARE(r.bridge->listenAttempts(), n);
        QCOMPARE(r.status().state, BridgeState::Stopped);
    }

    // INV-5 [parity: OnePortFailingToListenDoesNotStopTheOthers]
    void onePortFailingToListenDoesNotStopTheOthers()
    {
        QTcpServer taken;
        QVERIFY(taken.listen(QHostAddress::LocalHost, 0));
        auto world = std::make_shared<FakeSerialWorld>();
        RecordingSideChannel side;
        BridgeHost host(fakeSerialFactory(world), side, fast());
        BridgeConfig cfg;
        cfg.ports << mapping(taken.serverPort(), QStringLiteral("COM2")) << mapping(0, QStringLiteral("COM3"));
        PortMapping off = mapping(0, QStringLiteral("COM9"));
        off.enabled = false;
        off.name = QStringLiteral("off");
        cfg.ports << off;
        host.start(cfg);
        QCOMPARE(host.bridges().size(), size_t(2));
        QCOMPARE(host.snapshot().size(), 2);
        QCOMPARE(host.snapshot()[0].comPort, QStringLiteral("COM2"));
        QCOMPARE(host.snapshot()[0].state, BridgeState::Error);

        auto client = connectTo(host.bridges()[1]->server().serverPort());
        QTRY_COMPARE_WITH_TIMEOUT(world->opens, 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(host.snapshot()[1].state, BridgeState::Connected, 5000);
        client->write("SN;");
        QTRY_COMPARE_WITH_TIMEOUT(world->last()->wire, QByteArray("SN;"), 5000);
        QCOMPARE(host.snapshot()[0].state, BridgeState::Error);

        bool done = false;
        host.stop([&] { done = true; });
        QVERIFY(!done);   // from the event loop, not inside stop()
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        for (const PortStatus& p : host.snapshot()) QCOMPARE(p.state, BridgeState::Stopped);
    }

    // INV-9 [parity: BusySerialPortDropsTheClientAndRecoversLater]
    void busySerialDropsClientAndRecovers()
    {
        Rig r(fast(), mapping(0, QStringLiteral("COM2")));
        r.world->failNextOpens = 3;
        r.bridge->start();
        auto refused = connectTo(r.port());
        QVERIFY(waitClosedByPeer(*refused));
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Listening, 5000);
        QVERIFY(r.status().lastError.has_value());
        QVERIFY(r.status().lastError->contains(QStringLiteral("COM2")));
        QVERIFY(r.status().lastError->startsWith(QStringLiteral("Could not open COM2: ")));
        QCOMPARE(r.side.count(QStringLiteral("refused, could not open COM2")), 1);

        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        client->write("^SV;");
        QTRY_COMPARE_WITH_TIMEOUT(r.world->last()->wire, QByteArray("^SV;"), 5000);
        QVERIFY(!r.status().lastError.has_value());
    }

    // INV-10 [parity: ShutdownAndReplacementAreLoggedDifferently], with the endpoints.
    void shutdownAndReplacementLogDifferently()
    {
        Rig r;
        r.bridge->start();
        auto first = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        const QString ep1 = r.bridge->currentSession()->endpoint;
        auto second = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.side.count(QStringLiteral("connected to")), 2, 5000);
        const QString ep2 = r.bridge->currentSession()->endpoint;
        QVERIFY(ep1 != ep2);
        r.bridge->stop();

        QCOMPARE(r.side.count(QStringLiteral("replacing client")), 1);
        QCOMPARE(r.side.count(QStringLiteral("bridge stopping, closing client")), 1);
        QCOMPARE(r.side.count(QStringLiteral("new client %1, replacing client %2").arg(ep2, ep1)), 1);
        QCOMPARE(r.side.count(QStringLiteral("bridge stopping, closing client %1").arg(ep2)), 1);
        QVERIFY(ep1.startsWith(QStringLiteral("127.0.0.1:")));
    }

    // ----------------------------------------------------------------------------------------------
    // INV-27: buffering bounds
    // ----------------------------------------------------------------------------------------------
    void sourceReadBufferCapped()
    {
        Rig r(noStall());
        r.world->configure = [](FakeSerialDevice& d) { d.drainMode = FakeSerialDevice::Drain::Manual; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        Session* s = r.bridge->currentSession();
        QCOMPARE(s->socket()->readBufferSize(), kCap);
        QCOMPARE(r.dev(1)->readBufferSize, kCap);

        // The device never drains: the client floods 4 MiB; what the bridge holds stays bounded.
        const QByteArray flood = pattern(4 * 1024 * 1024);
        client->write(flood);
        QTest::qWait(800);
        QVERIFY2(s->socket()->bytesAvailable() <= kCap, qPrintable(QString::number(s->socket()->bytesAvailable())));
        QCOMPARE(r.dev(1)->bytesToWrite(), kCap);
        QVERIFY(s->up->pendingBytes() <= kChunk);
    }

    void destinationCapRespected()
    {
        Rig r(noStall());
        r.world->configure = [](FakeSerialDevice& d) { d.drainMode = FakeSerialDevice::Drain::Manual; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        FakeSerialDevice* dev = r.dev(1);
        const QByteArray data = pattern(300 * 1024);
        client->write(data);
        QTRY_COMPARE_WITH_TIMEOUT(dev->bytesToWrite(), kCap, 5000);
        QTest::qWait(200);
        QCOMPARE(dev->bytesToWrite(), kCap);   // never more than kCap queued at the destination

        // Draining lets the rest through, in order and complete.
        QElapsedTimer t;
        t.start();
        while (dev->wire.size() < data.size() && t.elapsed() < 10000) {
            dev->drain();
            QTest::qWait(5);
            QVERIFY(dev->bytesToWrite() <= kCap);
        }
        QCOMPARE(dev->wire.size(), data.size());
        QCOMPARE(dev->wire, data);
    }

    void totalPerDirectionBound()
    {
        Rig r(noStall());
        r.world->configure = [](FakeSerialDevice& d) { d.drainMode = FakeSerialDevice::Drain::Manual; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        Session* s = r.bridge->currentSession();
        FakeSerialDevice* dev = r.dev(1);
        client->write(pattern(2 * 1024 * 1024));
        // The client does not read: the device produces 8 MiB towards it.
        client->setReadBufferSize(1);
        for (int i = 0; i < 128; ++i) dev->feed(pattern(64 * 1024));
        for (int i = 0; i < 40; ++i) {
            QTest::qWait(25);
            const qint64 up = s->socket()->bytesAvailable() + s->up->pendingBytes() + dev->bytesToWrite();
            const qint64 down = s->down->pendingBytes() + s->socket()->bytesToWrite();
            QVERIFY2(up <= 2 * kCap + 2 * kChunk, qPrintable(QString::number(up)));
            QVERIFY2(down <= kCap + kChunk, qPrintable(QString::number(down)));
        }
        QCOMPARE(2 * kCap + 2 * kChunk, qint64(136 * 1024));
    }

    // INV-28: on every accepted socket, not only the first.
    void socketOptionsOnEveryAccepted()
    {
        Rig r;
        r.bridge->start();
        auto first = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        auto check = [&] {
            QTcpSocket* s = r.bridge->currentSession()->socket();
            QCOMPARE(s->socketOption(QAbstractSocket::LowDelayOption).toInt(), 1);
            QVERIFY(s->socketOption(QAbstractSocket::KeepAliveOption).toInt() != 0);
            QCOMPARE(s->readBufferSize(), kCap);
        };
        check();
        auto second = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.world->opens, 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        check();
        auto third = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.world->opens, 3, 5000);
        check();
    }

    // ----------------------------------------------------------------------------------------------
    // INV-55, INV-56, INV-59
    // ----------------------------------------------------------------------------------------------
    void shortWritesLoseNothing()
    {
        Rig r;
        r.world->configure = [](FakeSerialDevice& d) { d.maxWritePerCall = 7; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        const QByteArray data = pattern(10000);
        client->write(data);
        QTRY_COMPARE_WITH_TIMEOUT(r.dev(1)->wire.size(), data.size(), 10000);
        QCOMPARE(r.dev(1)->wire, data);
        QTRY_COMPARE_WITH_TIMEOUT(r.status().bytesToSerial, qint64(data.size()), 5000);
    }

    void minusOneWriteEnds()
    {
        Rig r;
        r.world->configure = [](FakeSerialDevice& d) { d.writeFails = true; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        client->write("X");
        QVERIFY(waitClosedByPeer(*client));
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Listening, 5000);
        QVERIFY(r.status().lastError->startsWith(QStringLiteral("serial write failed")));
        QVERIFY(!r.dev(1) || !r.dev(1)->isOpen);
    }

    void bytesDuringOpenRetryAreForwarded()
    {
        Rig r;
        r.world->configure = [](FakeSerialDevice& d) {
            d.failOpens = 1;
            d.rxOnOpen = "HELLO";   // already waiting in the device when the open succeeds
        };
        r.bridge->start();
        auto client = connectTo(r.port());
        QVERIFY(waitConnected(*client));
        client->write("EARLY;");   // while the bridge is still Opening
        client->flush();
        QTRY_COMPARE_WITH_TIMEOUT(r.world->opens, 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(r.dev(1)->wire, QByteArray("EARLY;"), 5000);
        QCOMPARE(readExactly(*client, 5), QByteArray("HELLO"));
    }

    void deviceErrorRecordsLastError()
    {
        Rig r;
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        r.dev(1)->emitError(SerialError::Timeout, QStringLiteral("ignored"));
        r.dev(1)->emitError(SerialError::Resource, QStringLiteral("Input/output error"));
        QVERIFY(waitClosedByPeer(*client));
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Listening, 5000);
        QCOMPARE(r.status().lastError, std::optional<QString>(QStringLiteral("device error: Input/output error")));
        QVERIFY(!r.status().client.has_value());
    }

    void deviceErrorDuringOpeningIgnored()
    {
        Rig r;
        r.world->configure = [](FakeSerialDevice& d) { d.failOpens = 1; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.world->openCalls, 1, 5000);
        r.dev(1)->emitError(SerialError::NotFound, QStringLiteral("from the failed open"));
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        QVERIFY(!r.status().lastError.has_value());
    }

    void readMinusOneEnds()
    {
        Rig r;
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        r.dev(1)->readFails = true;
        r.dev(1)->feed("x");
        QVERIFY(waitClosedByPeer(*client));
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Listening, 5000);
        QVERIFY(r.status().lastError->startsWith(QStringLiteral("serial read failed")));
    }

    // ----------------------------------------------------------------------------------------------
    // INV-57: independent stall deadlines
    // ----------------------------------------------------------------------------------------------
    void stalledUpstreamWhileDownstreamFlows()
    {
        Rig r;   // stall 600 ms
        r.world->configure = [](FakeSerialDevice& d) { d.drainMode = FakeSerialDevice::Drain::Manual; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        client->write("A");   // the device never takes it
        QElapsedTimer t;
        t.start();
        QByteArray got;
        while (r.status().state == BridgeState::Connected && t.elapsed() < 5000) {
            if (FakeSerialDevice* d = r.dev(1); d && d->isOpen) d->feed("x");   // downstream keeps flowing
            QTest::qWait(50);
            got += client->readAll();
        }
        QCOMPARE(r.status().state, BridgeState::Listening);
        QVERIFY2(t.elapsed() < 2000, qPrintable(QString::number(t.elapsed())));
        QVERIFY(got.size() >= 5);   // the client did receive bytes all along
        QVERIFY2(r.status().lastError->startsWith(QStringLiteral("stalled: the serial side")), qPrintable(*r.status().lastError));
    }

    void stallArmedBelowCap()
    {
        Rig r;
        r.world->configure = [](FakeSerialDevice& d) { d.drainMode = FakeSerialDevice::Drain::Manual; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        Session* s = r.bridge->currentSession();
        client->write("1");   // one byte queued at the device, the source then empty
        QTRY_COMPARE_WITH_TIMEOUT(r.dev(1)->bytesToWrite(), qint64(1), 5000);
        QVERIFY(s->up->stallArmed());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Listening, 3000);
        QVERIFY(r.status().lastError->startsWith(QStringLiteral("stalled:")));
    }

    // Arrivals at the source are not destination progress: the session ends 5 s after output first
    // became pending, however much keeps arriving.
    void sourceArrivalsDoNotExtendStall()
    {
        Rig r(EngineTimings{});   // the real 5 s
        r.world->configure = [](FakeSerialDevice& d) { d.drainMode = FakeSerialDevice::Drain::Manual; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        client->write("a");
        QTRY_COMPARE_WITH_TIMEOUT(r.dev(1)->bytesToWrite(), qint64(1), 5000);
        QElapsedTimer t;
        t.start();
        while (r.status().state == BridgeState::Connected && t.elapsed() < 9000) {
            client->write("b");
            QTest::qWait(200);
        }
        QCOMPARE(r.status().state, BridgeState::Listening);
        QVERIFY2(t.elapsed() >= 4700 && t.elapsed() <= 6500, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(*r.status().lastError, QStringLiteral("stalled: the serial side accepted no data for 5 s"));
    }

    void progressRestartsTheDeadline()
    {
        Rig r;   // stall 600 ms
        r.world->configure = [](FakeSerialDevice& d) { d.drainMode = FakeSerialDevice::Drain::Manual; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        client->write(pattern(1000));
        QTRY_COMPARE_WITH_TIMEOUT(r.dev(1)->bytesToWrite(), qint64(1000), 5000);
        // Slow but steady progress: 10 bytes every 150 ms keeps the session alive well past 600 ms.
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 1500) {
            r.dev(1)->drain(10);
            QTest::qWait(150);
            QCOMPARE(r.status().state, BridgeState::Connected);
        }
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Listening, 3000);   // progress stopped
    }

    // ----------------------------------------------------------------------------------------------
    // INV-80: queued entry, single liveness predicate, owner detached first
    // ----------------------------------------------------------------------------------------------
    void synchronousEmissionFromFakeWriteIsQueued()
    {
        Rig r;
        bool handlerRanInsideWrite = false;
        bool hookRan = false;
        r.world->configure = [&](FakeSerialDevice& d) {
            d.drainMode = FakeSerialDevice::Drain::InsideWrite;
            d.afterEmitInsideWrite = [&] {
                hookRan = true;
                // The pump's bytesWritten handler counts the bytes; it must not have run yet.
                if (r.bridge->snapshot().bytesToSerial > 0) handlerRanInsideWrite = true;
            };
        };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        client->write("ABC");
        QTRY_VERIFY_WITH_TIMEOUT(hookRan, 5000);
        QVERIFY(!handlerRanInsideWrite);
        QTRY_COMPARE_WITH_TIMEOUT(r.status().bytesToSerial, qint64(3), 5000);   // ran later, from the event loop
        QCOMPARE(r.dev(1)->wire, QByteArray("ABC"));
    }

    void staleQueuedDeliveryIgnored()
    {
        Rig r;
        r.world->configure = [](FakeSerialDevice& d) { d.drainMode = FakeSerialDevice::Drain::Manual; };
        r.bridge->start();
        auto first = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        first->write("AB");
        QTRY_COMPARE_WITH_TIMEOUT(r.dev(1)->bytesToWrite(), qint64(2), 5000);
        QPointer<Pump> oldUp = r.bridge->currentSession()->up;
        QVERIFY(oldUp->stallArmed());
        const int oldWrites = r.dev(1)->writeCalls;

        // The old device reports progress: the delivery is queued, not yet run...
        r.dev(1)->drain();
        // ...and before it runs, a new client replaces the session (newConnection runs synchronously).
        auto second = connectTo(r.port());
        QVERIFY(second->waitForConnected(5000));   // the client side only; no event loop runs
        QVERIFY(r.bridge->server().waitForNewConnection(5000));
        QVERIFY(r.bridge->currentSession() != nullptr);
        QVERIFY(r.bridge->currentSession()->up == nullptr);   // the new one is still Opening
        QVERIFY(!oldUp.isNull());
        QVERIFY(!oldUp->stallArmed());   // stopped by endSession

        QCoreApplication::processEvents();   // the stale delivery runs now
        QCOMPARE(r.status().bytesToSerial, qint64(0));
        if (FakeSerialDevice* d1 = r.dev(1)) QCOMPARE(d1->writeCalls, oldWrites);
        if (!oldUp.isNull()) QVERIFY(!oldUp->stallArmed());
        QTRY_COMPARE_WITH_TIMEOUT(r.world->opens, 2, 5000);
        QCOMPARE(r.status().bytesToSerial, qint64(0));
    }

    void currentClearedOnDisconnect()
    {
        Rig r;
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        QVERIFY(r.bridge->currentSession() != nullptr);
        client->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() == nullptr, 5000);
        QCOMPARE(r.status().state, BridgeState::Listening);
        QCOMPARE(r.side.count(QStringLiteral("disconnected")), 1);
    }

    // The m_inPump guard itself (§8.5.1: redundant while every connection is queued), by a direct call.
    void reentrantPumpCallIsDeferred()
    {
        Rig r;
        r.world->configure = [](FakeSerialDevice& d) { d.drainMode = FakeSerialDevice::Drain::InsideWrite; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        Pump* up = r.bridge->currentSession()->up;
        int depth = 0, maxDepth = 0;
        r.dev(1)->afterEmitInsideWrite = [&] {
            ++depth;
            maxDepth = std::max(maxDepth, depth);
            // Re-entry: must only set m_again. Called from the outermost write only, so a pump without
            // the guard shows up as depth 2 instead of recursing without end.
            if (depth == 1) up->pump();
            --depth;
        };
        client->write("xyz");
        QTRY_COMPARE_WITH_TIMEOUT(r.dev(1)->wire, QByteArray("xyz"), 5000);
        QCOMPARE(maxDepth, 1);
    }

    // INV-80 / INV-3: two clients accepted in one newConnection batch. The first session is replaced
    // before its queued first open attempt runs, so that attempt finds the session still allocated
    // (QPointer non-null, deleteLater not yet processed) but no longer current: isLive must refuse it,
    // and device 1 is never opened.
    void sameBatchReplacementNeverOpensTheOldDevice()
    {
        Rig r;
        r.bridge->start();
        QTcpSocket a, b;
        a.connectToHost(QHostAddress::LocalHost, r.port());
        QVERIFY(a.waitForConnected(5000));   // the client side only; the bridge has not accepted yet
        b.connectToHost(QHostAddress::LocalHost, r.port());
        QVERIFY(b.waitForConnected(5000));
        QVERIFY(r.bridge->server().waitForNewConnection(5000));   // accepts both, synchronously
        QCOMPARE(r.world->created, 2);
        QCOMPARE(r.world->openCalls, 0);
        QTRY_COMPARE_WITH_TIMEOUT(r.world->opens, 1, 5000);
        QTest::qWait(100);
        QCOMPARE(r.world->openCalls, 1);
        QCOMPARE(r.world->events.filter(QStringLiteral("open")), QStringList{QStringLiteral("open 2")});
        QCOMPARE(r.status().state, BridgeState::Connected);
        QCOMPARE(r.status().client, std::optional<QString>(QStringLiteral("127.0.0.1:%1").arg(b.localPort())));
    }

    // ----------------------------------------------------------------------------------------------
    // Client EOF: a client that sends and closes at once (printf 'PS0;' | nc -q0 host port)
    // still gets its bytes to the device. The session drains (Draining) before it ends.
    // ----------------------------------------------------------------------------------------------
    void sendAndCloseReachesTheDevice()
    {
        Rig r;   // Drain::Auto
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        client->write("PS0;");
        client->disconnectFromHost();
        QTRY_COMPARE_WITH_TIMEOUT(r.world->wires.value(1), QByteArray("PS0;"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() == nullptr, 5000);
        QVERIFY(r.world->events.contains(QStringLiteral("close 1")));
        QCOMPARE(r.status().state, BridgeState::Listening);
        QVERIFY(!r.status().lastError.has_value());
    }

    // More than the buffers hold (socket read cap + pending suffix + device buffer), short writes:
    // all of it arrives, in order, after the client is gone.
    void bulkSendAndCloseArrivesWhole()
    {
        Rig r(noStall());
        r.world->configure = [](FakeSerialDevice& d) { d.maxWritePerCall = 1000; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        const QByteArray payload = pattern(256 * 1024);
        client->write(payload);
        client->disconnectFromHost();
        QTRY_COMPARE_WITH_TIMEOUT(r.world->wires.value(1).size(), payload.size(), 10000);
        QVERIFY(r.world->wires.value(1) == payload);
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() == nullptr, 5000);
        QVERIFY(!r.status().lastError.has_value());
    }

    // The drain itself: the device holds the bytes when the client's close is seen; the device stays
    // open (and the down direction stops) until they are written, then the session ends as ClientGone.
    void clientCloseDrainsBeforeClosingTheDevice()
    {
        Rig r(noStall());
        r.world->configure = [](FakeSerialDevice& d) { d.drainMode = FakeSerialDevice::Drain::Manual; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        client->write("PS0;");
        QTRY_COMPARE_WITH_TIMEOUT(r.dev(1)->bytesToWrite(), qint64(4), 5000);
        client->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession()
                                     && r.bridge->currentSession()->state == Session::Draining, 5000);
        QTest::qWait(100);
        QVERIFY(r.dev(1)->isOpen);
        QVERIFY(!r.world->events.contains(QStringLiteral("close 1")));
        QCOMPARE(r.side.count(QStringLiteral("closed its side")), 1);
        r.dev(1)->feed("RV02.16;");   // the down direction no longer reads
        QTest::qWait(50);
        QCOMPARE(r.dev(1)->bytesAvailable(), qint64(8));

        r.dev(1)->drain();
        QCOMPARE(r.dev(1)->wire, QByteArray("PS0;"));
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() == nullptr, 5000);
        QVERIFY(r.world->events.contains(QStringLiteral("close 1")));
        QCOMPARE(r.status().state, BridgeState::Listening);
        QVERIFY(!r.status().lastError.has_value());
        QCOMPARE(r.status().bytesToSerial, qint64(4));
        QCOMPARE(r.side.count(QStringLiteral("disconnected")), 1);
    }

    // A drain the device never takes is ended by the up direction's stall deadline (no new timer).
    void stalledDrainEndsByTheDeadline()
    {
        Rig r;   // stall 600 ms
        r.world->configure = [](FakeSerialDevice& d) { d.drainMode = FakeSerialDevice::Drain::Manual; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        client->write("PS0;");
        QTRY_COMPARE_WITH_TIMEOUT(r.dev(1)->bytesToWrite(), qint64(4), 5000);
        QElapsedTimer t;
        t.start();   // the up deadline was armed when the bytes became pending, just before this
        client->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession()
                                     && r.bridge->currentSession()->state == Session::Draining, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() == nullptr, 5000);
        QVERIFY2(t.elapsed() < 1500, qPrintable(QString::number(t.elapsed())));
        QVERIFY(r.world->events.contains(QStringLiteral("close 1")));
        QCOMPARE(r.status().state, BridgeState::Listening);
        QCOMPARE(r.status().lastError.value_or(QString()),
                 QStringLiteral("stalled: the serial side accepted no data for 600 ms"));
    }

    // A new client pre-empts a drain (INV-2: the draining device is closed before the new open).
    void replacementPreemptsADrain()
    {
        Rig r(noStall());
        r.world->configure = [](FakeSerialDevice& d) { d.drainMode = FakeSerialDevice::Drain::Manual; };
        r.bridge->start();
        auto first = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        first->write("PS0;");
        QTRY_COMPARE_WITH_TIMEOUT(r.dev(1)->bytesToWrite(), qint64(4), 5000);
        first->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession()
                                     && r.bridge->currentSession()->state == Session::Draining, 5000);
        auto second = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.world->opens, 2, 5000);
        const QStringList& ev = r.world->events;
        QVERIFY2(ev.indexOf(QStringLiteral("close 1")) >= 0
                     && ev.indexOf(QStringLiteral("close 1")) < ev.indexOf(QStringLiteral("open 2")),
                 qPrintable(ev.join(u',')));
        QCOMPARE(r.side.count(QStringLiteral("replacing client")), 1);
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        second->write("AN;");
        QTRY_COMPARE_WITH_TIMEOUT(r.dev(2)->bytesToWrite(), qint64(3), 5000);
        r.dev(2)->drain();
        QCOMPARE(r.dev(2)->wire, QByteArray("AN;"));
    }

    // stop() pre-empts a drain.
    void stopPreemptsADrain()
    {
        Rig r(noStall());
        r.world->configure = [](FakeSerialDevice& d) { d.drainMode = FakeSerialDevice::Drain::Manual; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        client->write("PS0;");
        QTRY_COMPARE_WITH_TIMEOUT(r.dev(1)->bytesToWrite(), qint64(4), 5000);
        client->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession()
                                     && r.bridge->currentSession()->state == Session::Draining, 5000);
        r.bridge->stop();
        QVERIFY(r.bridge->currentSession() == nullptr);
        QVERIFY(!r.dev(1)->isOpen);
        QCOMPARE(r.status().state, BridgeState::Stopped);
        QCOMPARE(r.side.count(QStringLiteral("bridge stopping")), 1);
    }

    // Nothing pending when the client closes: the session ends at once, no drain.
    void closeWithNothingPendingEndsAtOnce()
    {
        Rig r;
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        client->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() == nullptr, 5000);
        QCOMPARE(r.side.count(QStringLiteral("closed its side")), 0);
        QCOMPARE(r.side.count(QStringLiteral("disconnected")), 1);
    }

    // ----------------------------------------------------------------------------------------------
    // Native transmission: Qt's write buffer being empty does not mean the bytes left the
    // port. The driver or adapter can still hold them, and closing the port can cancel them, so the
    // drain also waits for the device's native output queue, bounded by the same stall deadline.
    // ----------------------------------------------------------------------------------------------

    // The driver holds the bytes when the client's close is seen (Qt's buffer already empty): the
    // device stays open until the queue is transmitted, then the session ends as ClientGone.
    void nativeOutputHoldsTheCloseUntilTransmitted()
    {
        Rig r(noStall());
        r.world->configure = [](FakeSerialDevice& d) { d.nativeStage = true; };   // Drain::Auto above it
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        client->write("PS0;");
        QTRY_COMPARE_WITH_TIMEOUT(r.dev(1)->nativeOutputPending(), std::optional<qint64>(4), 5000);
        QCOMPARE(r.dev(1)->bytesToWrite(), qint64(0));   // Qt's buffer is empty: only the driver holds them
        client->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession()
                                     && r.bridge->currentSession()->state == Session::Draining, 5000);
        QTest::qWait(150);   // many polls
        QVERIFY2(r.dev(1) && r.dev(1)->isOpen, qPrintable(r.side.lines.join(u'\n')));
        QVERIFY(!r.world->events.contains(QStringLiteral("close 1")));
        r.dev(1)->transmit(2);
        QTest::qWait(100);
        QVERIFY2(r.dev(1) && r.dev(1)->isOpen, qPrintable(r.side.lines.join(u'\n')));   // 2 bytes still queued
        r.dev(1)->transmit();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() == nullptr, 5000);
        QCOMPARE(r.world->wires.value(1), QByteArray("PS0;"));
        QCOMPARE(r.world->lostOnClose.value(1), qint64(0));
        QVERIFY(r.world->events.contains(QStringLiteral("close 1")));
        QCOMPARE(r.status().state, BridgeState::Listening);
        QVERIFY(!r.status().lastError.has_value());
        QCOMPARE(r.side.count(QStringLiteral("closed its side")), 1);
        QCOMPARE(r.side.count(QStringLiteral("disconnected")), 1);
    }

    // The driver never transmits: the up stall deadline ends the drain, and the loss is reported (the
    // lastError and a warning name the bytes still queued), never silent.
    void nativeOutputStuckEndsStalledAndReportsTheLoss()
    {
        Rig r;   // stall 600 ms
        r.world->configure = [](FakeSerialDevice& d) { d.nativeStage = true; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        client->write("PS0;");
        QTRY_COMPARE_WITH_TIMEOUT(r.dev(1)->nativeOutputPending(), std::optional<qint64>(4), 5000);
        const QString endpoint = QStringLiteral("127.0.0.1:%1").arg(client->localPort());
        QElapsedTimer t;
        t.start();   // the deadline was restarted by the device's bytesWritten, just before this
        client->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession()
                                     && r.bridge->currentSession()->state == Session::Draining, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() == nullptr, 5000);
        QVERIFY2(t.elapsed() < 1500, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(r.world->lostOnClose.value(1), qint64(4));
        QVERIFY(r.world->wires.value(1).isEmpty());
        QCOMPARE(r.status().state, BridgeState::Listening);
        const QString why = QStringLiteral(
            "stalled: the serial device did not finish transmitting (4 bytes still queued, no progress for 600 ms)");
        QCOMPARE(r.status().lastError.value_or(QString()), why);
        QVERIFY2(r.side.lines.contains(QStringLiteral("W test: client %1 disconnected: %2").arg(endpoint, why)),
                 qPrintable(r.side.lines.join(u'\n')));
    }

    // A slow device: the queue shrinks, but more slowly than the deadline. A shrinking queue is
    // progress (bytes left the port), so the drain runs to the end instead of being cut as stalled.
    void nativeProgressKeepsTheDrainAlive()
    {
        EngineTimings timings = fast();
        timings.stallTimeout = 1500ms;
        Rig r(timings);
        r.world->configure = [](FakeSerialDevice& d) { d.nativeStage = true; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        client->write("PS0;");
        QTRY_COMPARE_WITH_TIMEOUT(r.dev(1)->nativeOutputPending(), std::optional<qint64>(4), 5000);
        client->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession()
                                     && r.bridge->currentSession()->state == Session::Draining, 5000);
        for (int i = 0; i < 4; ++i) {   // one byte every 500 ms: 2 s in all, past the 1.5 s deadline
            QTest::qWait(500);
            QVERIFY2(r.bridge->currentSession(), qPrintable(r.status().lastError.value_or(QString())));
            r.dev(1)->transmit(1);
        }
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() == nullptr, 5000);
        QCOMPARE(r.world->wires.value(1), QByteArray("PS0;"));
        QCOMPARE(r.world->lostOnClose.value(1), qint64(0));
        QVERIFY(!r.status().lastError.has_value());
    }

    // A driver that misreports its output queue (virtual COM, Bluetooth SPP) as non-empty forever.
    // A client that sent nothing never waits on that queue: its orderly close ends the session at once,
    // as ClientGone and without an error.
    void misreportedNativeQueueDoesNotHoldAClientThatSentNothing()
    {
        Rig r;   // stall 600 ms: a wait on the false count would end as an error at 600 ms
        r.world->configure = [](FakeSerialDevice& d) { d.nativeMisreport = 7; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        QCOMPARE(r.dev(1)->nativeOutputPending(), std::optional<qint64>(7));
        QElapsedTimer t;
        t.start();
        client->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() == nullptr, 5000);
        QVERIFY2(t.elapsed() < 400, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(r.status().state, BridgeState::Listening);
        QVERIFY2(!r.status().lastError.has_value(), qPrintable(r.status().lastError.value_or(QString())));
        QVERIFY(r.world->events.contains(QStringLiteral("close 1")));
        QCOMPARE(r.side.count(QStringLiteral("disconnected")), 1);
        QCOMPARE(r.side.count(QStringLiteral("did not finish transmitting")), 0);
    }

    // The same driver, but the client did send bytes: the native queue is then the bridge's to wait for,
    // so the drain holds the close and, the queue never moving, ends Stalled with the loss reported.
    void misreportedNativeQueueStillDrainsAClientThatSentBytes()
    {
        Rig r;   // stall 600 ms
        r.world->configure = [](FakeSerialDevice& d) { d.nativeMisreport = 7; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        client->write("PS0;");
        QTRY_COMPARE_WITH_TIMEOUT(r.dev(1)->wire, QByteArray("PS0;"), 5000);
        client->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession()
                                     && r.bridge->currentSession()->state == Session::Draining, 5000);
        QTest::qWait(150);
        QVERIFY(r.dev(1)->isOpen);   // held: the queue still reads non-zero
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() == nullptr, 5000);
        QCOMPARE(r.status().lastError.value_or(QString()),
                 QStringLiteral("stalled: the serial device did not finish transmitting "
                                "(7 bytes still queued, no progress for 600 ms)"));
    }

    // A device that cannot report its native queue: the drain ends once Qt's buffer is empty (the last
    // thing the bridge can see), as before; it never waits on an unknown queue.
    void unknownNativeQueueEndsWithQtsBuffer()
    {
        Rig r(noStall());
        r.world->configure = [](FakeSerialDevice& d) {
            d.nativeStage = true;
            d.nativeUnknown = true;
        };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        client->write("PS0;");
        client->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() == nullptr, 5000);
        QCOMPARE(r.world->lostOnClose.value(1), qint64(4));   // what such a driver may discard: not visible here
        QCOMPARE(r.status().state, BridgeState::Listening);
        QVERIFY(!r.status().lastError.has_value());
    }

    // ----------------------------------------------------------------------------------------------
    // A client that closes while the session is still Opening. With bytes to deliver the open retries go on and the
    // bytes reach the device; with nothing to deliver the retries are cancelled at once (INV-3).
    // ----------------------------------------------------------------------------------------------
    void openingCloseWithBytesDrainsAfterTheRetry()
    {
        Rig r(slowRetry());
        r.world->configure = [](FakeSerialDevice& d) { d.failOpens = 1; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QVERIFY(waitConnected(*client));
        QTRY_COMPARE_WITH_TIMEOUT(r.world->openCalls, 1, 5000);
        client->write("PS0;");
        client->disconnectFromHost();   // before the retry
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() && r.bridge->currentSession()->clientClosed, 5000);
        QCOMPARE(r.bridge->currentSession()->state, Session::Opening);
        QCOMPARE(r.world->opens, 0);
        QTRY_COMPARE_WITH_TIMEOUT(r.world->wires.value(1), QByteArray("PS0;"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() == nullptr, 5000);
        QCOMPARE(r.world->openCalls, 2);
        QVERIFY(r.world->events.contains(QStringLiteral("close 1")));
        QCOMPARE(r.status().state, BridgeState::Listening);
        QVERIFY(!r.status().lastError.has_value());
        QCOMPARE(r.status().bytesToSerial, qint64(4));
        QCOMPARE(r.side.count(QStringLiteral("closed its side")), 1);
        QCOMPARE(r.side.count(QStringLiteral("disconnected")), 1);
        QCOMPARE(r.side.count(QStringLiteral("connected to")), 0);   // the client was already gone
    }

    // The bytes do not change the outcome of the open: every attempt fails → OpenRefused, as today.
    void openingCloseWithBytesStillRefusedWhenEveryOpenFails()
    {
        Rig r(slowRetry());
        r.world->failNextOpens = 100;
        r.bridge->start();
        auto client = connectTo(r.port());
        QVERIFY(waitConnected(*client));
        QTRY_COMPARE_WITH_TIMEOUT(r.world->openCalls, 1, 5000);
        client->write("PS0;");
        client->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() && r.bridge->currentSession()->clientClosed, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() == nullptr, 10000);
        QCOMPARE(r.world->openCalls, 3);
        QCOMPARE(r.world->opens, 0);
        QVERIFY(r.world->wires.value(1).isEmpty());
        QCOMPARE(r.status().state, BridgeState::Listening);
        QCOMPARE(r.status().lastError, std::optional<QString>(QStringLiteral("Could not open COM3: Access to the port is denied.")));
        QCOMPARE(r.side.count(QStringLiteral("refused, could not open")), 1);
        QCOMPARE(r.side.count(QStringLiteral("closed its side")), 0);
        QCOMPARE(r.side.count(QStringLiteral("disconnected")), 0);
    }

    // INV-3: a close with nothing to deliver still cancels the retries at once (an orderly FIN; the
    // abort() case is disconnectDuringRetryCancels).
    void emptyCloseDuringOpeningCancelsAtOnce()
    {
        Rig r(slowRetry());
        r.world->configure = [](FakeSerialDevice& d) { d.failOpens = 2; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QVERIFY(waitConnected(*client));
        QTRY_COMPARE_WITH_TIMEOUT(r.world->openCalls, 1, 5000);
        client->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() == nullptr, 5000);
        QCOMPARE(r.world->openCalls, 1);
        QTest::qWait(1300);   // past the next attempt
        QCOMPARE(r.world->openCalls, 1);
        QCOMPARE(r.world->opens, 0);
        QCOMPARE(r.status().state, BridgeState::Listening);
        QCOMPARE(r.side.count(QStringLiteral("closed its side")), 0);
        QCOMPARE(r.side.count(QStringLiteral("disconnected")), 1);
    }

    // A new client pre-empts a session waiting to deliver a closed client's bytes (INV-2, INV-3): the
    // old device is never opened, although its retry would have succeeded.
    void replacementPreemptsAnOpeningDrain()
    {
        Rig r(slowRetry());
        r.world->configure = [](FakeSerialDevice& d) {
            if (d.id == 1) d.failOpens = 1;
        };
        r.bridge->start();
        auto first = connectTo(r.port());
        QVERIFY(waitConnected(*first));
        QTRY_COMPARE_WITH_TIMEOUT(r.world->openCalls, 1, 5000);
        first->write("PS0;");
        first->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() && r.bridge->currentSession()->clientClosed, 5000);
        auto second = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.world->opens, 1, 5000);
        QTest::qWait(1300);   // past the old session's retry
        const QStringList& ev = r.world->events;
        QVERIFY2(!ev.contains(QStringLiteral("open 1")), qPrintable(ev.join(u',')));
        QVERIFY2(ev.contains(QStringLiteral("open 2")), qPrintable(ev.join(u',')));
        QCOMPARE(r.world->openCalls, 2);
        QVERIFY(r.world->wires.value(1).isEmpty());
        QCOMPARE(r.side.count(QStringLiteral("replacing client")), 1);
        QCOMPARE(r.side.count(QStringLiteral("closed its side")), 0);
        QCOMPARE(r.status().state, BridgeState::Connected);
        second->write("AN;");
        QTRY_COMPARE_WITH_TIMEOUT(r.world->wires.value(2), QByteArray("AN;"), 5000);
    }

    void stopPreemptsAnOpeningDrain()
    {
        Rig r(slowRetry());
        r.world->configure = [](FakeSerialDevice& d) { d.failOpens = 1; };
        r.bridge->start();
        auto client = connectTo(r.port());
        QVERIFY(waitConnected(*client));
        QTRY_COMPARE_WITH_TIMEOUT(r.world->openCalls, 1, 5000);
        client->write("PS0;");
        client->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(r.bridge->currentSession() && r.bridge->currentSession()->clientClosed, 5000);
        r.bridge->stop();
        QVERIFY(r.bridge->currentSession() == nullptr);
        QTest::qWait(1300);
        QCOMPARE(r.world->openCalls, 1);
        QCOMPARE(r.world->opens, 0);
        QCOMPARE(r.status().state, BridgeState::Stopped);
        QCOMPARE(r.side.count(QStringLiteral("bridge stopping")), 1);
    }

    // A device's open notice (a baud rate within tolerance) is logged once, at Info, for the open
    // that succeeded (not for the refused attempt before it).
    void openNoticeLoggedOnceAtInfo()
    {
        Rig r;
        r.world->configure = [](FakeSerialDevice& d) {
            d.failOpens = 1;
            d.openNoticeText = QStringLiteral("baud 250000 requested, device reports 249600");
        };
        r.bridge->start();
        auto client = connectTo(r.port());
        QTRY_COMPARE_WITH_TIMEOUT(r.status().state, BridgeState::Connected, 5000);
        QTest::qWait(50);
        QCOMPARE(r.side.count(QStringLiteral("device reports 249600")), 1);
        QVERIFY2(r.side.lines.contains(QStringLiteral("I test: COM3: baud 250000 requested, device reports 249600")),
                 qPrintable(r.side.lines.join(u'\n')));
    }

    // ----------------------------------------------------------------------------------------------
    // INV-6: a blocked side channel never stops bytes (the real SideChannel, its sink blocked).
    // ----------------------------------------------------------------------------------------------
    void blockedSideChannelDoesNotStopBytes()
    {
        auto* sink = new BlockingSink(3s);
        SideChannel side(std::unique_ptr<LogSink>(sink), {});
        auto world = std::make_shared<FakeSerialWorld>();
        QElapsedTimer t;
        t.start();
        PortBridge bridge(mapping(), fakeSerialFactory(world), side, fast());
        bridge.start();   // logs "listening": the sink blocks on it
        QVERIFY(sink->waitEntered(1, 2s));
        auto client = connectTo(bridge.server().serverPort());
        QTRY_COMPARE_WITH_TIMEOUT(bridge.snapshot().state, BridgeState::Connected, 2000);
        client->write("RV;");
        QTRY_COMPARE_WITH_TIMEOUT(world->last()->wire, QByteArray("RV;"), 2000);
        world->last()->feed("RV02.16;");
        QCOMPARE(readExactly(*client, 8, 2000), QByteArray("RV02.16;"));
        QVERIFY2(t.elapsed() < 2500, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(sink->receivedLines().size(), 0);   // still blocked on the first line
        sink->release();
        QVERIFY(side.flush(5s));
        QVERIFY(sink->receivedLines().size() >= 2);
    }
};

QTEST_GUILESS_MAIN(tst_PortBridge)
#include "tst_PortBridge.moc"
