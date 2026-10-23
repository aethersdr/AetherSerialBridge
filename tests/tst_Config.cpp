// INV-30 (the config file format and its three deliberate refusals), the validation rows, the writer
// format, the ConfigFiles reads, and the address parser. tests/data/config-v1.json is the golden file: what
// the writer produces for it, byte for byte.

#include "core/AllowedClients.h"
#include "core/BridgeConfig.h"
#include "core/ConfigFiles.h"
#include "core/ConfigJson.h"
#include "support/TestPrinters.h"

#include <QFile>
#include <QTest>

using namespace asb;

namespace {

QByteArray readData(const char* name)
{
    QFile f(QStringLiteral(ASB_TEST_DATA_DIR "/") + QLatin1String(name));
    if (!f.open(QIODevice::ReadOnly)) qFatal("cannot open test data %s", name);
    return f.readAll();
}

PortMapping good(const QString& com = QStringLiteral("COM3"), int tcp = 4001)
{
    PortMapping p;
    p.name = QStringLiteral("KAT500");
    p.comPort = com;
    p.tcpPort = tcp;
    p.bindAddress = QStringLiteral("192.168.1.50");
    return p;
}

QStringList errorsFor(const PortMapping& p)
{
    BridgeConfig c;
    c.ports = {p};
    return c.validate();
}

bool anyContains(const QStringList& errors, const QString& needle)
{
    return std::any_of(errors.cbegin(), errors.cend(), [&](const QString& e) { return e.contains(needle); });
}

const QString kNotAnAddress = QStringLiteral(
    "is not an IP address, CIDR block (10.0.0.0/8), address range (a-b) or a Windows keyword such as LocalSubnet.");
const QString kRtsXonXoff = QStringLiteral(
    "Amp: handshake RequestToSendXOnXOff (RTS/CTS and XON/XOFF together) is not supported; use RequestToSend or "
    "XOnXOff.");
QString badBind(const QString& b)
{
    return QStringLiteral("Amp: bind address '%1' is not an IP address.").arg(b);
}

}  // namespace

class tst_Config : public QObject {
    Q_OBJECT

private slots:
    // ---- INV-30: the config file ----------------------------------------------------------------
    void goldenConfigLoads();
    void integerEnumsAccepted();
    void enumNamesIgnoreCase();
    void missingFieldsDefault();
    void unknownPropertiesAndDisplayNameIgnored();
    void shorthandBindAddressRefused_data();
    void shorthandBindAddressRefused();
    void rtsXOnXOffRefused();
    void firewallKeywords();
    // ---- writer -------------------------------------------------------------------------------
    void writerFormatIsTwoSpaceNamesOnly();
    void configRoundTripsWithEnumsAsText();
    void writerRoundTripsEveryValue();
    void undefinedEnumValuesWrittenAsIntegers();
    void loneSurrogates();
    // ---- decoding: nothing between the bytes and validate() changes a value (class audit) -------
    void bomInsideStringsIsKept_data();
    void bomInsideStringsIsKept();
    void bomOnlyAllowListIsAFenceError();
    void bomPrefixedNameMatchesNoField_data();
    void bomPrefixedNameMatchesNoField();
    void invalidUtf8RefusedWhereRead_data();
    void invalidUtf8RefusedWhereRead();
    void ordinalIgnoreCase_data();
    void ordinalIgnoreCase();
    void enumNamesDoNotFoldNonAsciiOntoAscii();
    void duplicateComPortUsesOrdinalIgnoreCase();
    // ---- validation -----------------------------------------------------------------------------
    void validConfigHasNoErrors();
    void duplicateComPortIsRejectedCaseInsensitively();
    void duplicateTcpPortIsRejected();
    void badBindAddressIsRejected_data();
    void badBindAddressIsRejected();
    void tcpPortOutOfRangeIsRejected_data();
    void tcpPortOutOfRangeIsRejected();
    void namesThatCouldEscapeAQuotedStringAreRejected_data();
    void namesThatCouldEscapeAQuotedStringAreRejected();
    void reasonableAllowedClientsPass_data();
    void reasonableAllowedClientsPass();
    void allowedClientsWithQuotesAreRejected();
    void stopBitsNoneIsRejected();
    void missingConfigLoadsEmpty();
    void badDeviceNameMentionsLinuxPaths();
    // ---- ConfigFiles ----------------------------------------------------------------------------
    void configFilesReads();
    // ---- addresses ----------------------------------------------------------------------------
    void ipAddressParsing_data();
    void ipAddressParsing();
    void sha256Hex();
};

