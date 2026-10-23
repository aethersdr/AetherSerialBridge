// The Linux process guard (WS-8, INV-110; INV-112's runtime half): each case runs asb_seccomp_probe, which
// installs the product's guard and then attempts one thing. Runs wherever seccomp is available (the build
// container and the CI runners; the systemd suite runs the same probe under PID 1, plus the QtDBus case).

#include <QProcess>
#include <QTest>

#include <csignal>

namespace {

struct Run {
    QProcess::ExitStatus status;
    int code;
    QString out;
};

Run probe(const QString& c)
{
    QProcess p;
    p.start(qEnvironmentVariable("ASB_SECCOMP_PROBE"), {c});
    if (!p.waitForFinished(30000)) return {QProcess::CrashExit, -1, QStringLiteral("timeout")};
    return {p.exitStatus(), p.exitCode(), QString::fromLocal8Bit(p.readAllStandardOutput() + p.readAllStandardError())};
}

}  // namespace

class tst_ProcessGuard : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(!qEnvironmentVariable("ASB_SECCOMP_PROBE").isEmpty()); }

    // The probe itself can tell allowed from denied.
    void controlWithoutTheGuardForks()
    {
        const Run r = probe(QStringLiteral("control-fork"));
        QVERIFY2(r.out.contains(QLatin1String("ALLOWED control-fork")), qPrintable(r.out));
    }

    void processCreationDenied_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("errnoName");
        QTest::newRow("applier_cannot_fork (libc fork = clone)") << "fork-libc" << "EPERM";
        QTest::newRow("applier_cannot_fork (fork system call)") << "fork-raw" << "EPERM";
        QTest::newRow("applier_cannot_vfork") << "vfork-raw" << "EPERM";
        QTest::newRow("applier_cannot_clone_process") << "clone-process" << "EPERM";
        QTest::newRow("clone3 is ENOSYS") << "clone3" << "ENOSYS";
        QTest::newRow("applier_cannot_exec") << "execve" << "EPERM";
        QTest::newRow("applier_cannot_execveat") << "execveat" << "EPERM";
    }
    void processCreationDenied()
    {
        QFETCH(QString, name);
        QFETCH(QString, errnoName);
        const Run r = probe(name);
        if (r.out.startsWith(QLatin1String("SKIP"))) QSKIP(qPrintable(r.out));
        QVERIFY2(r.status == QProcess::NormalExit && r.code == 0, qPrintable(r.out));
        QVERIFY2(r.out.contains(QStringLiteral("DENIED %1 %2").arg(name, errnoName)), qPrintable(r.out));
    }

    // i386 int 0x80 and x32 numbers: the process is killed (SIGSYS) before anything runs.
    void otherAbisKilled_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("i386_int80_killed") << "int80";
        QTest::newRow("x32_syscall_killed") << "x32";
    }
    void otherAbisKilled()
    {
        QFETCH(QString, name);
        const Run r = probe(name);
        if (r.out.startsWith(QLatin1String("SKIP"))) QSKIP(qPrintable(r.out));
        QVERIFY2(r.status == QProcess::CrashExit, qPrintable(QStringLiteral("not killed: %1").arg(r.out)));
        QCOMPARE(r.code, SIGSYS);
        QVERIFY2(!r.out.contains(QLatin1String("ALLOWED")), qPrintable(r.out));
    }

    void threads_still_work_under_filter()
    {
        const Run r = probe(QStringLiteral("threads"));
        QVERIFY2(r.status == QProcess::NormalExit && r.code == 0 && r.out.contains(QLatin1String("OK threads 11")), qPrintable(r.out));
    }

    // A positive seccomp() result (TSYNC: a thread could not be synchronised) and a failed install both end the
    // process with exit 5 before anything else.
    void guardFailuresExit5_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("tsync_failure_fatal") << "tsync-fake";
        QTest::newRow("filter_install_failure_exits_5") << "install-failure";
    }
    void guardFailuresExit5()
    {
        QFETCH(QString, name);
        const Run r = probe(name);
        QVERIFY2(r.status == QProcess::NormalExit && r.code == 5, qPrintable(QStringLiteral("exit %1: %2").arg(r.code).arg(r.out)));
        QVERIFY2(r.out.contains(QLatin1String("cannot install the process guard")), qPrintable(r.out));
        QVERIFY(!r.out.contains(QLatin1String("ALLOWED")));
    }
};

QTEST_GUILESS_MAIN(tst_ProcessGuard)
#include "tst_ProcessGuard.moc"
