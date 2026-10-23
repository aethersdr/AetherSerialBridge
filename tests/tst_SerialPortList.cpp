// --list-ports on Linux (WS-8; INV-32).
// Fixture directories stand in for /dev/serial/by-id and /dev. The process-level proof that nothing is
// opened (strace) is the systemd suite's list_ports_strace.

#include "platform/posix/SerialPortList.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#include <unistd.h>

using namespace asb::posix;

namespace {
void touch(const QString& p)
{
    QFile f(p);
    QVERIFY(f.open(QIODevice::WriteOnly));
}
void link(const QString& target, const QString& at)
{
    QVERIFY(::symlink(QFile::encodeName(target).constData(), QFile::encodeName(at).constData()) == 0);
}
}  // namespace

class tst_SerialPortList : public QObject {
    Q_OBJECT

private slots:
    void nothingFound()
    {
        QTemporaryDir d;
        QString out;
        QTextStream s(&out);
        SerialPortList::print(s, d.path() + QStringLiteral("/missing"), d.path());
        QCOMPARE(out, QStringLiteral("No USB serial adapters found. Plug one in and run --list-ports again.\n"));
    }
    void stableAndKernelNames()
    {
        QTemporaryDir d;
        const QString dev = d.path() + QStringLiteral("/dev"), byId = dev + QStringLiteral("/serial/by-id");
        QVERIFY(QDir().mkpath(byId));
        for (const char* n : {"ttyUSB10", "ttyUSB2", "ttyACM0", "ttyS0", "ttyUSBx", "tty"}) touch(dev + QLatin1Char('/') + QLatin1String(n));
        link(QStringLiteral("../../ttyUSB2"), byId + QStringLiteral("/usb-FTDI_B-if00-port0"));
        link(QStringLiteral("../../ttyACM0"), byId + QStringLiteral("/usb-Arduino_A-if00"));
        link(QStringLiteral("../../ttyGONE"), byId + QStringLiteral("/usb-Gone-if00"));
        touch(byId + QStringLiteral("/usb-Plain"));   // not a link: printed "(dangling)"
        QString out;
        QTextStream s(&out);
        SerialPortList::print(s, byId, dev);
        const QString expected = QStringLiteral(
            "Stable names (use these in --add-port):\n"
            "  %1/usb-Arduino_A-if00 -> %2/ttyACM0\n"
            "  %1/usb-FTDI_B-if00-port0 -> %2/ttyUSB2\n"
            "  %1/usb-Gone-if00 -> %2/ttyGONE\n"
            "  %1/usb-Plain -> (dangling)\n"
            "Kernel names (can change after a reboot or replug):\n"
            "  %2/ttyACM0\n"
            "  %2/ttyUSB2\n"
            "  %2/ttyUSB10\n").arg(byId, dev);
        QCOMPARE(out, expected);
    }
    void kernelNamesWithoutStableNames()
    {
        QTemporaryDir d;
        touch(d.path() + QStringLiteral("/ttyUSB0"));
        QString out;
        QTextStream s(&out);
        SerialPortList::print(s, d.path() + QStringLiteral("/none"), d.path());
        QVERIFY(out.contains(QStringLiteral("Stable names (use these in --add-port):\n  none\n")));
        QVERIFY(out.contains(QStringLiteral("  %1/ttyUSB0\n").arg(d.path())));
    }
    // A FIFO named like a port: listing it must not open it (an open would block here).
    void listingNeverOpensAnEntry()
    {
        QTemporaryDir d;
        QVERIFY(::mkfifo(QFile::encodeName(d.path() + QStringLiteral("/ttyUSB0")).constData(), 0600) == 0);
        QVERIFY(QDir().mkpath(d.path() + QStringLiteral("/by-id")));
        link(d.path() + QStringLiteral("/ttyUSB0"), d.path() + QStringLiteral("/by-id/usb-X"));
        QString out;
        QTextStream s(&out);
        SerialPortList::print(s, d.path() + QStringLiteral("/by-id"), d.path());
        QVERIFY(out.contains(QStringLiteral(" -> %1/ttyUSB0").arg(d.path())));
    }
};

QTEST_GUILESS_MAIN(tst_SerialPortList)
#include "tst_SerialPortList.moc"