void tst_Config::goldenConfigLoads()
{
    QString err;
    const auto c = fromJson(readData("config-v1.json"), &err);
    QVERIFY2(c, qPrintable(err));
    QCOMPARE(c->version, 1);
    QCOMPARE(c->ports.size(), 5);

    PortMapping p0 = good();
    p0.bindAddress = QStringLiteral("192.168.1.20");
    QCOMPARE(c->ports[0], p0);

    PortMapping p1;
    p1.name = QStringLiteral("KPA500");
    p1.comPort = QStringLiteral("/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A10K1234-if00-port0");
    p1.baudRate = 9600;
    p1.dataBits = 7;
    p1.parity = Parity::Even;
    p1.stopBits = StopBits::Two;
    p1.handshake = Handshake::XOnXOff;
    p1.dtrEnable = p1.rtsEnable = true;
    p1.tcpPort = 4002;
    p1.bindAddress = QStringLiteral("0.0.0.0");
    p1.allowedClients = QStringLiteral("192.168.1.0/24, LocalSubnet");
    QCOMPARE(c->ports[1], p1);

    PortMapping p2;
    p2.enabled = false;
    p2.comPort = QStringLiteral("COM12");
    p2.baudRate = 115200;
    p2.parity = Parity::Odd;
    p2.stopBits = StopBits::OnePointFive;
    p2.handshake = Handshake::RequestToSend;
    p2.tcpPort = 4003;
    p2.bindAddress = QStringLiteral("::");
    p2.allowedClients = QStringLiteral("2001:db8::/32,10.0.0.1-10.0.0.9");
    QCOMPARE(c->ports[2], p2);
    QCOMPARE(c->ports[2].displayName(), QStringLiteral("COM12"));   // computed, never stored

    QCOMPARE(c->ports[3].name, QString::fromUtf8("Ampère tuner (2)"));
    QCOMPARE(c->ports[3].parity, Parity::Mark);
    QCOMPARE(c->ports[3].dataBits, 5);
    QCOMPARE(c->ports[3].bindAddress, QStringLiteral("fe80::1"));
    QCOMPARE(c->ports[4].parity, Parity::Space);
    QCOMPARE(c->ports[4].tcpPort, 65535);

    QCOMPARE(c->validate(), QStringList());
    // Not written back, and the result re-reads to the same config.
    QVERIFY(!toJson(*c).contains("displayName"));
    QCOMPARE(fromJson(toJson(*c), nullptr), c);
}

// Killing test for the INV-30 mutant "reject integer enums".
void tst_Config::integerEnumsAccepted()
{
    const QByteArray json = R"({"ports":[{"name":"A","comPort":"COM3","tcpPort":4001,"bindAddress":"127.0.0.1",
        "parity":2,"stopBits":3,"handshake":2}]})";
    QString err;
    const auto c = fromJson(json, &err);
    QVERIFY2(c, qPrintable(err));
    QCOMPARE(c->ports[0].parity, Parity::Even);
    QCOMPARE(c->ports[0].stopBits, StopBits::OnePointFive);
    QCOMPARE(c->ports[0].handshake, Handshake::RequestToSend);
    QCOMPARE(c->validate(), QStringList());

    // An undefined value loads too, and is then reported by validate.
    const auto bad = fromJson(R"({"ports":[{"name":"A","comPort":"COM3","tcpPort":4001,"bindAddress":"127.0.0.1",
        "parity":7}]})", &err);
    QVERIFY2(bad, qPrintable(err));
    QCOMPARE(int(bad->ports[0].parity), 7);
    QCOMPARE(bad->validate(), QStringList{QStringLiteral("A: unknown parity.")});
}

void tst_Config::enumNamesIgnoreCase()
{
    QString err;
    const auto c = fromJson(R"({"ports":[{"parity":"eVeN","stopBits":"onepointfive","handshake":"XONXOFF"}]})", &err);
    QVERIFY2(c, qPrintable(err));
    QCOMPARE(c->ports[0].parity, Parity::Even);
    QCOMPARE(c->ports[0].stopBits, StopBits::OnePointFive);
    QCOMPARE(c->ports[0].handshake, Handshake::XOnXOff);
    QVERIFY(!fromJson(R"({"ports":[{"parity":"Banana"}]})", &err));
    QVERIFY(err.contains(QStringLiteral("ports[0].parity")));
}

