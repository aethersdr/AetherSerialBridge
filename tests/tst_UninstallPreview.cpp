// What an uninstall says goes down before it runs (WS-16, INV-125): the enabled ports of the committed config, the
// client connected to each, the settings folder and its trust, and an installed build that is not this one. Pure.

#include "core/ConfigJson.h"
#include "core/UninstallPreview.h"

#include <QTest>

using namespace asb;

namespace {

BridgeConfig twoPorts()
{
    BridgeConfig c;
    PortMapping a;
    a.name = QStringLiteral("Amp");
    a.comPort = QStringLiteral("COM3");
    a.tcpPort = 4001;
    PortMapping b;
    b.name = QStringLiteral("Tuner");
    b.comPort = QStringLiteral("COM4");
    b.tcpPort = 4002;
    PortMapping off;
    off.name = QStringLiteral("Spare");
    off.comPort = QStringLiteral("COM5");
    off.tcpPort = 4003;
    off.enabled = false;
    c.ports << a << b << off;
    return c;
}

StatusReport connectedOn4001()
{
    StatusReport s;
    PortStatus p;
    p.tcpPort = 4001;
    p.state = BridgeState::Connected;
    p.client = QStringLiteral("192.0.2.10:50000");
    PortStatus q;
    q.tcpPort = 4002;
    q.state = BridgeState::Listening;
    s.ports << p << q;
    return s;
}

Installation current()
{
    Installation i;
    i.state = InstallState::Current;
    return i;
}

OpenedFolder trusted()
{
    OpenedFolder o;
    o.usable = true;
    return o;
}

}  // namespace

class tst_UninstallPreview : public QObject {
    Q_OBJECT
private slots:
    void portsAndClients()
    {
        const core::UninstallPreview p =
            core::uninstallPreview(twoPorts(), connectedOn4001(), current(), trusted(), QStringLiteral("/etc/aether-serial-bridge"));
        QCOMPARE(p.portsGoingDown, (QStringList{QStringLiteral("port 4001 (Amp)"), QStringLiteral("port 4002 (Tuner)")}));
        QCOMPARE(p.connectedClients, QStringList{QStringLiteral("the client at 192.0.2.10:50000 on port 4001 (Amp)")});
        QCOMPARE(p.lines,
                 (QStringList{QStringLiteral("Stopping port 4001 (Amp): the client at 192.0.2.10:50000 is connected and will be "
                                             "disconnected."),
                              QStringLiteral("Stopping port 4002 (Tuner): no client is connected.")}));
        QVERIFY(p.installed);
        QVERIFY(p.settingsTrusted);
        QCOMPARE(p.settingsFolder, QStringLiteral("/etc/aether-serial-bridge"));
    }
    void noneConnected()
    {
        const core::UninstallPreview p = core::uninstallPreview(twoPorts(), std::nullopt, current(), trusted(), QString());
        QVERIFY(p.connectedClients.isEmpty());
        QCOMPARE(p.lines.size(), 2);
        for (const QString& l : p.lines) QVERIFY2(l.endsWith(QStringLiteral("no client is connected.")), qPrintable(l));
    }
    // Nothing installed: nothing goes down.
    void notInstalledStopsNothing()
    {
        const core::UninstallPreview p =
            core::uninstallPreview(twoPorts(), connectedOn4001(), Installation{}, trusted(), QString());
        QVERIFY(!p.installed);
        QVERIFY(p.portsGoingDown.isEmpty());
        QVERIFY(p.lines.isEmpty());
    }
    // An untrusted folder: the settings cannot be removed (and the caller says so).
    void untrustedFolderWording()
    {
        OpenedFolder o;
        o.error = QStringLiteral("an untrusted file is in it");
        const core::UninstallPreview p = core::uninstallPreview(twoPorts(), std::nullopt, current(), o, QStringLiteral("D"));
        QVERIFY(!p.settingsTrusted);
    }
    // A newer installed build: its version is carried (wording only), never this build's.
    void newerInstalledBuild()
    {
        Installation other;
        other.state = InstallState::IncompleteInstall;
        other.otherBuild = true;
        other.installedVersion = QStringLiteral("1.2.0");
        const core::UninstallPreview p = core::uninstallPreview(twoPorts(), std::nullopt, other, trusted(), QString());
        QCOMPARE(p.installedVersion, std::optional<QString>(QStringLiteral("1.2.0")));
        Installation sameBuild = current();
        sameBuild.installedVersion = QStringLiteral("1.2.0");
        const core::UninstallPreview same = core::uninstallPreview(twoPorts(), std::nullopt, sameBuild, trusted(), QString());
        QVERIFY(!same.installedVersion);
    }
};

QTEST_GUILESS_MAIN(tst_UninstallPreview)
#include "tst_UninstallPreview.moc"
