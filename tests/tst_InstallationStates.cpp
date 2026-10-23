// Installation-state detection from read-only evidence (D33, INV-98): NotInstalled, IncompleteInstall,
// Current and Foreign on both OSes, from synthetic evidence. A service or unit this program did not write is
// Foreign, and it takes precedence over any coexisting artifact of ours.

#include "core/InstallationEvidence.h"

#include <QTest>

using namespace asb;
using namespace asb::InstallationEvidence;

namespace {

const QString kQtExe = QStringLiteral(R"(C:\Program Files\AetherSerialBridge\aether-serial-bridge.exe)");

}  // namespace

class tst_InstallationStates : public QObject {
    Q_OBJECT
private slots:
    void windowsCurrentIncompleteNotInstalledForeign();
    void windowsForeignServiceUnderOurName();
    void windowsOtherBuild_data();
    void windowsOtherBuild();
    void entryNeverClassified();
    void linuxForeignUnitDetectedByContent();
    void linuxForeignUnitTakesPrecedence();
    void linuxStates_data();
    void linuxStates();
    void programRemainsOnlyWithoutAService();
};

void tst_InstallationStates::windowsCurrentIncompleteNotInstalledForeign()
{
    WindowsEvidence e;
    QCOMPARE(classifyWindows(e).state, InstallState::NotInstalled);
    e.serviceExists = true;
    e.expectedQtExe = kQtExe;
    e.binPath = QStringLiteral("\"%1\" --service").arg(kQtExe);
    e.qtExeDigestOk = e.lockFilesPresent = true;
    QCOMPARE(classifyWindows(e).state, InstallState::Current);
    e.lockFilesPresent = false;
    QCOMPARE(classifyWindows(e).state, InstallState::IncompleteInstall);   // INV-98
    e.lockFilesPresent = true;
    e.qtExeDigestOk = false;
    QCOMPARE(classifyWindows(e).state, InstallState::IncompleteInstall);
    e.binPath = QStringLiteral(R"("C:\Elsewhere\aether-serial-bridge.exe" --service)");
    QCOMPARE(classifyWindows(e).state, InstallState::Foreign);
    e.binPath = QStringLiteral("\"%1\" --run").arg(kQtExe);
    QCOMPARE(classifyWindows(e).state, InstallState::Foreign);
}

// Kills: accepting any program registered under the service name. Only this program's installed exe, run with
// --service, is ours; anything else under "AetherSerialBridge" is Foreign, even with our exe and locks beside it.
void tst_InstallationStates::windowsForeignServiceUnderOurName()
{
    WindowsEvidence e;
    e.serviceExists = true;
    e.expectedQtExe = kQtExe;
    e.qtExeDigestOk = e.lockFilesPresent = true;
    e.binPath = QStringLiteral(R"("C:\Program Files\AetherSerialBridge\bridge.exe" --service)");
    QCOMPARE(classifyWindows(e).state, InstallState::Foreign);
    e.binPath = QStringLiteral(R"("C:\Tools\Other.exe" --service)");
    const Installation i = classifyWindows(e);
    QCOMPARE(i.state, InstallState::Foreign);
    QVERIFY2(i.detail.contains(QStringLiteral("Other.exe")), qPrintable(i.detail));
}

// A user report (2026-10-16): the rule that decides "ask before replacing the installed program" against
// "finish the install without asking". Only a complete install of another build is otherBuild: the program was read
// whole (its digest is known) and is not this build, the lock files are there and the SCM configuration is what
// --install writes. Each condition has a row that fails it.
void tst_InstallationStates::windowsOtherBuild_data()
{
    QTest::addColumn<bool>("digestOk");
    QTest::addColumn<QString>("installedSha256");   // empty: the program could not be read whole
    QTest::addColumn<bool>("locks");
    QTest::addColumn<QString>("scmProblem");
    QTest::addColumn<int>("state");
    QTest::addColumn<bool>("otherBuild");
    QTest::addColumn<QString>("detail");
    const QString other = QStringLiteral("117eb29a0b1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f708192a3b4c5d6e7f8091a");
    const QString mine = QStringLiteral("9f8e7d6c5b4a39281706f5e4d3c2b1a09f8e7d6c5b4a39281706f5e4d3c2b1a0");
    const int incomplete = int(InstallState::IncompleteInstall);
    QTest::newRow("unreadable program") << false << QString() << true << QString() << incomplete << false
                                        << QStringLiteral("missing or not this build");
    QTest::newRow("readable, another build: otherBuild") << false << other << true << QString() << incomplete << true
                                                         << QStringLiteral("a different build");
    QTest::newRow("another build, a lock file missing") << false << other << false << QString() << incomplete << false
                                                        << QStringLiteral("missing or not this build");
    QTest::newRow("another build, SCM configuration incomplete") << false << other << true << QStringLiteral("account")
                                                                 << incomplete << false
                                                                 << QStringLiteral("missing or not this build");
    QTest::newRow("this build, SCM configuration incomplete") << true << mine << true << QStringLiteral("account")
                                                              << incomplete << false
                                                              << QStringLiteral("not fully configured");
    QTest::newRow("this build, complete") << true << mine << true << QString() << int(InstallState::Current) << false
                                          << QString();
}

