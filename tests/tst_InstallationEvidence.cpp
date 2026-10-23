// Build identity on both OSes (v1.1 D61, INV-120): an installation is Current only with this build's program; a
// complete installation of another build is IncompleteInstall with otherBuild (Save & apply asks before replacing it);
// a program that could not be read, or a caller that does not know its own build, is neither. The build stamp is
// wording only: it never changes the classification. (The other state rows are tst_InstallationStates.)

#include "core/InstallationEvidence.h"

#include <QtTest>

using namespace asb;
using namespace asb::InstallationEvidence;

namespace {

const QString kMine = QStringLiteral("9f8e7d6c5b4a39281706f5e4d3c2b1a09f8e7d6c5b4a39281706f5e4d3c2b1a0");
const QString kOther = QStringLiteral("117eb29a0b1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f708192a3b4c5d6e7f8091a");
const QString kQtExe = QStringLiteral(R"(C:\Program Files\AetherSerialBridge\aether-serial-bridge.exe)");

// A complete Linux installation whose program is `sha` (this build's when it is kMine).
LinuxEvidence linuxInstall(const QString& sha = kMine)
{
    LinuxEvidence l;
    l.etcUnit = LinuxEvidence::EtcUnit::MaskLink;
    l.qtUnitPresent = true;
    l.qtUnitContent = QByteArray(kQtUnitHeader) + "\n[Unit]\n";
    l.binaryPresent = true;
    l.lockFilesPresent = true;
    l.thisBuildKnown = true;
    l.installedSha256 = sha;
    l.programDigestOk = sha == kMine;
    return l;
}

WindowsEvidence windowsInstall(const QString& sha = kMine)
{
    WindowsEvidence e;
    e.serviceExists = true;
    e.expectedQtExe = kQtExe;
    e.binPath = QStringLiteral("\"%1\" --service").arg(kQtExe);
    e.installedSha256 = sha;
    e.qtExeDigestOk = sha == kMine;
    e.lockFilesPresent = true;
    return e;
}

}  // namespace

class tst_InstallationEvidence : public QObject {
    Q_OBJECT

private slots:
    // The installed program is complete but another build: IncompleteInstall with otherBuild and its digest, so the
    // TUI and the window ask before replacing it (as Windows since #21).
    void linuxOtherBuildWhenDigestDiffers()
    {
        const Installation i = classifyLinux(linuxInstall(kOther));
        QCOMPARE(i.state, InstallState::IncompleteInstall);
        QVERIFY(i.otherBuild);
        QCOMPARE(i.installedSha256, kOther);
        QCOMPARE(i.flavor, Installation::Flavor::Linux);
        QVERIFY2(i.detail.contains(QStringLiteral("a different build")), qPrintable(i.detail));
    }

    // Current needs this build's program, and everything else install writes. Each condition has a row that fails it.
    void linuxCurrentNeedsDigest_data()
    {
        QTest::addColumn<bool>("binary");
        QTest::addColumn<bool>("locks");
        QTest::addColumn<QString>("sha");
        QTest::addColumn<int>("state");
        QTest::addColumn<bool>("otherBuild");
        QTest::addColumn<QString>("detail");
        const int incomplete = int(InstallState::IncompleteInstall);
        QTest::newRow("this build, complete") << true << true << kMine << int(InstallState::Current) << false << QString();
        QTest::newRow("another build, complete") << true << true << kOther << incomplete << true << "a different build";
        QTest::newRow("another build, a lock file missing") << true << false << kOther << incomplete << false
                                                            << "a lock file is missing";
        QTest::newRow("this build, a lock file missing") << true << false << kMine << incomplete << false
                                                         << "a lock file is missing";
        QTest::newRow("no program") << false << true << QString() << incomplete << false << "the program is missing";
    }
    void linuxCurrentNeedsDigest()
    {
        QFETCH(bool, binary);
        QFETCH(bool, locks);
        QFETCH(QString, sha);
        QFETCH(int, state);
        QFETCH(bool, otherBuild);
        QFETCH(QString, detail);
        LinuxEvidence l = linuxInstall(sha);
        l.binaryPresent = binary;
        l.lockFilesPresent = locks;
        const Installation i = classifyLinux(l);
        QCOMPARE(int(i.state), state);
        QCOMPARE(i.otherBuild, otherBuild);
        if (!detail.isEmpty()) QVERIFY2(i.detail.contains(detail), qPrintable(i.detail));
    }

