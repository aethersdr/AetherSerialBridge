// The systemd adapter's steps over the fake manager and a temporary root (WS-8; INV-48, 79, 98, 115; master
// §4.6 installation states). Real files, fake D-Bus; the systemd suite runs the same steps against PID 1.

#include "core/InstallationEvidence.h"
#include "platform/posix/SystemdService.h"
#include "platform/posix/SystemdUnit.h"
#include "support/FakeSystemdBus.h"
#include "support/Fakes.h"
#include "support/RecordingSys.h"

#include <QDir>
#include <QFile>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#include <unistd.h>

using namespace asb;
using namespace asb::posix;
using namespace asb::test;
using namespace std::chrono_literals;

namespace {

constexpr char kUnit[] = "aether-serial-bridge.service";

QByteArray enc(const QString& p)
{
    return QFile::encodeName(p);
}

void writeFile(const QString& path, const QByteArray& bytes, mode_t mode = 0644)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).path()));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(bytes);
    f.close();
    QVERIFY(::chmod(enc(path).constData(), mode) == 0);
}

mode_t modeOf(const QString& path)
{
    struct stat st {};
    if (::lstat(enc(path).constData(), &st) != 0) return mode_t(-1);
    return st.st_mode & 07777;
}

const QByteArray kProgram = QByteArrayLiteral("ELF-ish program bytes");

QByteArray sha256(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

struct Fixture {
    QTemporaryDir tmp;
    FakeClock clock;
    FakeSystemdBus bus{clock};
    RecordingSys sys;
    QString root() const { return tmp.path(); }
    QString unitFile() const { return root() + QStringLiteral("/usr/local/lib/systemd/system/") + QLatin1String(kUnit); }
    QString etcUnit() const { return root() + QStringLiteral("/etc/systemd/system/") + QLatin1String(kUnit); }
    QString binary() const { return root() + QStringLiteral("/usr/local/bin/aether-serial-bridge"); }
    QString lockDir() const { return root() + QStringLiteral("/var/lib/aether-serial-bridge"); }
    QString source() const { return root() + QStringLiteral("/build/aether-serial-bridge"); }
    QString legacyPolicy() const { return root() + QStringLiteral("/usr/share/polkit-1/actions/agency.cloaked.aetherserialbridge.policy"); }
    SystemdService::Options opts() const
    {
        SystemdService::Options o;
        o.requireRoot = false;
        o.unitDirectory = root() + QStringLiteral("/usr/local/lib/systemd/system");
        o.etcDirectory = root() + QStringLiteral("/etc/systemd/system");
        o.binaryPath = binary();
        o.sourceBinary = source();
        o.lockDirectory = lockDir();
        o.stagingRoot = root() + QStringLiteral("/usr/local/lib/aether-serial-bridge");
        o.policyPath = root() + QStringLiteral("/usr/share/polkit-1/actions/org.aethersdr.aetherserialbridge.policy");
        o.legacyPolicyPath = legacyPolicy();   // never the real /usr/share: the tests may run as root
        o.policy = "<policy/>";
        o.owner = PosixOwner::current();
        o.expectedSha256 = sha256(kProgram);   // this build's program (v1.1 INV-120): what install copies
        return o;
    }
    std::unique_ptr<SystemdService> service;
    Fixture()
    {
        ::umask(022);
        QDir().mkpath(root() + QStringLiteral("/etc/systemd/system"));
        QDir().mkpath(root() + QStringLiteral("/var/lib"));
        writeFile(source(), kProgram, 0755);
        bus.unitFileExists = [this](const QString&) { return QFileInfo::exists(unitFile()); };
        service = std::make_unique<SystemdService>(bus, opts(), clock, sys);
    }
    Deadline in(std::chrono::milliseconds d) { return clock.now() + d; }
    void maskLink() { QVERIFY(::symlink("/dev/null", enc(etcUnit()).constData()) == 0); }
};

}  // namespace

class tst_SystemdService : public QObject {
    Q_OBJECT

private slots:
    // B1 (INV-48): a PERSISTENT mask (never --runtime: /run is gone after a reboot), then disable, reload,
    // and the unit directory flushed; verified masked.
    void inhibitStartsMasksPersistently()
    {
        Fixture f;
        f.service->inhibitStarts(f.in(30s));
        QStringList seq;
        for (const FakeSystemdBus::Call& c : f.bus.calls) seq << c.method;
        QCOMPARE(seq.mid(0, 3), (QStringList{QStringLiteral("MaskUnitFiles"), QStringLiteral("DisableUnitFiles"), QStringLiteral("Reload")}));
        QCOMPARE(f.bus.calls[0].args, (QStringList{QString::fromLatin1(kUnit), QStringLiteral("persistent"), QStringLiteral("noforce")}));
        QCOMPARE(f.bus.calls[1].args.value(1), QStringLiteral("persistent"));
        bool synced = false;
        for (const QString& e : f.sys.log()) synced = synced || e == QStringLiteral("fsync %1/etc/systemd/system").arg(f.root());
        QVERIFY2(synced, qPrintable(f.sys.log().join(QLatin1Char('\n'))));
        QVERIFY(seq.contains(QStringLiteral("Get")));   // LoadState verified
    }
    void inhibitStartsVerifiesTheMask()
    {
        Fixture f;
        f.bus.unitFileExists = [](const QString&) { return true; };
        f.bus.failures.clear();
        // A manager that ignores the mask: LoadState stays "loaded" -> the step fails.
        f.bus.onCall = [&](const QString& m) {
            if (m == QLatin1String("Reload")) f.bus.units[QString::fromLatin1(kUnit)].masked = false;
        };
        QVERIFY_THROWS_EXCEPTION(PlatformError, f.service->inhibitStarts(f.in(30s)));
    }

