// StatusPrinter: each port with its detail.

#include "core/StatusPrinter.h"

#include <QTest>

using namespace asb;

class tst_StatusPrinter : public QObject {
    Q_OBJECT

private slots:
    // Kills: the shared printer dropping a field --status used to show.
    void statusPrinterShowsEachPortWithItsDetail()
    {
        StatusReport report;
        report.updated = QDateTime::currentDateTimeUtc();
        report.pid = 42;
        report.configError = QStringLiteral("bad thing");
        PortStatus a;
        a.name = QStringLiteral("KAT500");
        a.comPort = QStringLiteral("COM3");
        a.bindAddress = QStringLiteral("192.168.1.50");
        a.tcpPort = 4001;
        a.state = BridgeState::Connected;
        a.client = QStringLiteral("192.168.1.20:5555");
        a.bytesToSerial = 3;
        a.bytesToClient = 8;
        PortStatus b;
        b.name = QStringLiteral("KPA500");
        b.comPort = QStringLiteral("/dev/serial/by-id/usb-FTDI_X-if00-port0");
        b.bindAddress = QStringLiteral("0.0.0.0");
        b.tcpPort = 4002;
        b.state = BridgeState::Error;
        b.lastError = QStringLiteral("Could not open");
        report.ports = {a, b};

        const QString text = StatusPrinter::format(&report);
        QVERIFY(text.contains(QStringLiteral("(pid 42)")));
        QVERIFY(text.contains(QStringLiteral("Config error: bad thing")));
        QVERIFY(text.contains(QStringLiteral("  KAT500           COM3")));
        QVERIFY(text.contains(QStringLiteral("in=3 out=8 192.168.1.20:5555")));
        QVERIFY(text.contains(QStringLiteral("Could not open")));

        QCOMPARE(StatusPrinter::format(nullptr), QStringLiteral("No status reported yet.\n"));
    }

    // The exact layout: "  <name, 16> <device, padded> <bind>:<tcp, 5>  <state, 9> in=.. out=.. <detail>"
    void exactLayout()
    {
        StatusReport report;
        report.updated = QDateTime(QDate(2026, 10, 14), QTime(1, 2, 3));   // local time
        report.pid = 7;
        PortStatus a;
        a.name = QStringLiteral("Amp");
        a.comPort = QStringLiteral("COM3");
        a.bindAddress = QStringLiteral("0.0.0.0");
        a.tcpPort = 4001;
        a.state = BridgeState::Listening;
        a.lastError = QStringLiteral("not shown while Listening");   // detail only for Connected/Error
        PortStatus b = a;
        b.comPort = QStringLiteral("/dev/ttyUSB0");   // widens the column from 6 to 12
        b.state = BridgeState::Stopped;
        report.ports = {a, b};
        QCOMPARE(StatusPrinter::format(&report),
                 QStringLiteral("Reported: 2026-10-14 01:02:03 (pid 7)\n"
                                "  Amp              COM3         0.0.0.0:4001   Listening in=0 out=0 \n"
                                "  Amp              /dev/ttyUSB0 0.0.0.0:4001   Stopped   in=0 out=0 \n"));
    }

    void longFieldsAreNotTruncated()
    {
        StatusReport report;
        PortStatus a;
        a.name = QStringLiteral("A-very-long-port-name-indeed");
        a.comPort = QStringLiteral("COM3");
        a.tcpPort = 65535;
        a.state = BridgeState::Connected;
        report.ports = {a};
        const QString text = StatusPrinter::format(&report);
        QVERIFY(text.contains(QStringLiteral("  A-very-long-port-name-indeed COM3   :65535  Connected in=0 out=0 \n")));
    }
};

QTEST_GUILESS_MAIN(tst_StatusPrinter)
#include "tst_StatusPrinter.moc"
