// The session engine behind the transaction core's runner (WS-9 integration; WS-4 deferred rows INV-1, 2, 3, 9,
// 59): BridgeRunner (engine.lock, Service trust, the start token, config.json) drives a real engine::BridgeHost on
// real TCP sockets, with fake serial devices. What the runner publishes is what the engine does.

#include "core/BridgeRunner.h"
#include "core/ConfigFiles.h"
#include "core/ConfigJson.h"
#include "core/StartToken.h"
#include "engine/BridgeHost.h"
#include "support/EngineTestSupport.h"
#include "support/FakeSerialDevice.h"
#include "support/Fakes.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

using namespace asb;
using namespace asb::test;
using namespace std::chrono_literals;

namespace {

const FirewallGeneration kGen{QByteArrayLiteral("0123456789abcdef")};
const QByteArray kToken = QByteArrayLiteral("dddddddddddddddddddddddddddddddd");

quint16 freePort()
{
    QTcpServer s;
    if (!s.listen(QHostAddress::LocalHost, 0)) return 0;
    const quint16 p = s.serverPort();
    s.close();
    return p;
}

std::unique_ptr<QTcpSocket> connectTo(quint16 port)
{
    auto s = std::make_unique<QTcpSocket>();
    s->connectToHost(QHostAddress::LocalHost, port);
    return s;
}

bool waitClosedByPeer(QTcpSocket& s, int ms = 5000)
{
    QElapsedTimer t;
    t.start();
    while (s.state() != QAbstractSocket::UnconnectedState && t.elapsed() < ms) QTest::qWait(10);
    return s.state() == QAbstractSocket::UnconnectedState;
}

engine::EngineTimings timings()
{
    engine::EngineTimings t;
    t.listenRetryDelay = 100ms;
    t.stallTimeout = 600ms;
    return t;   // the real 250 ms open retry (INV-3)
}

// The managed engine as --service builds it, with fake platform seams and fake serial devices.
struct Station {
    FakeWorld world;
    FakeFolder folder{world};
    FakeService service{world};
    FakeEngineLock lock{world, service};
    FileStartToken token{folder, world.files};
    std::shared_ptr<FakeSerialWorld> serial = std::make_shared<FakeSerialWorld>();
    RecordingSideChannel side;
    engine::BridgeHost engine{fakeSerialFactory(serial), side, timings()};
    NoFirewallMonitor monitor;
    FakeWatchdog watchdog{world.log, world.clock};
    std::unique_ptr<BridgeRunner> runner;
    quint16 port = freePort();
    QByteArray config;

    Station()
    {
        world.checkLock = false;   // the service never holds apply.lock
        config = QStringLiteral(R"({"version":1,"ports":[{"enabled":true,"name":"Amp","comPort":"COM9","baudRate":38400,"tcpPort":%1,"bindAddress":"127.0.0.1","allowedClients":""}]})")
                     .arg(port)
                     .toUtf8();
        world.files.plantFile(QString::fromLatin1(ConfigFiles::kConfig), config, QStringLiteral("root"));
        world.files.plantFile(QString::fromLatin1(ConfigFiles::kStartToken), StartToken::format(kToken, kGen), QStringLiteral("root"));
        lock.plantPair(QByteArrayLiteral("5555555555555555aaaaaaaaaaaaaaaa"));
        runner = std::make_unique<BridgeRunner>(lock, token, folder, engine, side, monitor, watchdog,
                                                QByteArrayLiteral("0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f"));
    }
    PortStatus port0() const { return runner->report().ports.value(0); }
};

}  // namespace

class tst_RunnerIntegration : public QObject {
    Q_OBJECT

private slots:
    // The runner starts the real engine on the committed config; the published status carries the ACK fields
    // (the token, this process's pid, sha256 of config.json) and the engine's own port state. INV-1: no device is
    // opened before a client connects.
    void runnerStartsTheEngine()
    {
        Station s;
        QVERIFY(s.port != 0);
        QCOMPARE(s.runner->start(), RunnerStart::Ok);
        QCOMPARE(s.engine.bridges().size(), size_t(1));
        QTRY_VERIFY_WITH_TIMEOUT(s.engine.bridges()[0]->server().isListening(), 5000);
        QCOMPARE(s.engine.bridges()[0]->server().serverPort(), s.port);
        QCOMPARE(s.serial->openCalls, 0);
        QVERIFY(!s.side.published.isEmpty());
        const StatusReport& r = s.side.published.last();
        QCOMPARE(r.startToken, kToken);
        QCOMPARE(r.configSha256, sha256Hex(s.config));
        QCOMPARE(r.pid, qint64(QCoreApplication::applicationPid()));
        QVERIFY(!r.configError.has_value());
        QCOMPARE(r.ports.size(), 1);
        QCOMPARE(r.ports[0].state, BridgeState::Listening);
    }