    // INV-79 (seam half of unit_never_visible_unmasked): install masks BEFORE the unit file exists.
    void installMasksBeforeTheUnitFile()
    {
        Fixture f;
        bool fileAtMask = true;
        f.bus.onCall = [&](const QString& m) {
            if (m == QLatin1String("MaskUnitFiles")) fileAtMask = QFileInfo::exists(f.unitFile());
        };
        f.service->installOrConfigure(InstallState::NotInstalled, f.in(60s));
        QVERIFY(!fileAtMask);
        QVERIFY(QFileInfo::exists(f.unitFile()));
        QCOMPARE(f.bus.calls.first().method, QStringLiteral("MaskUnitFiles"));
        QCOMPARE(f.bus.calls.first().args.value(1), QStringLiteral("persistent"));
        // The mask link is flushed before the unit file is created.
        const QStringList log = f.sys.log();
        qsizetype sync = -1, unit = -1;
        for (qsizetype i = 0; i < log.size(); ++i) {
            if (sync < 0 && log[i] == QStringLiteral("fsync %1/etc/systemd/system").arg(f.root())) sync = i;
            if (unit < 0 && log[i].startsWith(QStringLiteral("renameat %1.").arg(QLatin1String(kUnit)))) unit = i;
        }
        QVERIFY2(sync >= 0 && unit > sync, qPrintable(log.join(QLatin1Char('\n'))));
    }
    // Every directory on the way to what systemd runs is judged on its fd: owned by root (here the
    // test's owner) and not group/other-writable unless sticky; a directory the step creates is secured, and a
    // failure to secure it fails the step (the creation is still reported).
    void installRefusesAWritableDirectoryOnThePath()
    {
        Fixture f;
        QVERIFY(QDir().mkpath(f.root() + QStringLiteral("/usr/local")));
        QVERIFY(::chmod(enc(f.root() + QStringLiteral("/usr")).constData(), 0775) == 0);
        try {
            f.service->installOrConfigure(InstallState::NotInstalled, f.in(60s));
            QFAIL("installed below a group-writable directory");
        } catch (const PlatformError& e) {
            QVERIFY2(QString::fromLocal8Bit(e.what()).contains(QStringLiteral("can be changed by other accounts")), e.what());
        }
        QVERIFY(!QFileInfo::exists(f.binary()));
        QVERIFY(!QFileInfo::exists(f.unitFile()));
        QVERIFY(f.bus.calls.isEmpty());   // refused before the mask: nothing changed at all
    }
    // Debian's old layout (upgraded Pi OS hosts) has /usr/local and /usr/local/bin at 2775 root:staff.
    // Still refused (not widened to staff), but the message names every such directory and the exact remedy.
    void debianStaffLayoutNamesTheRemedy()
    {
        Fixture f;
        for (const char* d : {"/usr/local/bin", "/usr/local/lib"}) QVERIFY(QDir().mkpath(f.root() + QLatin1String(d)));
        for (const char* d : {"/usr/local", "/usr/local/bin", "/usr/local/lib"})
            QVERIFY(::chmod(enc(f.root() + QLatin1String(d)).constData(), 02775) == 0);
        try {
            f.service->installOrConfigure(InstallState::NotInstalled, f.in(60s));
            QFAIL("installed below group-writable directories");
        } catch (const PlatformError& e) {
            const QString m = QString::fromLocal8Bit(e.what());
            const QString remedy = QStringLiteral("run: chmod g-w %1/usr/local %1/usr/local/lib %1/usr/local/bin").arg(f.root());
            QVERIFY2(m.endsWith(remedy), qPrintable(m));
            QVERIFY2(m.contains(QStringLiteral("%1/usr/local is mode 2775, writable by group ").arg(f.root())), qPrintable(m));
        }
        QVERIFY(f.bus.calls.isEmpty());
    }
    // A directory that appears between the open and the mkdir (EEXIST) is someone else's: it is
    // judged, never reported as created, never re-owned or re-moded.
    void directoryCreatedByAnotherIsOnlyJudged()
    {
        Fixture f;
        QVERIFY(QDir().mkpath(f.root() + QStringLiteral("/usr/local")));
        const QString bin = f.root() + QStringLiteral("/usr/local/bin");
        // The first open of "bin" is install's read-only preflight; the race is staged after the second, the
        // open that the creating walk makes just before its mkdir.
        f.sys.after(QStringLiteral("openat bin"), [&] {
            f.sys.after(QStringLiteral("openat bin"), [&] { QVERIFY(::mkdir(enc(bin).constData(), 0711) == 0); });
        });
        f.service->installOrConfigure(InstallState::NotInstalled, f.in(60s));
        QCOMPARE(modeOf(bin), mode_t(0711));
        QVERIFY(!f.service->takeChanges().contains(QStringLiteral("created %1").arg(bin)));
        bool eexist = false;
        for (const QString& e : f.sys.log()) {
            QVERIFY2(!e.startsWith(QStringLiteral("fchmod bin ")) && !e.startsWith(QStringLiteral("fchown bin")), qPrintable(e));
            eexist = eexist || e.startsWith(QStringLiteral("mkdirat bin"));
        }
        QVERIFY2(eexist, "the race was not staged: no mkdirat of bin");
    }
    // Install run from the installed program itself keeps the copy only when it is what
    // install makes (owner, 0755); a 0700 copy the DynamicUser service cannot run is replaced atomically.
    void installFromTheInstalledProgramRepairsItsMode()
    {
        Fixture f;
        f.service->installOrConfigure(InstallState::NotInstalled, f.in(60s));
        QCOMPARE(modeOf(f.binary()), mode_t(0755));
        QVERIFY(::chmod(enc(f.binary()).constData(), 0700) == 0);
        SystemdService::Options o = f.opts();
        o.sourceBinary = f.binary();
        SystemdService s(f.bus, o, f.clock, f.sys);
        s.installOrConfigure(InstallState::Current, f.in(60s));
        QCOMPARE(modeOf(f.binary()), mode_t(0755));
        QFile b(f.binary());
        QVERIFY(b.open(QIODevice::ReadOnly));
        QCOMPARE(b.readAll(), QByteArray("ELF-ish program bytes"));
        QVERIFY(s.takeChanges().contains(QStringLiteral("installed %1").arg(f.binary())));
        // Already right: kept as it is (same inode, nothing installed).
        struct stat before {};
        QVERIFY(::stat(enc(f.binary()).constData(), &before) == 0);
        SystemdService again(f.bus, o, f.clock, f.sys);
        again.installOrConfigure(InstallState::Current, f.in(60s));
        struct stat after {};
        QVERIFY(::stat(enc(f.binary()).constData(), &after) == 0);
        QCOMPARE(after.st_ino, before.st_ino);
        QVERIFY(!again.takeChanges().contains(QStringLiteral("installed %1").arg(f.binary())));
    }
    void installAcceptsAStickyWorldWritableDirectory()
    {
        Fixture f;
        QVERIFY(QDir().mkpath(f.root() + QStringLiteral("/usr/local")));
        QVERIFY(::chmod(enc(f.root() + QStringLiteral("/usr")).constData(), 01777) == 0);
        f.service->installOrConfigure(InstallState::NotInstalled, f.in(60s));
        QVERIFY(QFileInfo::exists(f.binary()));
    }
    void installFailsWhenACreatedDirectoryCannotBeSecured()
    {
        Fixture f;
        f.sys.failWith(QStringLiteral("fchmod bin "), EPERM);
        QVERIFY_THROWS_EXCEPTION(PlatformError, f.service->installOrConfigure(InstallState::NotInstalled, f.in(60s)));
        QVERIFY(!QFileInfo::exists(f.binary()));
        const QStringList changes = f.service->takeChanges();
        QVERIFY2(changes.contains(QStringLiteral("created %1/usr/local/bin").arg(f.root())), qPrintable(changes.join(QLatin1Char('\n'))));
    }
    void uninstallReportsAFailedFlush()
    {
        Fixture f;
        f.service->installOrConfigure(InstallState::NotInstalled, f.in(60s));
        f.service->takeChanges();
        f.sys.failWith(QStringLiteral("fsync %1/usr/local/bin").arg(f.root()), EIO);
        QVERIFY_THROWS_EXCEPTION(PlatformError, f.service->uninstall(f.in(60s)));
        QVERIFY(!QFileInfo::exists(f.binary()));   // removed, and reported although the step failed
        QVERIFY(f.service->takeChanges().contains(QStringLiteral("removed %1").arg(f.binary())));
    }
    void installWritesEverythingAndStaysMasked()
    {
        Fixture f;
        f.service->installOrConfigure(InstallState::NotInstalled, f.in(60s));
        QFile u(f.unitFile());
        QVERIFY(u.open(QIODevice::ReadOnly));
        QCOMPARE(u.readAll(), SystemdUnit::render(f.binary()));
        QCOMPARE(modeOf(f.unitFile()), mode_t(0644));
        QFile b(f.binary());
        QVERIFY(b.open(QIODevice::ReadOnly));
        QCOMPARE(b.readAll(), QByteArray("ELF-ish program bytes"));
        QCOMPARE(modeOf(f.binary()), mode_t(0755));
        QCOMPARE(modeOf(f.lockDir()), mode_t(0755));
        QVERIFY(QFileInfo::exists(f.opts().policyPath));
        QCOMPARE(f.bus.units[QString::fromLatin1(kUnit)].load, QStringLiteral("masked"));
        QVERIFY(!f.service->takeChanges().isEmpty());
        // The lock files are the core's (LockIdentity, after the engine-free proof): not written here.
        QVERIFY(QDir(f.lockDir()).isEmpty());
    }
    void installNeverChangesAnExistingLockDirectory()
    {
        Fixture f;
        QVERIFY(QDir().mkpath(f.lockDir()));
        QVERIFY(::chmod(enc(f.lockDir()).constData(), 0777) == 0);
        f.service->installOrConfigure(InstallState::IncompleteInstall, f.in(60s));
        QCOMPARE(modeOf(f.lockDir()), mode_t(0777));   // EngineLock refuses it; nothing here repairs it
    }
    void installRefusesForeign()
    {
        Fixture f;
        QVERIFY_THROWS_EXCEPTION(PlatformError, f.service->installOrConfigure(InstallState::Foreign, f.in(60s)));
        QVERIFY(f.bus.calls.isEmpty());
    }