void tst_Config::missingFieldsDefault()
{
    QString err;
    const auto c = fromJson(R"({"ports":[{}]})", &err);
    QVERIFY2(c, qPrintable(err));
    QCOMPARE(c->version, 1);
    QCOMPARE(c->ports.size(), 1);
    QCOMPARE(c->ports[0], PortMapping{});
    const PortMapping& p = c->ports[0];
    QVERIFY(p.enabled);
    QCOMPARE(p.baudRate, 38400);
    QCOMPARE(p.dataBits, 8);
    QCOMPARE(p.parity, Parity::None);
    QCOMPARE(p.stopBits, StopBits::One);
    QCOMPARE(p.handshake, Handshake::None);
    QVERIFY(!p.dtrEnable && !p.rtsEnable);
    QCOMPARE(p.tcpPort, 0);
    QVERIFY(p.name.isEmpty() && p.comPort.isEmpty() && p.bindAddress.isEmpty() && p.allowedClients.isEmpty());

    const auto empty = fromJson("{}", &err);
    QVERIFY2(empty, qPrintable(err));
    QCOMPARE(*empty, BridgeConfig{});
}

void tst_Config::unknownPropertiesAndDisplayNameIgnored()
{
    QString err;
    const auto c = fromJson(R"({"x":1,"ports":[{"comPort":"COM3","displayName":"Shown","ComPort":"COM9",
        "nested":{"a":[1,2]}}]})", &err);
    QVERIFY2(c, qPrintable(err));
    QCOMPARE(c->ports[0].comPort, QStringLiteral("COM3"));   // exact camelCase only
    QCOMPARE(c->ports[0].name, QString());
    QCOMPARE(c->ports[0].displayName(), QStringLiteral("COM3"));
}

void tst_Config::shorthandBindAddressRefused_data()
{
    QTest::addColumn<QString>("bind");
    QTest::newRow("10") << QStringLiteral("10");
    QTest::newRow("1.2.3") << QStringLiteral("1.2.3");
    QTest::newRow("127.1") << QStringLiteral("127.1");
    QTest::newRow("hex") << QStringLiteral("0x0a.0.0.1");
    QTest::newRow("bracketed") << QStringLiteral("[::1]");
}

void tst_Config::shorthandBindAddressRefused()
{
    QFETCH(QString, bind);
    PortMapping p = good();
    p.name = QStringLiteral("Amp");
    p.bindAddress = bind;
    QCOMPARE(errorsFor(p), QStringList{badBind(bind)});
}

void tst_Config::rtsXOnXOffRefused()
{
    PortMapping p = good();
    p.name = QStringLiteral("Amp");
    p.handshake = Handshake::RequestToSendXOnXOff;
    QCOMPARE(errorsFor(p), QStringList{kRtsXonXoff});
    // Read from a file by name and by value alike.
    QString err;
    for (const QByteArray& hs : {QByteArray("\"RequestToSendXOnXOff\""), QByteArray("3")}) {
        const auto c = fromJson(R"({"ports":[{"name":"Amp","comPort":"COM3","tcpPort":4001,"bindAddress":"0.0.0.0",
            "handshake":)" + hs + "}]}", &err);
        QVERIFY2(c, qPrintable(err));
        QCOMPARE(c->validate(), QStringList{kRtsXonXoff});
    }
}

void tst_Config::firewallKeywords()
{
    // WS-7 pinned the accepted set from fw_keyword_matrix: every known keyword is stored by the provider, so these
    // load (the refusal message stays for a build that refuses one: AllowedClients' policy seam).
    for (const char* k : {"PlayToDevice", "LocalSubnet4", "LocalSubnet6", "playtodevice"}) {
        PortMapping p = good();
        p.name = QStringLiteral("Amp");
        p.allowedClients = QLatin1String(k);
        QCOMPARE(errorsFor(p), QStringList());
    }
    // Refused, never translated: PlayToTargets (the provider's spelling) is not a known keyword.
    PortMapping p = good();
    p.name = QStringLiteral("Amp");
    p.allowedClients = QStringLiteral("PlayToTargets");
    QCOMPARE(errorsFor(p), QStringList{QStringLiteral("Amp: allowed client 'PlayToTargets' ") + kNotAnAddress});
}

void tst_Config::writerFormatIsTwoSpaceNamesOnly()
{
    BridgeConfig c;
    PortMapping p = good();
    p.bindAddress = QStringLiteral("192.168.1.20");
    c.ports = {p};
    const QByteArray expected = R"({
  "version": 1,
  "ports": [
    {
      "enabled": true,
      "name": "KAT500",
      "comPort": "COM3",
      "baudRate": 38400,
      "dataBits": 8,
      "parity": "None",
      "stopBits": "One",
      "handshake": "None",
      "dtrEnable": false,
      "rtsEnable": false,
      "tcpPort": 4001,
      "bindAddress": "192.168.1.20",
      "allowedClients": ""
    }
  ]
})";
    QCOMPARE(toJson(c), expected);
    QCOMPARE(toJson(BridgeConfig{}), QByteArray("{\n  \"version\": 1,\n  \"ports\": []\n}"));
    // The golden file is byte-identical to what we write for it (non-ASCII as UTF-8, never escaped).
    const QByteArray golden = readData("config-v1.json");
    const auto loaded = fromJson(golden, nullptr);
    QVERIFY(loaded);
    QCOMPARE(toJson(*loaded), golden);
}