    // INV-1: the device is open only while a client is connected; bytes flow both ways through the runner's engine.
    void deviceOpenOnlyWhileConnected()
    {
        Station s;
        QCOMPARE(s.runner->start(), RunnerStart::Ok);
        auto client = connectTo(s.port);
        QTRY_COMPARE_WITH_TIMEOUT(s.serial->opens, 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(s.port0().state, BridgeState::Connected, 5000);
        client->write("^SV;");
        QTRY_COMPARE_WITH_TIMEOUT(s.serial->last()->wire, QByteArray("^SV;"), 5000);
        s.serial->last()->feed("KAT500;");
        QTRY_VERIFY_WITH_TIMEOUT(client->bytesAvailable() >= 7, 5000);
        QCOMPARE(client->readAll(), QByteArray("KAT500;"));
        client->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(s.serial->events.contains(QStringLiteral("close 1")), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(s.port0().state, BridgeState::Listening, 5000);
    }

    // INV-2: a new client replaces the old one, and the old device is closed before the new one opens.
    void replacementClosesBeforeReopen()
    {
        Station s;
        QCOMPARE(s.runner->start(), RunnerStart::Ok);
        auto first = connectTo(s.port);
        QTRY_COMPARE_WITH_TIMEOUT(s.serial->opens, 1, 5000);
        auto second = connectTo(s.port);
        QTRY_COMPARE_WITH_TIMEOUT(s.serial->opens, 2, 5000);
        const qsizetype closed = s.serial->events.indexOf(QStringLiteral("close 1"));
        const qsizetype reopened = s.serial->events.indexOf(QStringLiteral("open 2"));
        QVERIFY2(closed >= 0 && reopened > closed, qPrintable(s.serial->events.join(", ")));
        QVERIFY(waitClosedByPeer(*first));
    }

    // INV-3 and INV-9: a busy device is tried exactly three times; the client is then refused and the port goes
    // back to Listening with lastError, in the runner's published status.
    void busyDeviceRefusedThenListening()
    {
        Station s;
        QCOMPARE(s.runner->start(), RunnerStart::Ok);
        s.serial->failNextOpens = 100;
        auto client = connectTo(s.port);
        QVERIFY(waitClosedByPeer(*client));
        QTRY_VERIFY_WITH_TIMEOUT(s.port0().lastError.has_value(), 5000);
        QTest::qWait(700);   // a fourth attempt would have come by now
        QCOMPARE(s.serial->openCalls, 3);
        QCOMPARE(s.port0().state, BridgeState::Listening);
        QVERIFY2(s.port0().lastError->startsWith(QStringLiteral("Could not open COM9: ")), qPrintable(*s.port0().lastError));
        s.runner->publishNow();
        QVERIFY(s.side.published.last().ports[0].lastError.has_value());
    }

    // INV-59: a device error ends the session with lastError and the port listens again.
    void deviceErrorRecordedInStatus()
    {
        Station s;
        QCOMPARE(s.runner->start(), RunnerStart::Ok);
        auto client = connectTo(s.port);
        QTRY_COMPARE_WITH_TIMEOUT(s.port0().state, BridgeState::Connected, 5000);
        s.serial->last()->emitError(engine::SerialError::Resource, QStringLiteral("Input/output error"));
        QVERIFY(waitClosedByPeer(*client));
        QTRY_COMPARE_WITH_TIMEOUT(s.port0().state, BridgeState::Listening, 5000);
        QCOMPARE(s.port0().lastError, std::optional<QString>(QStringLiteral("device error: Input/output error")));
    }

    // The runner's refusals keep the engine from ever listening: no token -> exit 6, nothing started.
    void noTokenNoListener()
    {
        Station s;
        s.world.files.eraseEntry(QString::fromLatin1(ConfigFiles::kStartToken));
        QCOMPARE(s.runner->start(), RunnerStart::NotActivated);
        QVERIFY(s.engine.bridges().empty());
        QTcpSocket probe;
        probe.connectToHost(QHostAddress::LocalHost, s.port);
        QVERIFY(!probe.waitForConnected(500));
    }
};

QTEST_GUILESS_MAIN(tst_RunnerIntegration)
#include "tst_RunnerIntegration.moc"