    void allowStartsUnmasksEnablesAndVerifies()
    {
        Fixture f;
        f.service->installOrConfigure(InstallState::NotInstalled, f.in(60s));
        f.bus.calls.clear();
        f.service->allowStarts(f.in(15s));
        QStringList seq = f.bus.methods();
        seq.removeAll(QStringLiteral("LoadUnit"));
        seq.removeAll(QStringLiteral("Get"));
        QCOMPARE(seq, (QStringList{QStringLiteral("UnmaskUnitFiles"), QStringLiteral("EnableUnitFiles"), QStringLiteral("Reload")}));
        QCOMPARE(f.bus.units[QString::fromLatin1(kUnit)].load, QStringLiteral("loaded"));
    }

    // INV-79: stopping a unit systemd does not know is a no-op; any other error is not.
    void stopWhenNotInstalledIsNoop()
    {
        Fixture f;
        f.service->requestStop(f.in(30s));
        QCOMPARE(f.bus.methods(), QStringList{QStringLiteral("StopUnit")});
        f.bus.units[QString::fromLatin1(kUnit)].load = QStringLiteral("loaded");
        f.bus.failures.insert(QStringLiteral("StopUnit"), QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"));
        QVERIFY_THROWS_EXCEPTION(std::exception, f.service->requestStop(f.in(30s)));
    }
    void stopIsNotAwaited()
    {
        Fixture f;
        f.bus.units[QString::fromLatin1(kUnit)].load = QStringLiteral("loaded");
        f.bus.plans << FakeSystemdBus::JobPlan{FakeSystemdBus::When::Never, QStringLiteral("done"), {}, false};
        f.service->requestStop(f.in(30s));   // returns at once: the engine lock is the proof
        QVERIFY(f.bus.pumpTimeouts.isEmpty());
    }

    // The engine-free proof needs "stopped" AND an empty process set; the pid is reported only while a process
    // exists.
    void queryStates_data()
    {
        QTest::addColumn<QString>("load");
        QTest::addColumn<QString>("active");
        QTest::addColumn<int>("processes");
        QTest::addColumn<int>("mainPid");
        QTest::addColumn<int>("state");
        QTest::addColumn<int>("pid");
        QTest::newRow("not installed") << "not-found" << "inactive" << 0 << 0 << int(ServiceQuery::Absent) << 0;
        QTest::newRow("masked, stopped") << "masked" << "inactive" << 0 << 0 << int(ServiceQuery::Stopped) << 0;
        QTest::newRow("failed, empty") << "loaded" << "failed" << 0 << 0 << int(ServiceQuery::Stopped) << 0;
        QTest::newRow("inactive but a process left") << "loaded" << "inactive" << 1 << 0 << int(ServiceQuery::Other) << 77;
        QTest::newRow("running") << "loaded" << "active" << 1 << 77 << int(ServiceQuery::Running) << 77;
        QTest::newRow("starting") << "loaded" << "activating" << 1 << 77 << int(ServiceQuery::StartPending) << 77;
        QTest::newRow("stopping") << "masked" << "deactivating" << 1 << 77 << int(ServiceQuery::StopPending) << 77;
    }
    void queryStates()
    {
        QFETCH(QString, load);
        QFETCH(QString, active);
        QFETCH(int, processes);
        QFETCH(int, mainPid);
        QFETCH(int, state);
        QFETCH(int, pid);
        Fixture f;
        FakeSystemdBus::Unit u;
        u.load = load;
        u.active = active;
        u.mainPid = quint32(mainPid);
        for (int i = 0; i < processes; ++i) u.processes << SystemdUnitProcess{QStringLiteral("/system.slice/x"), 77, QStringLiteral("x")};
        f.bus.units.insert(QString::fromLatin1(kUnit), u);
        const ServiceQuery q = f.service->query(f.in(15s));
        QCOMPARE(int(q.state), state);
        QCOMPARE(q.pid, qint64(pid));
    }

    // A stopped unit says how its main process ended (ExecMainCode CLD_EXITED -> ExecMainStatus), so an
    // ACK timeout names exit 4 or 6 on Linux too; a signal or no run leaves the code unset.
    void queryExitCode_data()
    {
        QTest::addColumn<QString>("active");
        QTest::addColumn<int>("how");
        QTest::addColumn<int>("status");
        QTest::addColumn<int>("expected");   // -1: unset
        QTest::newRow("failed, exit 4") << QStringLiteral("failed") << 1 << 4 << 4;
        QTest::newRow("inactive, exit 6 (clean: SuccessExitStatus)") << QStringLiteral("inactive") << 1 << 6 << 6;
        QTest::newRow("inactive, exit 0") << QStringLiteral("inactive") << 1 << 0 << 0;
        QTest::newRow("killed by a signal") << QStringLiteral("failed") << 2 << 9 << -1;
        QTest::newRow("never ran") << QStringLiteral("inactive") << 0 << 0 << -1;
    }
    void queryExitCode()
    {
        QFETCH(QString, active);
        QFETCH(int, how);
        QFETCH(int, status);
        QFETCH(int, expected);
        Fixture f;
        FakeSystemdBus::Unit u;
        u.load = QStringLiteral("loaded");
        u.active = active;
        u.execMainCode = how;
        u.execMainStatus = status;
        f.bus.units.insert(QString::fromLatin1(kUnit), u);
        const ServiceQuery q = f.service->query(f.in(15s));
        QCOMPARE(int(q.state), int(ServiceQuery::Stopped));
        QCOMPARE(q.exitCode.has_value(), expected >= 0);
        if (expected >= 0) QCOMPARE(*q.exitCode, expected);
    }

    // The exit-code reads are a diagnostic; a deadline that passes before them leaves the
    // code unset and the query still answers Stopped (the engine-free proof reads it), never a throw.
    void queryExitCodeAfterTheDeadline()
    {
        Fixture f;
        FakeSystemdBus::Unit u;
        u.load = QStringLiteral("loaded");
        u.active = QStringLiteral("failed");
        u.execMainCode = 1;
        u.execMainStatus = 4;
        f.bus.units.insert(QString::fromLatin1(kUnit), u);
        f.bus.onCall = [&f](const QString& method) {
            if (method == QLatin1String("Get") && f.bus.calls.back().args.contains(QStringLiteral("MainPID")))
                f.clock.advance(std::chrono::seconds(20));   // past the 15 s deadline before ExecMainCode
        };
        ServiceQuery q;
        try {
            q = f.service->query(f.in(15s));
        } catch (const std::exception& e) {
            QFAIL(e.what());
        }
        QCOMPARE(int(q.state), int(ServiceQuery::Stopped));
        QVERIFY(!q.exitCode);
    }

    void detectStates_data()
    {
        QTest::addColumn<QString>("setup");
        QTest::addColumn<int>("state");
        QTest::newRow("nothing") << "" << int(InstallState::NotInstalled);
        QTest::newRow("mask link only") << "mask" << int(InstallState::IncompleteInstall);
        QTest::newRow("qt unit only") << "unit" << int(InstallState::IncompleteInstall);
        QTest::newRow("qt unit + binary, no lock files") << "unit,binary" << int(InstallState::IncompleteInstall);
        QTest::newRow("current") << "unit,binary,locks" << int(InstallState::Current);
        QTest::newRow("current, masked") << "unit,binary,locks,mask" << int(InstallState::Current);
        QTest::newRow("another build (v1.1 INV-120)") << "unit,otherbinary,locks" << int(InstallState::IncompleteInstall);
        QTest::newRow("foreign file in /etc") << "etcforeign" << int(InstallState::Foreign);
        QTest::newRow("foreign file in /etc beside the qt artifacts") << "etcforeign,unit,binary,locks"
                                                                       << int(InstallState::Foreign);
        QTest::newRow("foreign link in /etc") << "etclink" << int(InstallState::Foreign);
        QTest::newRow("foreign qt-path unit") << "unitforeign" << int(InstallState::Foreign);
    }
    void detectStates()
    {
        QFETCH(QString, setup);
        QFETCH(int, state);
        Fixture f;
        const QStringList s = setup.split(QLatin1Char(','), Qt::SkipEmptyParts);
        if (s.contains(QStringLiteral("mask"))) f.maskLink();
        if (s.contains(QStringLiteral("unit"))) writeFile(f.unitFile(), SystemdUnit::render(f.binary()));
        if (s.contains(QStringLiteral("unitforeign"))) writeFile(f.unitFile(), "[Unit]\nDescription=someone else's\n");
        if (s.contains(QStringLiteral("binary"))) writeFile(f.binary(), kProgram, 0755);
        if (s.contains(QStringLiteral("otherbinary"))) writeFile(f.binary(), "another build's bytes", 0755);
        if (s.contains(QStringLiteral("locks"))) {
            writeFile(f.lockDir() + QStringLiteral("/engine.lock"), "{}");
            writeFile(f.lockDir() + QStringLiteral("/engine.lock.id"), "{}");
        }
        if (s.contains(QStringLiteral("etcforeign"))) writeFile(f.etcUnit(), "[Unit]\n");
        if (s.contains(QStringLiteral("etclink"))) QVERIFY(::symlink("/lib/systemd/system/x.service", enc(f.etcUnit()).constData()) == 0);
        const Installation i = f.service->detect(f.in(15s));
        QCOMPARE(int(i.state), state);
        QVERIFY(f.bus.calls.isEmpty());   // detection reads files only
    }

    // v1.1 D61, INV-120: detect() hashes the installed program (a regular file of the owner, read through the opened
    // fd) and compares it with this build's digest; its build stamp is read for wording only.
    void detectHashesInstalledProgram()
    {
        Fixture f;
        writeFile(f.unitFile(), SystemdUnit::render(f.binary()));
        writeFile(f.lockDir() + QStringLiteral("/engine.lock"), "{}");
        writeFile(f.lockDir() + QStringLiteral("/engine.lock.id"), "{}");
        writeFile(f.binary(), kProgram, 0755);
        Installation i = f.service->detect(f.in(15s));
        QCOMPARE(i.state, InstallState::Current);
        QCOMPARE(i.installedVersion, std::optional<QString>(QString()));   // read, no stamp: an earlier build
        const QByteArray other = "another build's bytes";
        writeFile(f.binary(), other, 0755);
        i = f.service->detect(f.in(15s));
        QCOMPARE(i.state, InstallState::IncompleteInstall);
        QVERIFY(i.otherBuild);
        QCOMPARE(i.installedSha256.toLatin1(), sha256(other));
        // Same-size rewrites in place, back to back (kProgram and `other` are both 21 bytes): each one is seen at once,
        // inside one tick of the kernel's coarse file clock, because a file this young is never served from the
        // digest cache (detectKeepsDigestOnlyForSettledProgram).
        QByteArray same = other;
        same[0] = 'A';
        writeFile(f.binary(), same, 0755);
        i = f.service->detect(f.in(15s));
        QCOMPARE(i.installedSha256.toLatin1(), sha256(same));
        writeFile(f.binary(), kProgram, 0755);
        QCOMPARE(f.service->detect(f.in(15s)).state, InstallState::Current);
        writeFile(f.binary(), other, 0755);
        QVERIFY(f.service->detect(f.in(15s)).otherBuild);
        // A program of another owner is not read: unfinished, never "another build".
        SystemdService::Options o = f.opts();
        o.owner = PosixOwner{::getuid() + 1, ::getgid()};
        SystemdService notOurs(f.bus, o, f.clock, f.sys);
        i = notOurs.detect(f.in(15s));
        QCOMPARE(i.state, InstallState::IncompleteInstall);
        QVERIFY(!i.otherBuild);
        QVERIFY2(i.detail.contains(QStringLiteral("could not be read")), qPrintable(i.detail));
        QVERIFY(i.installedSha256.isEmpty());
        // A caller without this build's digest: never Current, never "another build".
        writeFile(f.binary(), kProgram, 0755);
        o = f.opts();
        o.expectedSha256.clear();
        SystemdService unknown(f.bus, o, f.clock, f.sys);
        i = unknown.detect(f.in(15s));
        QCOMPARE(i.state, InstallState::IncompleteInstall);
        QVERIFY(!i.otherBuild);
        QVERIFY(f.bus.calls.isEmpty());   // detection reads files only
    }

    // The digest cache (racy-clean rule): a program's digest is kept only once its ctime is settled, 2 s older than
    // the wall clock when it was hashed. The file times are pinned to one instant T here, as a same-size rewrite in
    // place inside one coarse tick leaves them: device, inode, size, mtime and ctime all unchanged.
    void detectKeepsDigestOnlyForSettledProgram()
    {
        Fixture f;
        writeFile(f.unitFile(), SystemdUnit::render(f.binary()));
        writeFile(f.lockDir() + QStringLiteral("/engine.lock"), "{}");
        writeFile(f.lockDir() + QStringLiteral("/engine.lock.id"), "{}");
        const struct timespec t {1'800'000'000, 500'000'000};
        struct timespec ctime = t;
        f.sys.editFstat = [&ctime, t](struct stat& st) {
            st.st_mtim = t;
            st.st_ctim = ctime;
        };
        const QByteArray other = "another build's bytes";
        QCOMPARE(other.size(), kProgram.size());
        struct timespec now {};
        SystemdService::Options o = f.opts();
        o.wallClock = [&now] { return now; };
        // Racy: hashed within 2 s of its ctime (one second later), so not kept; the rewrite in the same tick is seen.
        now = timespec{t.tv_sec + 1, t.tv_nsec};
        {
            SystemdService racy(f.bus, o, f.clock, f.sys);
            writeFile(f.binary(), kProgram, 0755);
            QCOMPARE(racy.detect(f.in(15s)).state, InstallState::Current);
            writeFile(f.binary(), other, 0755);
            const Installation i = racy.detect(f.in(15s));
            QCOMPARE(i.state, InstallState::IncompleteInstall);
            QVERIFY(i.otherBuild);
            QCOMPARE(i.installedSha256.toLatin1(), sha256(other));
        }
        // Settled: hashed an hour after its ctime, so kept. No write can still land on that ctime, so the pinned
        // times below stand for "unchanged" and the kept digest is what detect() reports, without reading the file.
        now = timespec{t.tv_sec + 3600, t.tv_nsec};
        {
            SystemdService settled(f.bus, o, f.clock, f.sys);
            writeFile(f.binary(), kProgram, 0755);
            QCOMPARE(settled.detect(f.in(15s)).state, InstallState::Current);
            writeFile(f.binary(), other, 0755);
            QCOMPARE(settled.detect(f.in(15s)).state, InstallState::Current);   // the kept digest
            // Any write moves ctime: hashed again.
            ctime.tv_nsec += 1;
            const Installation i = settled.detect(f.in(15s));
            QVERIFY(i.otherBuild);
            QCOMPARE(i.installedSha256.toLatin1(), sha256(other));
        }
        // Exactly 2 s is not settled yet (the bound is strict).
        ctime = t;
        now = timespec{t.tv_sec + 2, t.tv_nsec};
        {
            SystemdService edge(f.bus, o, f.clock, f.sys);
            writeFile(f.binary(), kProgram, 0755);
            QCOMPARE(edge.detect(f.in(15s)).state, InstallState::Current);
            writeFile(f.binary(), other, 0755);
            QVERIFY(edge.detect(f.in(15s)).otherBuild);
        }
        QVERIFY(f.bus.calls.isEmpty());   // detection reads files only
    }

    // The installed program is never read through a link: a symlink at /usr/local/bin/aether-serial-bridge to this
    // build's bytes is not this build's program (O_NOFOLLOW, lstat).
    void detectRefusesSymlinkedProgram()
    {
        Fixture f;
        writeFile(f.unitFile(), SystemdUnit::render(f.binary()));
        writeFile(f.lockDir() + QStringLiteral("/engine.lock"), "{}");
        writeFile(f.lockDir() + QStringLiteral("/engine.lock.id"), "{}");
        const QString real = f.root() + QStringLiteral("/elsewhere/aether-serial-bridge");
        writeFile(real, kProgram, 0755);
        QVERIFY(QDir().mkpath(QFileInfo(f.binary()).path()));
        QVERIFY(::symlink(enc(real).constData(), enc(f.binary()).constData()) == 0);
        const Installation i = f.service->detect(f.in(15s));
        QVERIFY(i.state != InstallState::Current);
        QCOMPARE(i.state, InstallState::IncompleteInstall);
        QVERIFY(!i.otherBuild);
        QVERIFY(i.installedSha256.isEmpty());
    }

    void uninstallRemovesEverything()
    {
        Fixture f;
        f.service->installOrConfigure(InstallState::NotInstalled, f.in(60s));
        writeFile(f.lockDir() + QStringLiteral("/engine.lock"), "{}");
        writeFile(f.lockDir() + QStringLiteral("/engine.lock.id"), "{}");
        f.bus.calls.clear();
        f.service->uninstall(f.in(60s));
        QVERIFY(!QFileInfo::exists(f.unitFile()));
        QVERIFY(!QFileInfo::exists(f.binary()));
        QVERIFY(!QFileInfo::exists(f.opts().policyPath));
        QVERIFY(!QFileInfo::exists(f.lockDir()));
        QStringList seq = f.bus.methods();
        seq.removeAll(QStringLiteral("LoadUnit"));
        seq.removeAll(QStringLiteral("Get"));
        QCOMPARE(seq, (QStringList{QStringLiteral("StopUnit"), QStringLiteral("DisableUnitFiles"), QStringLiteral("UnmaskUnitFiles"),
                                   QStringLiteral("Reload")}));
        QCOMPARE(f.bus.units[QString::fromLatin1(kUnit)].load, QStringLiteral("not-found"));
    }

    // An uninstall that removed the unit and failed on the program leaves NotInstalled with the program
    // (programRemains). The next uninstall finishes it: the program, the policy and the lock files go, and
    // DisableUnitFiles, which systemd refuses for a unit file that does not exist, is not called.
    void uninstallFinishesAKeptProgram()
    {
        Fixture f;
        f.service->installOrConfigure(InstallState::NotInstalled, f.in(60s));
        writeFile(f.lockDir() + QStringLiteral("/engine.lock"), "{}");
        writeFile(f.lockDir() + QStringLiteral("/engine.lock.id"), "{}");
        QVERIFY(QFile::remove(f.unitFile()));   // what the failed uninstall left
        const Installation left = f.service->detect(f.in(15s));
        QCOMPARE(left.state, InstallState::NotInstalled);
        QVERIFY(left.programRemains);
        f.bus.calls.clear();
        f.service->uninstall(f.in(60s));
        QVERIFY(!QFileInfo::exists(f.binary()));
        QVERIFY(!QFileInfo::exists(f.opts().policyPath));
        QVERIFY(!QFileInfo::exists(f.lockDir()));
        QStringList seq = f.bus.methods();
        seq.removeAll(QStringLiteral("LoadUnit"));
        seq.removeAll(QStringLiteral("Get"));
        QCOMPARE(seq, (QStringList{QStringLiteral("StopUnit"), QStringLiteral("UnmaskUnitFiles"), QStringLiteral("Reload")}));
        const Installation done = f.service->detect(f.in(15s));
        QCOMPARE(done.state, InstallState::NotInstalled);
        QVERIFY(!done.programRemains);
    }

    // The identifiers moved from agency.cloaked to org.aethersdr (2026-10-22). The defaults name both policies, in the
    // same directory.
    void legacyPolicyDefaults()
    {
        const SystemdService::Options o;
        QCOMPARE(o.policyPath, QStringLiteral("/usr/share/polkit-1/actions/org.aethersdr.aetherserialbridge.policy"));
        QCOMPARE(o.legacyPolicyPath, QStringLiteral("/usr/share/polkit-1/actions/agency.cloaked.aetherserialbridge.policy"));
    }
    // Install (and the update, which is the same step over an older program) writes the current policy, then removes
    // the one every release up to v1.1.0 installed: never two actions for the same program, never none.
    void installRemovesTheLegacyPolicy()
    {
        Fixture f;
        writeFile(f.legacyPolicy(), "<old policy/>");
        f.service->installOrConfigure(InstallState::IncompleteInstall, f.in(60s));   // an update: another build installed (INV-120)
        QVERIFY(QFileInfo::exists(f.opts().policyPath));
        QVERIFY(!QFileInfo::exists(f.legacyPolicy()));
        const QStringList changes = f.service->takeChanges();
        const qsizetype wrote = changes.indexOf(QStringLiteral("wrote %1").arg(f.opts().policyPath));
        const qsizetype removed = changes.indexOf(QStringLiteral("removed %1").arg(f.legacyPolicy()));
        QVERIFY2(wrote >= 0 && removed > wrote, qPrintable(changes.join(QStringLiteral(" | "))));
        // A later install finds none and reports nothing about it.
        SystemdService again(f.bus, f.opts(), f.clock, f.sys);
        again.installOrConfigure(InstallState::Current, f.in(60s));
        QVERIFY(!again.takeChanges().join(QLatin1Char('\n')).contains(f.legacyPolicy()));
    }
    // Not a regular file (a directory, a symlink): refused by the scan before any change, named with whatever else that
    // scan finds, never removed (not after the mask: by then the unit and the program are already written).
    void installRefusesALegacyPolicyThatIsNotAFile_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::newRow("directory") << "directory";
        QTest::newRow("symlink") << "symlink";
    }
    void installRefusesALegacyPolicyThatIsNotAFile()
    {
        QFETCH(QString, kind);
        Fixture f;
        writeFile(f.root() + QStringLiteral("/elsewhere/policy"), "<a policy/>");
        if (kind == QLatin1String("directory")) {
            QVERIFY(QDir().mkpath(f.legacyPolicy()));
        } else {
            QVERIFY(QDir().mkpath(QFileInfo(f.legacyPolicy()).path()));
            QVERIFY(::symlink(enc(f.root() + QStringLiteral("/elsewhere/policy")).constData(), enc(f.legacyPolicy()).constData()) == 0);
        }
        try {
            f.service->installOrConfigure(InstallState::NotInstalled, f.in(60s));
            QFAIL("installed beside a legacy policy path that is not a file");
        } catch (const PlatformError& e) {
            QVERIFY2(QString::fromLocal8Bit(e.what()).contains(f.legacyPolicy() + QStringLiteral(" is not a regular file")), e.what());
        }
        QVERIFY(f.bus.calls.isEmpty());   // refused before the mask: nothing changed at all
        QVERIFY(!QFileInfo::exists(f.unitFile()));
        QVERIFY(!QFileInfo::exists(f.binary()));
        QVERIFY(!QFileInfo::exists(f.opts().policyPath));
        QVERIFY(QFileInfo(f.legacyPolicy()).isSymLink() || QFileInfo(f.legacyPolicy()).isDir());
        QCOMPARE(QFile(f.root() + QStringLiteral("/elsewhere/policy")).exists(), true);
    }
    // The refusal names the whole remedy at once: a writable directory on the path and the legacy entry together.
    void installNamesALegacyNonFileWithTheOtherFindings()
    {
        Fixture f;
        QVERIFY(QDir().mkpath(f.legacyPolicy()));
        QVERIFY(::chmod(enc(f.root() + QStringLiteral("/usr")).constData(), 0775) == 0);
        try {
            f.service->installOrConfigure(InstallState::NotInstalled, f.in(60s));
            QFAIL("installed below a group-writable directory");
        } catch (const PlatformError& e) {
            const QString m = QString::fromLocal8Bit(e.what());
            QVERIFY2(m.contains(QStringLiteral("can be changed by other accounts")), e.what());
            QVERIFY2(m.contains(f.legacyPolicy() + QStringLiteral(" is not a regular file")), e.what());
        }
        QVERIFY(f.bus.calls.isEmpty());
    }
    // A legacy policy in another directory than the current one: that directory is judged too.
    void installJudgesALegacyPolicyDirectoryOfItsOwn()
    {
        Fixture f;
        SystemdService::Options o = f.opts();
        o.legacyPolicyPath = f.root() + QStringLiteral("/usr/share/polkit-old/actions/agency.cloaked.aetherserialbridge.policy");
        writeFile(o.legacyPolicyPath, "<old policy/>");
        QVERIFY(::chmod(enc(f.root() + QStringLiteral("/usr/share/polkit-old")).constData(), 0777) == 0);
        SystemdService s(f.bus, o, f.clock, f.sys);
        try {
            s.installOrConfigure(InstallState::NotInstalled, f.in(60s));
            QFAIL("installed with a world-writable legacy policy directory");
        } catch (const PlatformError& e) {
            QVERIFY2(QString::fromLocal8Bit(e.what()).contains(f.root() + QStringLiteral("/usr/share/polkit-old")), e.what());
        }
        QVERIFY(f.bus.calls.isEmpty());
        QVERIFY(QFileInfo::exists(o.legacyPolicyPath));
    }
    void uninstallRemovesTheLegacyPolicy()
    {
        Fixture f;
        f.service->installOrConfigure(InstallState::NotInstalled, f.in(60s));
        writeFile(f.legacyPolicy(), "<old policy/>");   // as if both were there
        f.service->takeChanges();
        f.service->uninstall(f.in(60s));
        QVERIFY(!QFileInfo::exists(f.opts().policyPath));
        QVERIFY(!QFileInfo::exists(f.legacyPolicy()));
        QVERIFY(f.service->takeChanges().contains(QStringLiteral("removed %1").arg(f.legacyPolicy())));
    }
    // Uninstalling with only the legacy policy present (a v1.1.0 installation, uninstalled by this build).
    void uninstallRemovesALegacyOnlyInstallation()
    {
        Fixture f;
        f.service->installOrConfigure(InstallState::NotInstalled, f.in(60s));
        QVERIFY(QFile::rename(f.opts().policyPath, f.legacyPolicy()));
        f.service->uninstall(f.in(60s));
        QVERIFY(!QFileInfo::exists(f.opts().policyPath));
        QVERIFY(!QFileInfo::exists(f.legacyPolicy()));
    }
    // Only with a policy to install (the CLI and the helper set it), and never the current policy itself.
    void legacyPolicyTouchedOnlyWithAPolicy()
    {
        Fixture f;
        writeFile(f.legacyPolicy(), "<old policy/>");
        SystemdService::Options o = f.opts();
        o.policy.clear();
        SystemdService none(f.bus, o, f.clock, f.sys);
        none.installOrConfigure(InstallState::NotInstalled, f.in(60s));
        QVERIFY(QFileInfo::exists(f.legacyPolicy()));
        none.uninstall(f.in(60s));
        QVERIFY(QFileInfo::exists(f.legacyPolicy()));
        o = f.opts();
        o.legacyPolicyPath = o.policyPath;
        SystemdService same(f.bus, o, f.clock, f.sys);
        same.installOrConfigure(InstallState::NotInstalled, f.in(60s));
        QVERIFY(QFileInfo::exists(o.policyPath));
    }

    // WS-16: uninstall also removes WS-15's staging root with its staging.* directories (a staged program running the
    // uninstall included); anything else in the root keeps it, named.
    void uninstallRemovesStagingRoot()
    {
        Fixture f;
        f.service->installOrConfigure(InstallState::NotInstalled, f.in(60s));
        const QString root = f.opts().stagingRoot;
        QVERIFY(QDir().mkpath(root + QStringLiteral("/staging.abc123")));
        writeFile(root + QStringLiteral("/staging.abc123/aether-serial-bridge"), "ELF");
        writeFile(root + QStringLiteral("/staging.abc123/request"), "{}");
        f.service->takeChanges();
        f.service->uninstall(f.in(60s));
        QVERIFY(!QFileInfo::exists(root));
        QVERIFY2(f.service->takeChanges().contains(QStringLiteral("removed %1").arg(root)), "the root is reported");

        Fixture k;
        k.service->installOrConfigure(InstallState::NotInstalled, k.in(60s));
        const QString kroot = k.opts().stagingRoot;
        QVERIFY(QDir().mkpath(kroot + QStringLiteral("/staging.x")));
        writeFile(kroot + QStringLiteral("/notes"), "mine");
        k.service->takeChanges();
        k.service->uninstall(k.in(60s));
        QVERIFY(!QFileInfo::exists(kroot + QStringLiteral("/staging.x")));
        QVERIFY(QFileInfo::exists(kroot + QStringLiteral("/notes")));
        QVERIFY(k.service->takeChanges().contains(QStringLiteral("kept %1 (it still holds notes)").arg(kroot)));
    }

    // The adapter runs only as root: as anyone else it refuses before talking to the manager.
    void refusesWithoutRoot()
    {
        if (::geteuid() == 0) QSKIP("running as root");
        Fixture f;
        SystemdService::Options o = f.opts();
        o.requireRoot = true;
        SystemdService s(f.bus, o, f.clock, f.sys);
        QVERIFY_THROWS_EXCEPTION(PlatformError, s.inhibitStarts(f.in(30s)));
        QVERIFY_THROWS_EXCEPTION(PlatformError, s.start(f.in(30s)));
        QVERIFY_THROWS_EXCEPTION(PlatformError, s.installOrConfigure(InstallState::NotInstalled, f.in(30s)));
        QVERIFY(f.bus.calls.isEmpty());
    }
};

QTEST_GUILESS_MAIN(tst_SystemdService)
#include "tst_SystemdService.moc"