void tst_Config::configRoundTripsWithEnumsAsText()
{
    PortMapping p = good();
    p.parity = Parity::Even;
    p.stopBits = StopBits::Two;
    p.dtrEnable = true;
    BridgeConfig c;
    c.ports = {p};
    const QByteArray text = toJson(c);
    QVERIFY(text.contains("\"Even\""));
    QVERIFY(text.contains("\"Two\""));
    const auto back = fromJson(text, nullptr);
    QVERIFY(back);
    QCOMPARE(back->ports.size(), 1);
    QCOMPARE(back->ports[0].parity, Parity::Even);
    QCOMPARE(back->ports[0].stopBits, StopBits::Two);
    QVERIFY(back->ports[0].dtrEnable);
    QCOMPARE(back->ports[0].comPort, QStringLiteral("COM3"));
}

void tst_Config::writerRoundTripsEveryValue()
{
    BridgeConfig c;
    c.version = 3;
    PortMapping p;
    p.enabled = false;
    p.name = QString::fromUtf8("q\"uote \\ back\nnl\ttab \x01 é 𝐀 /");
    p.comPort = QStringLiteral("/dev/x");
    p.baudRate = -5;
    p.dataBits = 0;
    p.parity = Parity::Space;
    p.stopBits = StopBits::OnePointFive;
    p.handshake = Handshake::RequestToSendXOnXOff;
    p.dtrEnable = p.rtsEnable = true;
    p.tcpPort = 65536;
    p.bindAddress = QStringLiteral("::1");
    p.allowedClients = QStringLiteral("a, b");
    c.ports = {p, PortMapping{}};
    QString err;
    const auto back = fromJson(toJson(c), &err);
    QVERIFY2(back, qPrintable(err));
    QCOMPARE(*back, c);
}

// A lone surrogate is refused where the string is read, but not in a property the reader skips. The writer escapes one losslessly, so nothing is silently changed.
void tst_Config::loneSurrogates()
{
    QString err;
    QVERIFY(!fromJson(R"({"ports":[{"name":"a\uD800"}]})", &err));
    QVERIFY(err.contains(QStringLiteral("ports[0].name")));
    QVERIFY(fromJson(R"({"x":"\uDC00","y":{"\uD800":1},"ports":[]})", &err));   // never read: ignored
    // A property name the reader looks at (the root's, a port's) is unescaped, which fails on a lone surrogate.
    QVERIFY(!fromJson(R"({"\uD800":1,"ports":[]})", &err));
    QVERIFY(!fromJson(R"({"ports":[{"\uDC00":1}]})", &err));
    QVERIFY(fromJson(R"({"ports":[{"name":"\uD835\uDC00"}]})", &err));   // a pair is fine
    BridgeConfig c;
    c.ports = {PortMapping{}};
    c.ports[0].name = QStringLiteral("a") + QChar(0xD800);
    QVERIFY(toJson(c).contains("\\ud800"));
    QVERIFY(!fromJson(toJson(c), &err));
}

void tst_Config::undefinedEnumValuesWrittenAsIntegers()
{
    BridgeConfig c;
    PortMapping p;
    p.parity = Parity(9);
    c.ports = {p};
    const QByteArray text = toJson(c);
    QVERIFY(text.contains("\"parity\": 9,"));
    QCOMPARE(fromJson(text, nullptr).value_or(BridgeConfig{}), c);
}

void tst_Config::validConfigHasNoErrors()
{
    BridgeConfig c;
    c.ports = {good(), good(QStringLiteral("COM2"), 4003)};
    QCOMPARE(c.validate(), QStringList());
}

void tst_Config::duplicateComPortIsRejectedCaseInsensitively()
{
    BridgeConfig c;
    c.ports = {good(QStringLiteral("COM3"), 4001), good(QStringLiteral("com3"), 4002)};
    QVERIFY(anyContains(c.validate(), QStringLiteral("already used")));
}

void tst_Config::duplicateTcpPortIsRejected()
{
    BridgeConfig c;
    c.ports = {good(QStringLiteral("COM3"), 4001), good(QStringLiteral("COM2"), 4001)};
    QVERIFY(anyContains(c.validate(), QStringLiteral("TCP port 4001 is already used")));
}

