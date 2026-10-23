// The config editor (WS-9): what --add-port, --remove-port and
// --set-enabled change, what they refuse (nothing changed), what they say, the format parser and --show-config's
// lines. Every accepted change is validated as a whole config.

#include "cli/ConfigEditor.h"

#include <QTest>

using namespace asb;
using namespace asb::cli;

namespace {

AddPortArgs add(const QString& name, const QString& device = QStringLiteral("/dev/ttyUSB0"), int tcp = 4001)
{
    AddPortArgs a;
    a.name = name;
    a.device = device;
    a.tcpPort = tcp;
    return a;
}

template <class F>
QString refusal(F&& f)
{
    try {
        f();
    } catch (const EditRefused& e) {
        return QString::fromStdString(e.what());
    }
    return {};
}

}  // namespace

class tst_ConfigEditor : public QObject {
    Q_OBJECT

private slots:
    void parseFormat_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("bits");
        QTest::addColumn<int>("parity");
        QTest::addColumn<int>("stop");
        QTest::newRow("8N1") << "8N1" << 8 << int(Parity::None) << int(StopBits::One);
        QTest::newRow("7e2") << "7e2" << 7 << int(Parity::Even) << int(StopBits::Two);
        QTest::newRow("5O1.5") << "5O1.5" << 5 << int(Parity::Odd) << int(StopBits::OnePointFive);
        QTest::newRow("8M1") << "8M1" << 8 << int(Parity::Mark) << int(StopBits::One);
        QTest::newRow("6s2") << "6s2" << 6 << int(Parity::Space) << int(StopBits::Two);
    }
    void parseFormat()
    {
        QFETCH(QString, text);
        QFETCH(int, bits);
        QFETCH(int, parity);
        QFETCH(int, stop);
        const ConfigEditor::Format f = ConfigEditor::parseFormat(text);
        QCOMPARE(f.dataBits, bits);
        QCOMPARE(int(f.parity), parity);
        QCOMPARE(int(f.stopBits), stop);
    }
    void parseFormatRefuses()
    {
        for (const char* bad : {"9N1", "8X1", "8N3", "8N", "", "8N1 ", "4N1", "\xEF\xBC\x98N1"})
            QVERIFY2(refusal([&] { ConfigEditor::parseFormat(QString::fromUtf8(bad)); }).contains("should look like 8N1"), bad);
    }
    void addPortSaysWhatItDid()
    {
        BridgeConfig c;
        QStringList said;
        ConfigEditor::addPort(c, add(QStringLiteral("KAT500")), &said);
        QCOMPARE(c.ports.size(), 1);
        const PortMapping& p = c.ports[0];
        QCOMPARE(p.name, QStringLiteral("KAT500"));
        QCOMPARE(p.comPort, QStringLiteral("/dev/ttyUSB0"));
        QCOMPARE(p.baudRate, 38400);
        QCOMPARE(p.bindAddress, QStringLiteral("0.0.0.0"));
        QVERIFY(p.enabled);
        QCOMPARE(said.value(0), QStringLiteral("Added KAT500: /dev/ttyUSB0 at 38400 8N1 on 0.0.0.0:4001."));
        // No --bind: every interface, with a warning; a ttyUSB name: the by-id advice.
        QVERIFY(said.join('\n').contains("Warning: listening on every interface (0.0.0.0)"));
        QVERIFY(said.join('\n').contains("Prefer its /dev/serial/by-id/ name"));
    }
    void addPortWithBindHasNoWarning()
    {
        BridgeConfig c;
        QStringList said;
        AddPortArgs a = add(QStringLiteral("KPA"), QStringLiteral("/dev/serial/by-id/usb-KPA"), 4002);
        a.bind = QStringLiteral("192.168.1.10");
        a.format = QStringLiteral("7e2");
        a.baud = 9600;
        ConfigEditor::addPort(c, a, &said);
        QCOMPARE(c.ports[0].bindAddress, QStringLiteral("192.168.1.10"));
        QCOMPARE(c.ports[0].dataBits, 7);
        QCOMPARE(said, QStringList{QStringLiteral("Added KPA: /dev/serial/by-id/usb-KPA at 9600 7E2 on 192.168.1.10:4002.")});
    }
    // The warning follows the address's value, not whether --bind was given (WS-12 round 1 class audit): an explicit
    // wildcard in any accepted spelling listens on every interface too.
    void addPortWarnsForEveryWildcardSpelling_data()
    {
        QTest::addColumn<QString>("bind");
        QTest::addColumn<bool>("warns");
        QTest::newRow("0.0.0.0") << QStringLiteral("0.0.0.0") << true;
        QTest::newRow("zero-padded v4") << QStringLiteral("0.00.000.0") << true;
        QTest::newRow("::") << QStringLiteral("::") << true;
        QTest::newRow("expanded v6") << QStringLiteral("0:0:0:0:0:0:0:0") << true;
        QTest::newRow("v4-mapped any") << QStringLiteral("::ffff:0.0.0.0") << true;
        QTest::newRow("loopback v4") << QStringLiteral("127.0.0.1") << false;
        QTest::newRow("loopback v6") << QStringLiteral("::1") << false;
        QTest::newRow("lan") << QStringLiteral("192.168.1.10") << false;
    }
    void addPortWarnsForEveryWildcardSpelling()
    {
        QFETCH(QString, bind);
        QFETCH(bool, warns);
        BridgeConfig c;
        QStringList said;
        AddPortArgs a = add(QStringLiteral("KPA"), QStringLiteral("/dev/serial/by-id/usb-KPA"), 4002);
        a.bind = bind;
        ConfigEditor::addPort(c, a, &said);
        QCOMPARE(said.join('\n').contains(QStringLiteral("Warning: listening on every interface (%1)").arg(bind)), warns);
        QCOMPARE(listensOnEveryInterface(bind), warns);
    }
    void addPortRefusals()
    {
        BridgeConfig c;
        ConfigEditor::addPort(c, add(QStringLiteral("KAT500")), nullptr);
        const BridgeConfig before = c;
        // Duplicate name, ignoring ASCII case.
        QVERIFY(refusal([&] { ConfigEditor::addPort(c, add(QStringLiteral("kat500"), QStringLiteral("/dev/ttyUSB1"), 4002), nullptr); })
                    .contains("A port named 'kat500' already exists"));
        QVERIFY(refusal([&] { ConfigEditor::addPort(c, add(QStringLiteral("  ")), nullptr); }).contains("needs a name"));
        // Whole-config validation: the same TCP port twice, a port out of range, a bad bind address.
        QVERIFY(refusal([&] { ConfigEditor::addPort(c, add(QStringLiteral("B"), QStringLiteral("/dev/ttyUSB1"), 4001), nullptr); })
                    .startsWith("Not saved:"));
        QVERIFY(refusal([&] { ConfigEditor::addPort(c, add(QStringLiteral("C"), QStringLiteral("/dev/ttyUSB2"), 70000), nullptr); })
                    .startsWith("Not saved:"));
        AddPortArgs bad = add(QStringLiteral("D"), QStringLiteral("/dev/ttyUSB3"), 4003);
        bad.bind = QStringLiteral("10");
        QVERIFY(refusal([&] { ConfigEditor::addPort(c, bad, nullptr); }).contains("not an IP address"));
        QCOMPARE(c, before);   // nothing changed by any refusal
    }
    void removeAndSetEnabled()
    {
        BridgeConfig c;
        ConfigEditor::addPort(c, add(QStringLiteral("KAT500")), nullptr);
        ConfigEditor::addPort(c, add(QStringLiteral("KPA"), QStringLiteral("/dev/ttyUSB1"), 4002), nullptr);
        QStringList said;
        ConfigEditor::setEnabled(c, QStringLiteral("kpa"), false, &said);
        QVERIFY(!c.ports[1].enabled);
        QCOMPARE(said, QStringList{QStringLiteral("KPA is now disabled.")});
        said.clear();
        ConfigEditor::removePort(c, QStringLiteral("KAT500"), &said);
        QCOMPARE(c.ports.size(), 1);
        QCOMPARE(said, QStringList{QStringLiteral("Removed KAT500.")});
        const BridgeConfig before = c;
        QVERIFY(refusal([&] { ConfigEditor::removePort(c, QStringLiteral("nope"), nullptr); }).contains("No port named 'nope'"));
        QVERIFY(refusal([&] { ConfigEditor::setEnabled(c, QStringLiteral("nope"), true, nullptr); }).contains("See --show-config"));
        QCOMPARE(c, before);
    }
    void describeLines()
    {
        BridgeConfig c;
        QCOMPARE(ConfigEditor::describe(c),
                 QStringList{QStringLiteral("No ports yet. Add one with: aether-serial-bridge --add-port <name> <device> <tcp-port>")});
        QCOMPARE(ConfigEditor::describe(c, QStringLiteral("aether-serial-bridge.exe")),
                 QStringList{QStringLiteral("No ports yet. Add one with: aether-serial-bridge.exe --add-port <name> <device> <tcp-port>")});
        AddPortArgs a = add(QStringLiteral("KAT500"));
        a.allow = QStringLiteral("10.0.0.5");
        ConfigEditor::addPort(c, a, nullptr);
        c.ports[0].enabled = false;
        QCOMPARE(ConfigEditor::describe(c),
                 QStringList{QStringLiteral("  off  KAT500           /dev/ttyUSB0  38400 8N1  0.0.0.0:4001  allow=10.0.0.5")});
        c.ports[0].tcpPort = 0;   // a config made by hand: its problems are shown
        QVERIFY(ConfigEditor::describe(c).last().startsWith("Problem: "));
    }
    void deviceNotesNeedNoIo()
    {
        QCOMPARE(ConfigEditor::deviceNotes(QStringLiteral("/dev/ttyACM3")).size(), 1);
        QVERIFY(ConfigEditor::deviceNotes(QStringLiteral("/dev/serial/by-id/usb-x")).isEmpty());
        QVERIFY(ConfigEditor::deviceNotes(QStringLiteral("COM3")).isEmpty());
    }
};

QTEST_GUILESS_MAIN(tst_ConfigEditor)
#include "tst_ConfigEditor.moc"