void tst_InstallationStates::windowsOtherBuild()
{
    QFETCH(bool, digestOk);
    QFETCH(QString, installedSha256);
    QFETCH(bool, locks);
    QFETCH(QString, scmProblem);
    QFETCH(int, state);
    QFETCH(bool, otherBuild);
    QFETCH(QString, detail);
    WindowsEvidence e;
    e.serviceExists = true;
    e.expectedQtExe = kQtExe;
    e.binPath = QStringLiteral("\"%1\" --service").arg(kQtExe);
    e.qtExeDigestOk = digestOk;
    e.installedSha256 = installedSha256;
    e.lockFilesPresent = locks;
    e.scmConfigProblem = scmProblem;
    const Installation i = classifyWindows(e);
    QCOMPARE(int(i.state), state);
    QCOMPARE(i.otherBuild, otherBuild);
    if (otherBuild) QCOMPARE(i.installedSha256, installedSha256);
    if (!detail.isEmpty()) QVERIFY2(i.detail.contains(detail), qPrintable(i.detail));
}

// WS-16 (D63, INV-126): the Apps & features entry is never an input to the classifier. For every state (a v1.0 install
// has no entry; an uninstall's retry has an entry and no service), the answer is the same with and without it. Kills
// inv126-classifier-reads-entry.
void tst_InstallationStates::entryNeverClassified()
{
    QList<WindowsEvidence> cases;
    WindowsEvidence e;
    cases << e;   // nothing installed
    e.serviceExists = true;
    e.expectedQtExe = kQtExe;
    e.binPath = QStringLiteral("\"%1\" --service").arg(kQtExe);
    e.qtExeDigestOk = e.lockFilesPresent = true;
    e.installedSha256 = QStringLiteral("9f8e7d6c5b4a39281706f5e4d3c2b1a09f8e7d6c5b4a39281706f5e4d3c2b1a0");
    cases << e;   // Current
    e.lockFilesPresent = false;
    cases << e;   // a lock file missing
    e.lockFilesPresent = true;
    e.qtExeDigestOk = false;
    e.installedSha256 = QStringLiteral("117eb29a0b1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f708192a3b4c5d6e7f8091a");
    cases << e;   // another build
    e.binPath = QStringLiteral(R"("C:\Elsewhere\aether-serial-bridge.exe" --service)");
    cases << e;   // foreign
    for (WindowsEvidence c : cases) {
        c.appsEntryPresent = false;
        const Installation without = classifyWindows(c);
        c.appsEntryPresent = true;
        const Installation with = classifyWindows(c);
        QCOMPARE(int(with.state), int(without.state));
        QCOMPARE(with.otherBuild, without.otherBuild);
        QCOMPARE(with.detail, without.detail);
        QCOMPARE(with.programRemains, without.programRemains);   // nor is it a leftover
    }
}

void tst_InstallationStates::linuxForeignUnitDetectedByContent()
{
    LinuxEvidence l;
    l.etcUnit = LinuxEvidence::EtcUnit::RegularFile;
    l.etcUnitContent = "[Unit]\nDescription=something else\n[Service]\nExecStart=/usr/bin/other\n";
    QCOMPARE(classifyLinux(l).state, InstallState::Foreign);
    // Even one that claims to be ours: the Qt unit lives in /usr/local/lib/systemd/system, never in /etc.
    l.etcUnitContent = QByteArray(kQtUnitHeader) + "\n[Service]\nExecStart=/usr/local/bin/aether-serial-bridge --run\n";
    QCOMPARE(classifyLinux(l).state, InstallState::Foreign);
}