void tst_Config::badBindAddressIsRejected_data()
{
    QTest::addColumn<QString>("bind");
    QTest::newRow("not-an-ip") << QStringLiteral("not-an-ip");
    QTest::newRow("empty") << QString();
}

void tst_Config::badBindAddressIsRejected()
{
    QFETCH(QString, bind);
    PortMapping p = good();
    p.bindAddress = bind;
    QVERIFY(anyContains(errorsFor(p), QStringLiteral("bind address")));
}

void tst_Config::tcpPortOutOfRangeIsRejected_data()
{
    QTest::addColumn<int>("port");
    QTest::newRow("0") << 0;
    QTest::newRow("70000") << 70000;
}

void tst_Config::tcpPortOutOfRangeIsRejected()
{
    QFETCH(int, port);
    PortMapping p = good();
    p.tcpPort = port;
    QVERIFY(anyContains(errorsFor(p), QStringLiteral("1-65535")));
}

void tst_Config::namesThatCouldEscapeAQuotedStringAreRejected_data()
{
    QTest::addColumn<QString>("name");
    QTest::newRow("quote") << QStringLiteral("KAT'500");
    QTest::newRow("double quote") << QStringLiteral("amp\"; rm");
    QTest::newRow("dollar") << QStringLiteral("$(evil)");
    QTest::newRow("backtick") << QStringLiteral("a`b");
    QTest::newRow("semicolon") << QStringLiteral("a;b");
}

void tst_Config::namesThatCouldEscapeAQuotedStringAreRejected()
{
    QFETCH(QString, name);
    PortMapping p = good();
    p.name = name;
    QVERIFY(anyContains(errorsFor(p), QStringLiteral("name may use")));
}

void tst_Config::reasonableAllowedClientsPass_data()
{
    QTest::addColumn<QString>("clients");
    QTest::newRow("cgnat") << QStringLiteral("100.64.0.0/10");
    QTest::newRow("two") << QStringLiteral("192.168.1.10, 192.168.1.11");
    QTest::newRow("LocalSubnet") << QStringLiteral("LocalSubnet");
    QTest::newRow("blank") << QString();
}

void tst_Config::reasonableAllowedClientsPass()
{
    QFETCH(QString, clients);
    PortMapping p = good();
    p.allowedClients = clients;
    QCOMPARE(errorsFor(p), QStringList());
}

void tst_Config::allowedClientsWithQuotesAreRejected()
{
    PortMapping p = good();
    p.allowedClients = QStringLiteral("1.2.3.4'; Remove-Item");
    QVERIFY(anyContains(errorsFor(p), QStringLiteral("allowed clients")));
}

void tst_Config::stopBitsNoneIsRejected()
{
    PortMapping p = good();
    p.stopBits = StopBits::None;
    QVERIFY(anyContains(errorsFor(p), QStringLiteral("stop bits")));
}

void tst_Config::missingConfigLoadsEmpty()
{
    const ConfigFiles::Loaded l = ConfigFiles::readCommitted([](const QString&, qint64) {
        return ConfigFiles::RawRead{ConfigFiles::RawRead::Status::Missing, {}, {}};
    });
    QVERIFY(!l.present);
    QVERIFY(l.config);
    QCOMPARE(*l.config, BridgeConfig{});
}

void tst_Config::badDeviceNameMentionsLinuxPaths()
{
    PortMapping p;
    p.name = QStringLiteral("x");
    p.comPort = QStringLiteral("ttyUSB0");
    p.tcpPort = 4001;
    p.bindAddress = QStringLiteral("127.0.0.1");
    QVERIFY(anyContains(errorsFor(p), QStringLiteral("/dev/serial/by-id/")));
    p.comPort = QStringLiteral("/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A10K1234-if00-port0");
    QCOMPARE(errorsFor(p), QStringList());
}

