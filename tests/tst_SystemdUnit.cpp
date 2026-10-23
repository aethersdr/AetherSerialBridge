// The unit --install writes (WS-8): --service, no cache directory, the Qt header for detection.

#include "core/InstallationEvidence.h"
#include "platform/posix/SystemdUnit.h"

#include <QTest>

#include <stdexcept>

using namespace asb;
using namespace asb::posix;

class tst_SystemdUnit : public QObject {
    Q_OBJECT

    static QList<QByteArray> lines() { return SystemdUnit::render().split('\n'); }

private slots:
    void firstLineIsTheQtHeader()
    {
        QVERIFY(SystemdUnit::render().startsWith(InstallationEvidence::kQtUnitHeader));
    }
    void execStartRunsTheServiceMode()
    {
        QVERIFY(lines().contains("ExecStart=/usr/local/bin/aether-serial-bridge --service"));
        QVERIFY(SystemdUnit::render(QStringLiteral("/srv/x/asb")).contains("\nExecStart=/srv/x/asb --service\n"));
    }
    // The Documentation= link is the repository the build was configured with (ASB_REPO, one value for the code and
    // the docs; tools/set_repo.py moves both).
    void documentationLinksTheRepository()
    {
        QVERIFY(lines().contains(QByteArray("Documentation=https://github.com/" ASB_REPO)));
        QCOMPARE(SystemdUnit::render().count("Documentation="), 1);
    }
    void serviceSection_data()
    {
        QTest::addColumn<QByteArray>("line");
        for (const char* l : {"Type=simple", "Restart=on-failure", "RestartSec=5", "DynamicUser=yes",
                              "SupplementaryGroups=dialout", "ConfigurationDirectory=aether-serial-bridge",
                              "RuntimeDirectory=aether-serial-bridge", "NoNewPrivileges=yes", "ProtectSystem=strict",
                              "ProtectHome=yes", "PrivateTmp=yes", "WantedBy=multi-user.target",
                              "After=network-online.target", "Wants=network-online.target",
                              // v1.1 station fix: exit 6 (not activated) is clean, so a stopped bridge is not restarted
                              "SuccessExitStatus=6"})
            QTest::newRow(l) << QByteArray(l);
    }
    void serviceSection()
    {
        QFETCH(QByteArray, line);
        QVERIFY(lines().contains(line));
    }
    // A native binary unpacks nothing: no cache directory, and the service mode only.
    void unitHasNoCacheDirectoryOrRunMode()
    {
        const QByteArray u = SystemdUnit::render();
        QVERIFY(!u.contains("CacheDirectory"));
        QVERIFY(!u.contains("--run"));
    }
    // DynamicUser + StateDirectory would move /var/lib/aether-serial-bridge under /var/lib/private and chown
    // it to the dynamic user: the root-owned engine lock would no longer be trusted (D54).
    void noStateDirectory() { QVERIFY(!SystemdUnit::render().contains("StateDirectory")); }
    void badExecutablesRefused_data()
    {
        QTest::addColumn<QString>("exe");
        QTest::newRow("relative") << QStringLiteral("aether-serial-bridge");
        QTest::newRow("space") << QStringLiteral("/srv/a b/asb");
        QTest::newRow("percent") << QStringLiteral("/srv/%h/asb");
        QTest::newRow("tab") << QStringLiteral("/srv/a\tb");
    }
    void badExecutablesRefused()
    {
        QFETCH(QString, exe);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, SystemdUnit::render(exe));
    }
    // The rendered unit is what detection calls ours.
    void detectedAsCurrent()
    {
        InstallationEvidence::LinuxEvidence ev;
        ev.qtUnitPresent = true;
        ev.qtUnitContent = SystemdUnit::render();
        ev.binaryPresent = true;
        ev.lockFilesPresent = true;
        // This build's program (v1.1 INV-120: Current needs the digest).
        ev.thisBuildKnown = ev.programDigestOk = true;
        ev.installedSha256 = QString(64, QLatin1Char('a'));
        QCOMPARE(InstallationEvidence::classifyLinux(ev).state, InstallState::Current);
        ev.etcUnit = InstallationEvidence::LinuxEvidence::EtcUnit::MaskLink;
        QCOMPARE(InstallationEvidence::classifyLinux(ev).state, InstallState::Current);
    }
};

QTEST_GUILESS_MAIN(tst_SystemdUnit)
#include "tst_SystemdUnit.moc"
