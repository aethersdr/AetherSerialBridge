// INV-90 (WS-9): the managed config.json is run only by --service under the service manager. Started any other way
// (here: directly, by this test), the executable's --service refuses with exit 2 before it reads anything — on
// Windows because the dispatcher connect fails (WS-7), on Linux because the process is not in the unit's exact
// cgroup (INV-100, WS-8). Killing test of the spec mutant "--service runs without the service manager".

#include <QProcess>
#include <QtTest>

class tst_ServiceHost : public QObject {
    Q_OBJECT

private slots:
    void serviceRequiresServiceManager()
    {
        const QString exe = qEnvironmentVariable("ASB_HEADLESS_EXE");
        QVERIFY(!exe.isEmpty());
        QProcess p;
        p.start(exe, {QStringLiteral("--service")});
        QVERIFY(p.waitForStarted(10000));
        if (!p.waitForFinished(30000)) {
            p.kill();
            p.waitForFinished(5000);
            QFAIL("--service kept running outside the service manager");
        }
        QCOMPARE(p.exitStatus(), QProcess::NormalExit);
        QCOMPARE(p.exitCode(), 2);
        const QString out = QString::fromLocal8Bit(p.readAllStandardOutput() + p.readAllStandardError());
        QVERIFY2(!out.contains(QLatin1String("listening")), qPrintable(out));
    }
};

QTEST_GUILESS_MAIN(tst_ServiceHost)
#include "tst_ServiceHost.moc"