void tst_Config::configFilesReads()
{
    using ConfigFiles::RawRead;
    QString askedName;
    qint64 askedCap = 0;
    const auto reader = [&](RawRead r) {
        return [&, r](const QString& name, qint64 cap) {
            askedName = name;
            askedCap = cap;
            return r;
        };
    };
    const QByteArray good = R"({"ports":[]})";

    auto l = ConfigFiles::readCommitted(reader({RawRead::Status::Ok, good, {}}));
    QCOMPARE(askedName, QStringLiteral("config.json"));
    QCOMPARE(askedCap, ConfigFiles::kConfigMaxBytes);
    QVERIFY(l.present && l.config && l.error.isEmpty());
    QCOMPARE(l.bytes, good);

    l = ConfigFiles::readPending(reader({RawRead::Status::Missing, {}, {}}));
    QCOMPARE(askedName, QStringLiteral("config.pending.json"));
    QVERIFY(!l.present && !l.config && l.error.isEmpty());

    l = ConfigFiles::readCommitted(reader({RawRead::Status::Failed, {}, QStringLiteral("access denied")}));
    QVERIFY(l.present && !l.config);
    QCOMPARE(l.error, QStringLiteral("Could not read config.json: access denied"));

    l = ConfigFiles::readPending(reader({RawRead::Status::Ok, "{\"ports\":5}", {}}));
    QVERIFY(l.present && !l.config);
    QVERIFY(l.error.startsWith(QStringLiteral("Could not read config.pending.json: ports")));

    // A reader that ignored the cap is still caught.
    l = ConfigFiles::readCommitted(reader({RawRead::Status::Ok, QByteArray(ConfigFiles::kConfigMaxBytes + 1, ' '), {}}));
    QVERIFY(!l.config);
    QVERIFY(l.error.contains(QStringLiteral("larger than")));
}

void tst_Config::ipAddressParsing_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("canonical");   // empty: refused
    QTest::newRow("quad") << QStringLiteral("192.168.1.20") << QStringLiteral("192.168.1.20");
    QTest::newRow("zero") << QStringLiteral("0.0.0.0") << QStringLiteral("0.0.0.0");
    QTest::newRow("octal group") << QStringLiteral("010.0.0.1") << QStringLiteral("8.0.0.1");
    QTest::newRow("octal 077") << QStringLiteral("077.00.0.1") << QStringLiteral("63.0.0.1");
    QTest::newRow("bad octal") << QStringLiteral("08.0.0.1") << QString();
    QTest::newRow("256") << QStringLiteral("256.1.1.1") << QString();
    QTest::newRow("four digits") << QStringLiteral("0001.1.1.1") << QString();
    QTest::newRow("shorthand") << QStringLiteral("1.2.3") << QString();
    QTest::newRow("one number") << QStringLiteral("10") << QString();
    QTest::newRow("trailing dot") << QStringLiteral("1.2.3.4.") << QString();
    QTest::newRow("space") << QStringLiteral(" 1.2.3.4") << QString();
    QTest::newRow("non-ascii digit") << QString::fromUtf8("1.2.3.\xD9\xA3") << QString();
    QTest::newRow("v6 any") << QStringLiteral("::") << QStringLiteral("::");
    QTest::newRow("v6 full") << QStringLiteral("2001:0DB8:0000:0000:0000:0000:0000:0001") << QStringLiteral("2001:db8::1");
    QTest::newRow("v6 mapped") << QStringLiteral("::ffff:1.2.3.4") << QStringLiteral("::ffff:1.2.3.4");
    QTest::newRow("v6 scope") << QStringLiteral("fe80::1%3") << QStringLiteral("fe80::1%3");
    QTest::newRow("v6 bracketed") << QStringLiteral("[::1]") << QString();
    QTest::newRow("v6 space") << QStringLiteral("::1 ") << QString();
    QTest::newRow("v6 two ::") << QStringLiteral("1::2::3") << QString();
    QTest::newRow("v6 bad hex") << QStringLiteral("2001:db8::g") << QString();
    QTest::newRow("empty") << QString() << QString();
}

void tst_Config::ipAddressParsing()
{
    QFETCH(QString, input);
    QFETCH(QString, canonical);
    const auto a = parseIpAddress(input);
    if (canonical.isEmpty()) {
        QVERIFY2(!a, qPrintable(a ? toString(*a) : QString()));
    } else {
        QVERIFY(a);
        QCOMPARE(toString(*a), canonical);
    }
}