// Kills: letting Current win when a unit this program did not write sits in /etc beside a complete installation.
void tst_InstallationStates::linuxForeignUnitTakesPrecedence()
{
    LinuxEvidence l;
    l.etcUnit = LinuxEvidence::EtcUnit::RegularFile;
    l.etcUnitContent = "[Service]\nExecStart=/usr/local/bin/aether-serial-bridge --run\n";
    l.qtUnitPresent = true;
    l.qtUnitContent = QByteArray(kQtUnitHeader) + "\n";
    l.binaryPresent = l.lockFilesPresent = true;
    l.thisBuildKnown = l.programDigestOk = true;
    l.installedSha256 = QString(64, QLatin1Char('a'));
    QCOMPARE(classifyLinux(l).state, InstallState::Foreign);
}

void tst_InstallationStates::linuxStates_data()
{
    QTest::addColumn<int>("etc");
    QTest::addColumn<bool>("qtUnit");
    QTest::addColumn<bool>("binary");
    QTest::addColumn<bool>("locks");
    QTest::addColumn<int>("state");
    using E = LinuxEvidence::EtcUnit;
    QTest::newRow("nothing") << int(E::Absent) << false << false << false << int(InstallState::NotInstalled);
    QTest::newRow("mask only (killed after mask, before the unit)") << int(E::MaskLink) << false << false << false
                                                                    << int(InstallState::IncompleteInstall);
    QTest::newRow("unit without binary") << int(E::MaskLink) << true << false << true << int(InstallState::IncompleteInstall);
    QTest::newRow("unit without locks") << int(E::Absent) << true << true << false << int(InstallState::IncompleteInstall);
    QTest::newRow("complete, masked") << int(E::MaskLink) << true << true << true << int(InstallState::Current);
    QTest::newRow("complete, enabled") << int(E::Absent) << true << true << true << int(InstallState::Current);
    QTest::newRow("something else in /etc") << int(E::Other) << true << true << true << int(InstallState::Foreign);
}

void tst_InstallationStates::linuxStates()
{
    QFETCH(int, etc);
    QFETCH(bool, qtUnit);
    QFETCH(bool, binary);
    QFETCH(bool, locks);
    QFETCH(int, state);
    LinuxEvidence l;
    l.etcUnit = LinuxEvidence::EtcUnit(etc);
    l.qtUnitPresent = qtUnit;
    l.qtUnitContent = QByteArray(kQtUnitHeader) + "\n[Unit]\n";
    l.binaryPresent = binary;
    l.lockFilesPresent = locks;
    // This build's program where there is one (v1.1 INV-120: Current needs the digest; tst_InstallationEvidence).
    l.thisBuildKnown = true;
    l.programDigestOk = binary;
    if (binary) l.installedSha256 = QString(64, QLatin1Char('a'));
    QCOMPARE(int(classifyLinux(l).state), state);
}

// No service, but the program still at its installed path, is NotInstalled with programRemains (an
// uninstall that kept the program, so the window offers the Uninstall that finishes it). A clean machine has neither;
// a machine with a service never reports it (the state already offers Uninstall).
void tst_InstallationStates::programRemainsOnlyWithoutAService()
{
    WindowsEvidence w;
    w.expectedQtExe = kQtExe;
    QCOMPARE(classifyWindows(w).state, InstallState::NotInstalled);
    QVERIFY(!classifyWindows(w).programRemains);
    w.qtExePresent = true;
    QCOMPARE(classifyWindows(w).state, InstallState::NotInstalled);
    QVERIFY(classifyWindows(w).programRemains);
    w.lockFilesPresent = true;   // lock files kept with it change nothing in the state either
    QCOMPARE(classifyWindows(w).state, InstallState::NotInstalled);
    w.serviceExists = true;
    w.binPath = QStringLiteral("\"%1\" --service").arg(kQtExe);
    w.qtExeDigestOk = true;
    QCOMPARE(classifyWindows(w).state, InstallState::Current);
    QVERIFY(!classifyWindows(w).programRemains);

    LinuxEvidence l;
    QCOMPARE(classifyLinux(l).state, InstallState::NotInstalled);
    QVERIFY(!classifyLinux(l).programRemains);
    l.binaryPresent = true;   // the unit removed, the program kept
    QCOMPARE(classifyLinux(l).state, InstallState::NotInstalled);
    QVERIFY(classifyLinux(l).programRemains);
    l.qtUnitPresent = true;
    l.qtUnitContent = QByteArray(kQtUnitHeader) + "\n";
    QCOMPARE(classifyLinux(l).state, InstallState::IncompleteInstall);
    QVERIFY(!classifyLinux(l).programRemains);
}

QTEST_GUILESS_MAIN(tst_InstallationStates)
#include "tst_InstallationStates.moc"
