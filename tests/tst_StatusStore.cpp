// status.json: round trip of every field (the ACK fields of D25/D52 included), bounded and strict reads.

#include "core/StatusStore.h"
#include "support/FakeFileIo.h"
#include "support/TestPrinters.h"

#include <QRegularExpression>
#include <QTest>

using namespace asb;

namespace {

StatusReport sample()
{
    StatusReport r;
    r.updated = QDateTime::fromString(QStringLiteral("2026-10-14T04:05:06.789Z"), Qt::ISODateWithMs);
    r.startedAt = r.updated.addSecs(-60);
    r.pid = 4242;
    r.instanceId = "0123456789abcdef0123456789abcdef";
    r.startToken = "fedcba9876543210fedcba9876543210";
    r.configSha256 = QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    r.configError = QStringLiteral("Amp: TCP port must be 1-65535.");
    PortStatus p;
    p.name = QStringLiteral("KAT500");
    p.comPort = QStringLiteral("COM3");
    p.bindAddress = QStringLiteral("0.0.0.0");
    p.tcpPort = 4001;
    p.state = BridgeState::Connected;
    p.client = QStringLiteral("192.168.1.20:5555");
    p.bytesToSerial = 5'000'000'000LL;   // beyond 32 bits
    p.bytesToClient = 9'007'199'254'740'993LL;   // beyond a double's 53 bits
    p.since = r.startedAt;
    PortStatus q = p;
    q.state = BridgeState::Listening;
    q.client.reset();
    q.lastError = QStringLiteral("device error: unplugged");
    r.ports = {p, q};
    return r;
}

}  // namespace

class tst_StatusStore : public QObject {
    Q_OBJECT

private slots:
    void roundTripsEveryField()
    {
        const StatusReport r = sample();
        QString err;
        const auto back = StatusStore::fromJson(StatusStore::toJson(r), &err);
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(*back, r);
    }

    void absentOptionalsStayAbsent()
    {
        StatusReport r;
        r.pid = 1;
        const QByteArray json = StatusStore::toJson(r);
        QVERIFY(json.contains("\"configError\": null"));
        const auto back = StatusStore::fromJson(json);
        QVERIFY(back);
        QVERIFY(!back->configError);
        QCOMPARE(*back, r);
    }

    void malformedIsRejected_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::newRow("not json") << QByteArray("{");
        QTest::newRow("array") << QByteArray("[]");
        QTest::newRow("pid string") << QByteArray(R"({"pid":"1"})");
        QTest::newRow("pid fraction") << QByteArray(R"({"pid":1.5})");
        QTest::newRow("token number") << QByteArray(R"({"startToken":5})");
        QTest::newRow("bad time") << QByteArray(R"({"updated":"yesterday"})");
        QTest::newRow("ports object") << QByteArray(R"({"ports":{}})");
        QTest::newRow("unknown state") << QByteArray(R"({"ports":[{"state":"Melting"}]})");
        QTest::newRow("configError number") << QByteArray(R"({"configError":1})");
    }

    void malformedIsRejected()
    {
        QFETCH(QByteArray, json);
        QString err;
        QVERIFY(!StatusStore::fromJson(json, &err));
        QVERIFY(!err.isEmpty());
    }

    void oversizeIsRejected()
    {
        QByteArray big = StatusStore::toJson(StatusReport{});
        big.append(QByteArray(StatusStore::kStatusMaxBytes, ' '));
        QString err;
        QVERIFY(!StatusStore::fromJson(big, &err));
        QVERIFY(err.contains(QStringLiteral("larger than")));
    }

    void writeReplacesStatusJsonWithTheRuntimePolicy()
    {
        asb::test::FakeFileIo io;
        asb::test::FakeDir dir;
        const WriteResult w = StatusStore::write(io, dir, sample());
        QVERIFY(w.replaced && w.durable);
        QCOMPARE(io.names(), QStringList{QStringLiteral("status.json")});
        QCOMPARE(StatusStore::fromJson(io.contents(QStringLiteral("status.json"))).value_or(StatusReport{}), sample());
    }

    void instanceIdsAreRandom128BitHex()
    {
        static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{32}$"));
        const QByteArray a = StatusStore::newInstanceId();
        const QByteArray b = StatusStore::newInstanceId();
        QVERIFY(hex.match(QString::fromLatin1(a)).hasMatch());
        QVERIFY(a != b);
    }

    void stateNames()
    {
        for (BridgeState s : {BridgeState::Stopped, BridgeState::Listening, BridgeState::Connected, BridgeState::Error})
            QCOMPARE(bridgeStateFromName(enumName(s)), std::optional<BridgeState>(s));
        QCOMPARE(enumName(BridgeState::Listening), QStringLiteral("Listening"));
    }
};

QTEST_GUILESS_MAIN(tst_StatusStore)
#include "tst_StatusStore.moc"
