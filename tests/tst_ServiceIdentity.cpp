// The Linux --service activation identity (WS-8, D41, INV-100): exactly one line,
// "0::/system.slice/aether-serial-bridge.service". Pure fixtures of /proc/self/cgroup.

#include "platform/posix/ServiceIdentity.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace asb::posix;

class tst_ServiceIdentity : public QObject {
    Q_OBJECT

private slots:
    void parsesOnlyExactV2Line_data()
    {
        QTest::addColumn<QByteArray>("content");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("the managed unit") << QByteArray("0::/system.slice/aether-serial-bridge.service\n") << true;
        QTest::newRow("the managed unit, no newline") << QByteArray("0::/system.slice/aether-serial-bridge.service") << true;
        QTest::newRow("cgroup v1 / hybrid (more than one line)")
            << QByteArray("12:pids:/system.slice/aether-serial-bridge.service\n"
                          "1:name=systemd:/system.slice/aether-serial-bridge.service\n"
                          "0::/system.slice/aether-serial-bridge.service\n")
            << false;
        QTest::newRow("v1 only (no 0:: line)") << QByteArray("1:name=systemd:/system.slice/aether-serial-bridge.service\n") << false;
        QTest::newRow("a user unit")
            << QByteArray("0::/user.slice/user-1000.slice/user@1000.service/app.slice/aether-serial-bridge.service\n") << false;
        QTest::newRow("another system unit") << QByteArray("0::/system.slice/run-u5.service\n") << false;
        QTest::newRow("a systemd-run --scope scope") << QByteArray("0::/system.slice/run-r4463.scope\n") << false;
        QTest::newRow("a sub-cgroup of the unit") << QByteArray("0::/system.slice/aether-serial-bridge.service/sub\n") << false;
        QTest::newRow("a look-alike unit") << QByteArray("0::/system.slice/aether-serial-bridge.service2\n") << false;
        QTest::newRow("a nested slice") << QByteArray("0::/system.slice/x.slice/aether-serial-bridge.service\n") << false;
        QTest::newRow("a rebased path (nested cgroup namespace)") << QByteArray("0::/\n") << false;
        QTest::newRow("init.scope (docker exec)") << QByteArray("0::/init.scope\n") << false;
        QTest::newRow("trailing blank") << QByteArray("0::/system.slice/aether-serial-bridge.service \n") << false;
        QTest::newRow("two newlines") << QByteArray("0::/system.slice/aether-serial-bridge.service\n\n") << false;
        QTest::newRow("leading line") << QByteArray("\n0::/system.slice/aether-serial-bridge.service\n") << false;
        QTest::newRow("empty") << QByteArray() << false;
    }
    void parsesOnlyExactV2Line()
    {
        QFETCH(QByteArray, content);
        QFETCH(bool, accepted);
        QCOMPARE(ServiceIdentity::isManagedUnit(content), accepted);
        QTemporaryDir dir;
        const QString path = dir.path() + QStringLiteral("/cgroup");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(content);
        f.close();
        QString why;
        const QByteArray p = QFile::encodeName(path);
        QCOMPARE(ServiceIdentity::check(&why, p.constData()), accepted);
        if (!accepted) QVERIFY(!why.isEmpty());
    }
    void unreadableIsARefusal()
    {
        QString why;
        QVERIFY(!ServiceIdentity::check(&why, "/nonexistent/cgroup"));
        QVERIFY(!why.isEmpty());
    }
    // This test process is not the managed unit.
    void thisProcessIsRefused()
    {
        QString why;
        QVERIFY(!ServiceIdentity::check(&why));
    }
};

QTEST_GUILESS_MAIN(tst_ServiceIdentity)
#include "tst_ServiceIdentity.moc"