    // A program that is there but could not be read whole (not root's, over the cap, a read error) is not a digest
    // match and not "another build": the installation is unfinished and nothing is asked (--install finishes it).
    void linuxUnreadableProgramIsNotOtherBuild()
    {
        LinuxEvidence l = linuxInstall(QString());
        l.programUnreadable = true;
        Installation i = classifyLinux(l);
        QCOMPARE(i.state, InstallState::IncompleteInstall);
        QVERIFY(!i.otherBuild);
        QVERIFY2(i.detail.contains(QStringLiteral("could not be read")), qPrintable(i.detail));
        // Even with a digest beside it (inconsistent evidence), unreadable is never otherBuild.
        l.installedSha256 = kOther;
        i = classifyLinux(l);
        QCOMPARE(i.state, InstallState::IncompleteInstall);
        QVERIFY(!i.otherBuild);
    }

    // A caller that does not know its own build (no expected digest) never sees Current, and never "another build":
    // fail closed into IncompleteInstall.
    void linuxEmptyExpectedDigestNeverCurrent()
    {
        LinuxEvidence l = linuxInstall(kMine);
        l.thisBuildKnown = false;
        Installation i = classifyLinux(l);
        QCOMPARE(i.state, InstallState::IncompleteInstall);
        QVERIFY(!i.otherBuild);
        l.programDigestOk = false;
        l.installedSha256 = kOther;
        i = classifyLinux(l);
        QCOMPARE(i.state, InstallState::IncompleteInstall);
        QVERIFY(!i.otherBuild);
    }

    // The build stamp (installedVersion) is wording only (D61): no stamp, an older, the same or a newer version never
    // changes the state, otherBuild or the detail, on either OS; it is passed through for the question.
    void stampNeverChangesClassification()
    {
        const std::optional<QString> stamps[] = {std::nullopt, QString(), QStringLiteral("1.0.0"), QStringLiteral("1.1.0"),
                                                 QStringLiteral("99.0.0")};
        std::vector<LinuxEvidence> linuxRows = {linuxInstall(kMine), linuxInstall(kOther), linuxInstall(QString())};
        linuxRows[2].programUnreadable = true;
        LinuxEvidence noLocks = linuxInstall(kOther);
        noLocks.lockFilesPresent = false;
        linuxRows.push_back(noLocks);
        LinuxEvidence unknown = linuxInstall(kMine);
        unknown.thisBuildKnown = false;
        linuxRows.push_back(unknown);
        for (LinuxEvidence l : linuxRows) {
            l.installedVersion = std::nullopt;
            const Installation base = classifyLinux(l);
            for (const auto& v : stamps) {
                l.installedVersion = v;
                const Installation i = classifyLinux(l);
                QCOMPARE(i.state, base.state);
                QCOMPARE(i.otherBuild, base.otherBuild);
                QCOMPARE(i.detail, base.detail);
                if (base.state == InstallState::Current || base.state == InstallState::IncompleteInstall)
                    QCOMPARE(i.installedVersion, v);
            }
        }
        std::vector<WindowsEvidence> windowsRows = {windowsInstall(kMine), windowsInstall(kOther), windowsInstall(QString())};
        WindowsEvidence scm = windowsInstall(kOther);
        scm.scmConfigProblem = QStringLiteral("account");
        windowsRows.push_back(scm);
        for (WindowsEvidence e : windowsRows) {
            e.installedVersion = std::nullopt;
            const Installation base = classifyWindows(e);
            for (const auto& v : stamps) {
                e.installedVersion = v;
                const Installation i = classifyWindows(e);
                QCOMPARE(i.state, base.state);
                QCOMPARE(i.otherBuild, base.otherBuild);
                QCOMPARE(i.detail, base.detail);
                // Passed through where the build question can be asked (another build).
                if (base.state == InstallState::IncompleteInstall) QCOMPARE(i.installedVersion, v);
            }
        }
    }
};

QTEST_GUILESS_MAIN(tst_InstallationEvidence)
#include "tst_InstallationEvidence.moc"