void tst_Config::sha256Hex()
{
    QCOMPARE(asb::sha256Hex(QByteArray()), QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

// ---- decoding -----------------------------------------------------------------------------------

namespace {

const QByteArray kBom("\xEF\xBB\xBF");

// A valid one-port config whose allowedClients member is `clients` (raw JSON string contents, bytes as
// given) followed by `extra` (raw members).
QByteArray portJson(const QByteArray& member, const QByteArray& extra = QByteArray())
{
    return R"({"version":1,"ports":[{"name":"Amp","comPort":"COM3","tcpPort":4001,"bindAddress":"0.0.0.0",)" +
           member + extra + "}]}";
}

}  // namespace

// A literal UTF-8 BOM inside a string (at its start, or at the start of the bytes after an
// escape) was dropped by the decoder. Only the document's BOM is skipped; string contents are exact.
void tst_Config::bomInsideStringsIsKept_data()
{
    QTest::addColumn<QByteArray>("contents");   // raw JSON string contents of allowedClients
    QTest::addColumn<QString>("expected");
    const QString bom(QChar(0xFEFF));
    QTest::newRow("literal alone") << kBom << bom;
    QTest::newRow("escaped alone") << QByteArray("\\uFEFF") << bom;
    QTest::newRow("literal leading") << kBom + "10.0.0.1" << bom + QStringLiteral("10.0.0.1");
    QTest::newRow("literal after an escape") << "\\u0031" + kBom + "0.0.0.1" << QStringLiteral("1") + bom + QStringLiteral("0.0.0.1");
    QTest::newRow("escaped after an escape") << QByteArray("\\u0031\\uFEFF0.0.0.1") << QStringLiteral("1") + bom + QStringLiteral("0.0.0.1");
    QTest::newRow("literal after \\n") << "\\n" + kBom << QStringLiteral("\n") + bom;
    QTest::newRow("literal twice") << kBom + kBom << bom + bom;
    QTest::newRow("literal in the middle") << "1" + kBom + "0" << QStringLiteral("1") + bom + QStringLiteral("0");
}

void tst_Config::bomInsideStringsIsKept()
{
    QFETCH(QByteArray, contents);
    QFETCH(QString, expected);
    QString err;
    const auto c = fromJson(portJson(R"("allowedClients":")" + contents + '"'), &err);
    QVERIFY2(c, qPrintable(err));
    QCOMPARE(c->ports.value(0).allowedClients, expected);
    // The same in a name, and as the whole document's BOM in front (skipped).
    const auto named = fromJson(kBom + R"({"ports":[{"name":")" + contents + R"("}]})", &err);
    QVERIFY2(named, qPrintable(err));
    QCOMPARE(named->ports.value(0).name, expected);
}

// The security consequence of the bug: an allow-list of just a literal U+FEFF read as "" (any address).
// It must stay a character-fence error, and tokens() must refuse it.
void tst_Config::bomOnlyAllowListIsAFenceError()
{
    QString err;
    const auto c = fromJson(portJson(R"("allowedClients":")" + kBom + '"'), &err);
    QVERIFY2(c, qPrintable(err));
    QCOMPARE(c->ports.value(0).allowedClients, QString(QChar(0xFEFF)));
    QCOMPARE(c->validate(),
             QStringList{QStringLiteral("Amp: allowed clients may only list IP addresses, ranges or LocalSubnet.")});
    QVERIFY(!AllowedClients::tokens(c->ports.value(0).allowedClients));
}

// A BOM-prefixed property name is an unknown name (ignored), never the field it would be without
// the BOM: it must not override an earlier restrictive allow-list (last wins) or the ports list.
void tst_Config::bomPrefixedNameMatchesNoField_data()
{
    QTest::addColumn<QByteArray>("json");
    QTest::addColumn<QString>("clients");
    QTest::addColumn<int>("ports");
    const QByteArray restricted = R"("allowedClients":"10.0.0.1")";
    QTest::newRow("literal") << portJson(restricted, ",\"" + kBom + R"(allowedClients":"")") << QStringLiteral("10.0.0.1") << 1;
    QTest::newRow("escaped") << portJson(restricted, R"(,"\uFEFFallowedClients":"")") << QStringLiteral("10.0.0.1") << 1;
    QTest::newRow("literal after an escape")
        << portJson(restricted, ",\"\\u0061" + kBom + R"(llowedClients":"")") << QStringLiteral("10.0.0.1") << 1;
    QTest::newRow("top-level ports") << QByteArray(R"({"version":1,"ports":[],")" + kBom + R"(ports":[{}]})")
                                     << QString() << 0;
}

void tst_Config::bomPrefixedNameMatchesNoField()
{
    QFETCH(QByteArray, json);
    QFETCH(QString, clients);
    QFETCH(int, ports);
    QString err;
    const auto c = fromJson(json, &err);
    QVERIFY2(c, qPrintable(err));
    QCOMPARE(c->ports.size(), qsizetype(ports));
    if (ports) QCOMPARE(c->ports[0].allowedClients, clients);
}

// Invalid UTF-8 is never replaced (U+FFFD) into a value that is read: such a value is refused.
// A value that is never read, or a property name, is not decoded, so it does not stop the load, and an
// undecodable name matches no field.
void tst_Config::invalidUtf8RefusedWhereRead_data()
{
    QTest::addColumn<QByteArray>("json");
    QTest::addColumn<bool>("loads");
    QTest::addColumn<QString>("clients");
    const QByteArray restricted = R"("allowedClients":"10.0.0.1")";
    for (const char* bad : {"\xC0\xAF", "\xC0\x80", "\xE0\x80\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xFF", "\xC3"}) {
        const QByteArray b(bad);
        QTest::newRow(qPrintable(QStringLiteral("read %1").arg(QString::fromLatin1(b.toHex()))))
            << portJson(R"("allowedClients":"10.0.0.1)" + b + '"') << false << QString();
        QTest::newRow(qPrintable(QStringLiteral("ignored %1").arg(QString::fromLatin1(b.toHex()))))
            << portJson(restricted, R"(,"foo":"A)" + b + '"') << true << QStringLiteral("10.0.0.1");
        QTest::newRow(qPrintable(QStringLiteral("name %1").arg(QString::fromLatin1(b.toHex()))))
            << portJson(restricted, R"(,"allowedClients)" + b + R"(":"")") << true << QStringLiteral("10.0.0.1");
    }
}

void tst_Config::invalidUtf8RefusedWhereRead()
{
    QFETCH(QByteArray, json);
    QFETCH(bool, loads);
    QFETCH(QString, clients);
    QString err;
    const auto c = fromJson(json, &err);
    QCOMPARE(bool(c), loads);
    if (!loads) {
        QVERIFY2(err.contains(QStringLiteral("ports[0].allowedClients")) && err.contains(QStringLiteral("invalid UTF-8")),
                 qPrintable(err));
        return;
    }
    QCOMPARE(c->ports.value(0).allowedClients, clients);
}

void tst_Config::ordinalIgnoreCase_data()
{
    QTest::addColumn<QString>("a");
    QTest::addColumn<QString>("b");
    QTest::addColumn<bool>("equal");
    QTest::newRow("ascii case") << QStringLiteral("LocalSubnet") << QStringLiteral("lOCALsUBNET") << true;
    QTest::newRow("length") << QStringLiteral("Any") << QStringLiteral("Any ") << false;
    QTest::newRow("kelvin is not K") << QStringLiteral("Mark") << QStringLiteral("Mar\u212A") << false;
    QTest::newRow("long s is not S") << QStringLiteral("Space") << QStringLiteral("\u017Fpace") << false;
    QTest::newRow("dotless i is not I") << QStringLiteral("I") << QStringLiteral("\u0131") << false;
    QTest::newRow("dotted I is not i") << QStringLiteral("i") << QStringLiteral("\u0130") << false;
    QTest::newRow("sharp s") << QStringLiteral("\u00DF") << QStringLiteral("\u1E9E") << false;
    QTest::newRow("final sigma") << QStringLiteral("\u03A3") << QStringLiteral("\u03C2") << true;
    QTest::newRow("latin-1") << QStringLiteral("\u00E8") << QStringLiteral("\u00C8") << true;
}

void tst_Config::ordinalIgnoreCase()
{
    QFETCH(QString, a);
    QFETCH(QString, b);
    QFETCH(bool, equal);
    QCOMPARE(equalsOrdinalIgnoreCase(a, b), equal);
    QCOMPARE(equalsOrdinalIgnoreCase(b, a), equal);
}

// Enum names fold ASCII case only: Qt::CaseInsensitive would have read these as Mark and Space.
void tst_Config::enumNamesDoNotFoldNonAsciiOntoAscii()
{
    QVERIFY(!fromJson(portJson(R"("parity":"Mar\u212A")"), nullptr));
    QVERIFY(!fromJson(portJson(R"("parity":"\u017Fpace")"), nullptr));
    QCOMPARE(parityFromName(QStringLiteral("mARK")), std::optional<Parity>(Parity::Mark));
}

// Duplicate serial ports compare with ordinal, ASCII-only case folding.
void tst_Config::duplicateComPortUsesOrdinalIgnoreCase()
{
    const auto dup = [](const QString& a, const QString& b) {
        BridgeConfig c;
        c.ports = {good(a, 4001), good(b, 4002)};
        return anyContains(c.validate(), QStringLiteral("already used"));
    };
    QVERIFY(dup(QStringLiteral("/dev/ttyUSB0"), QStringLiteral("/DEV/TTYusb0")));
    QVERIFY(dup(QStringLiteral("/dev/tty\u03A3"), QStringLiteral("/dev/tty\u03C2")));
    QVERIFY(!dup(QStringLiteral("/dev/ttyK"), QStringLiteral("/dev/tty\u212A")));
    QVERIFY(!dup(QStringLiteral("/dev/ttyS0"), QStringLiteral("/dev/tty\u017F0")));
}

QTEST_GUILESS_MAIN(tst_Config)
#include "tst_Config.moc"
