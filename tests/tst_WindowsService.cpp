// WindowsService at the SCM seam (WS-7; master §4.5 "SCM configuration", §4.6 installation states; INV-41, INV-77)
// and the install copy (tst_Install's rows live here too: INV-41 corruptedCopyRefused).

#include "platform/win/InstallBinary.h"
#include "platform/win/ServiceStatus.h"
#include "platform/win/WindowsService.h"
#include "support/FakeFileIo.h"
#include "support/FakeScm.h"

#include <QTest>

using namespace asb;
using namespace asb::test;
using namespace asb::win;

namespace {
const QString kLockDir = QStringLiteral(R"(C:\Program Files\AetherSerialBridge)");
const QString kExe = QStringLiteral(R"(C:\Program Files\AetherSerialBridge\aether-serial-bridge.exe)");
const QByteArray kImage = QByteArrayLiteral("MZ this build's headless executable");

struct Dir final : DirHandle {
    FileId id() const override { return {}; }
};

class Clock final : public IClock {
public:
    Deadline now() const override { return t; }
    void sleepFor(std::chrono::milliseconds d) override { t += d; }
    Deadline t = Deadline{} + std::chrono::hours(1);
};

struct Rig {
    FakeScm scm;
    FakeFileIo io;
    Dir dir;
    Clock clock;
    QByteArray image = kImage;
    WindowsService svc{scm,
                       WindowsService::Install{kLockDir, InstallBinary::sha256Hex(kImage),
                                               [this](QString*) { return std::optional<QByteArray>(image); }, &io,
                                               [this]() -> const DirHandle& { return dir; }},
                       clock};
    Deadline later() const { return clock.now() + std::chrono::seconds(30); }

    void installedQt(const QByteArray& installed = kImage, bool lockFiles = true)
    {
        // As installOrConfigure leaves it (then activated: Automatic).
        const ServicePlan::ScmConfig c = ServicePlan::windowsConfig(kExe);
        scm.service = FakeScm::Service{c.serviceName, c.displayName, c.binaryPath, c.account, c.description, scm::kAutoStart,
                                       c.failureActions, c.failureResetSeconds, c.failureActionsOnNonCrashFailures, {1, 0, 0, 0}};
        scm.files[kExe.toLower()] = installed;
        if (lockFiles) {
            scm.files[(kLockDir + QStringLiteral("\\engine.lock")).toLower()] = "{}";
            scm.files[(kLockDir + QStringLiteral("\\engine.lock.id")).toLower()] = "{}";
        }
    }
};

int indexOf(const QStringList& log, const QString& prefix, int from = 0)
{
    for (int i = from; i < log.size(); ++i)
        if (log[i].startsWith(prefix)) return i;
    return -1;
}
}  // namespace

class tst_WindowsService : public QObject {
    Q_OBJECT

private slots:
    void programOfBinPath_data()
    {
        QTest::addColumn<QString>("binPath");
        QTest::addColumn<QString>("program");
        QTest::newRow("quoted") << QStringLiteral(R"("C:\Program Files\X\a.exe" --service)") << QStringLiteral(R"(C:\Program Files\X\a.exe)");
        QTest::newRow("unquoted with spaces") << QStringLiteral(R"(C:\Program Files\X\a.exe --service)") << QStringLiteral(R"(C:\Program Files\X\a.exe)");
        QTest::newRow("no args") << QStringLiteral(R"(C:\x\b.exe)") << QStringLiteral(R"(C:\x\b.exe)");
    }
    void programOfBinPath()
    {
        QFETCH(QString, binPath);
        QFETCH(QString, program);
        QCOMPARE(WindowsService::programOf(binPath), program);
    }

    void detectStates()
    {
        {
            Rig r;
            const Installation i = r.svc.detect(r.later());
            QCOMPARE(i.state, InstallState::NotInstalled);
            QVERIFY(!i.programRemains);   // a clean machine: the window offers no Uninstall
        }
        {
            Rig r;   // the service removed, the program kept (an uninstall to finish)
            r.installedQt();
            r.scm.service.reset();
            const Installation i = r.svc.detect(r.later());
            QCOMPARE(i.state, InstallState::NotInstalled);
            QVERIFY(i.programRemains);
        }
        {
            Rig r;
            r.installedQt();
            QCOMPARE(r.svc.detect(r.later()).state, InstallState::Current);
        }
        {
            Rig r;   // another build's exe at the Qt path: the install must be finished again (INV-41)
            r.installedQt(QByteArrayLiteral("MZ another build"));
            const Installation i = r.svc.detect(r.later());
            QCOMPARE(i.state, InstallState::IncompleteInstall);
        }
        {
            Rig r;
            r.installedQt(kImage, /*lockFiles=*/false);
            QCOMPARE(r.svc.detect(r.later()).state, InstallState::IncompleteInstall);
        }
        {
            Rig r;   // another program under the service name, with the Qt exe and locks beside it: Foreign (refused)
            const QString other = QStringLiteral(R"(C:\Program Files\AetherSerialBridge\bridge.exe)");
            r.installedQt();
            r.scm.service->binaryPath = QStringLiteral("\"%1\" --service").arg(other);
            r.scm.files[other.toLower()] = kImage;
            QCOMPARE(r.svc.detect(r.later()).state, InstallState::Foreign);
            // Never executed: no file call starts anything.
            for (const QString& l : r.scm.log) QVERIFY(!l.startsWith(QLatin1String("start")));
        }
        {
            Rig r;
            r.scm.fail[QStringLiteral("open")] = scm::kErrorAccessDenied;
            QVERIFY_THROWS_EXCEPTION(PlatformError, r.svc.detect(r.later()));
        }
    }

    // INV-77: every start-type change is followed by RegFlushKey before anything else.
    void startTypeChangesAreFlushed()
    {
        Rig r;
        r.installedQt();
        r.svc.inhibitStarts(r.later());
        const int d = indexOf(r.scm.log, QStringLiteral("startType 4"));
        QVERIFY(d >= 0);
        QCOMPARE(r.scm.log.value(d + 1), QStringLiteral("flush AetherSerialBridge"));
        r.scm.log.clear();
        r.svc.allowStarts(r.later());
        const int a = indexOf(r.scm.log, QStringLiteral("startType 2"));
        QVERIFY(a >= 0);
        QCOMPARE(r.scm.log.value(a + 1), QStringLiteral("flush AetherSerialBridge"));
        // A failed flush is a step failure.
        r.scm.fail[QStringLiteral("flush")] = 1;
        QVERIFY_THROWS_EXCEPTION(PlatformError, r.svc.inhibitStarts(r.later()));
    }

    void notInstalledStopAndDisableAreNoOps()
    {
        Rig r;
        r.svc.inhibitStarts(r.later());
        r.svc.requestStop(r.later());
        r.svc.uninstall(r.later());
        QCOMPARE(r.svc.query(r.later()).state, ServiceQuery::Absent);
        QVERIFY_THROWS_EXCEPTION(PlatformError, r.svc.allowStarts(r.later()));
    }

    // WS-16 (D69): uninstall() removes the service only; the program goes in removeProgram(), which the Applier calls
    // after the lock files were marked. A failed removal keeps the exe under its own name and says why. Kills
    // inv126-program-removed-before-lock-cleanup at the adapter (the exe removed with the service).
    void uninstallRemovesOnlyTheService()
    {
        Rig r;
        r.svc.installOrConfigure(InstallState::NotInstalled, r.later());
        QVERIFY(r.io.exists(QStringLiteral("aether-serial-bridge.exe")));
        r.svc.uninstall(r.later());
        QVERIFY(!r.scm.service);
        QVERIFY(r.io.exists(QStringLiteral("aether-serial-bridge.exe")));
        QVERIFY(r.svc.takeChanges().isEmpty());
        const IServiceControl::ProgramRemoval removed = r.svc.removeProgram(r.later());
        QCOMPARE(int(removed.kind), int(IServiceControl::ProgramRemoval::Kind::Removed));
        QVERIFY(!r.io.exists(QStringLiteral("aether-serial-bridge.exe")));
        QCOMPARE(removed.changes, QStringList{QStringLiteral("removed %1").arg(kExe)});
        QCOMPARE(int(r.svc.removeProgram(r.later()).kind), int(IServiceControl::ProgramRemoval::Kind::Absent));

        Rig k;
        k.svc.installOrConfigure(InstallState::NotInstalled, k.later());
        k.io.failNext(FakeFileIo::Op::Remove, QStringLiteral("the file is in use"));
        const IServiceControl::ProgramRemoval kept = k.svc.removeProgram(k.later());
        QCOMPARE(int(kept.kind), int(IServiceControl::ProgramRemoval::Kind::Kept));
        QCOMPARE(kept.error, QStringLiteral("the file is in use"));
        QVERIFY(k.io.exists(QStringLiteral("aether-serial-bridge.exe")));
        QCOMPARE(kept.changes, QStringList{QStringLiteral("kept %1 (the file is in use)").arg(kExe)});

        Rig t;   // the test seam (ASB_TEST_FAIL_PROGRAM_REMOVAL): kept, nothing touched
        t.svc.installOrConfigure(InstallState::NotInstalled, t.later());
        WindowsService::Lifecycle l;
        l.injectedRemovalFailure = [] { return QStringLiteral("injected"); };
        t.svc.setLifecycle(l);
        QCOMPARE(int(t.svc.removeProgram(t.later()).kind), int(IServiceControl::ProgramRemoval::Kind::Kept));
        QVERIFY(t.io.exists(QStringLiteral("aether-serial-bridge.exe")));
    }

    // WS-16: the program this process runs from is renamed aside in its folder and scheduled for deletion at restart
    // (counts as removed); then the folder is scheduled after it. A rename that fails keeps the program. Kills
    // inv125-running-image-kept-not-scheduled at the adapter.
    void runningImageRenamedAsideAndScheduled()
    {
        Rig r;
        r.svc.installOrConfigure(InstallState::NotInstalled, r.later());
        QStringList renamed, scheduled;
        WindowsService::Lifecycle l;
        l.isOwnImage = [](const QString& p) { return p == kExe; };
        l.randomSuffix = [] { return QStringLiteral("0123456789abcdef"); };
        l.rename = [&](const QString& from, const QString& to, QString*) {
            renamed << from + QStringLiteral(" -> ") + to;
            return true;
        };
        l.deleteAtRestart = [&](const QString& p, QString*) {
            scheduled << p;
            return true;
        };
        l.removeDirectory = [](const QString&, QString* err) {
            *err = QStringLiteral("not empty");
            return false;
        };
        l.children = [] { return QStringList{QStringLiteral("aether-serial-bridge.exe.removed-0123456789abcdef")}; };
        r.svc.setLifecycle(l);
        const IServiceControl::ProgramRemoval pr = r.svc.removeProgram(r.later());
        QCOMPARE(int(pr.kind), int(IServiceControl::ProgramRemoval::Kind::ScheduledAtRestart));
        const QString aside = kExe + QStringLiteral(".removed-0123456789abcdef");
        QCOMPARE(renamed, QStringList{kExe + QStringLiteral(" -> ") + aside});
        QCOMPARE(scheduled, QStringList{aside});
        QVERIFY2(pr.changes.join(QLatin1Char('|')).contains(QStringLiteral("deleted when Windows restarts")), qPrintable(pr.changes.join(QLatin1Char('|'))));
        // Only the scheduled image is left: the folder is scheduled after it, never removed while it holds anything.
        QCOMPARE(r.svc.removeProgramFolder(), QStringList{QStringLiteral("%1 is removed when Windows restarts").arg(kLockDir)});
        QCOMPARE(scheduled, (QStringList{aside, kLockDir}));

        Rig k;   // the rename fails: kept under its own name, nothing scheduled
        k.svc.installOrConfigure(InstallState::NotInstalled, k.later());
        WindowsService::Lifecycle kl = l;
        kl.rename = [](const QString&, const QString&, QString* err) {
            *err = QStringLiteral("sharing violation");
            return false;
        };
        k.svc.setLifecycle(kl);
        scheduled.clear();
        const IServiceControl::ProgramRemoval kept = k.svc.removeProgram(k.later());
        QCOMPARE(int(kept.kind), int(IServiceControl::ProgramRemoval::Kind::Kept));
        QVERIFY(scheduled.isEmpty());
        QVERIFY(k.io.exists(QStringLiteral("aether-serial-bridge.exe")));
    }

    // The renamed image counts as removed only once its deletion at restart is registered. When the
    // registration fails it is renamed back and kept; when that fails too it is kept under the new name, which the
    // change line and the error name with what to do. Either way nothing is scheduled, so the folder is not either.
    // Kills ws16-image-scheduled-before-registration.
    void runningImageNotScheduledIsKept()
    {
        QStringList renamed, attempted;
        bool renameBackFails = false;
        WindowsService::Lifecycle l;
        l.isOwnImage = [](const QString& p) { return p == kExe; };
        l.randomSuffix = [] { return QStringLiteral("0123456789abcdef"); };
        l.rename = [&](const QString& from, const QString& to, QString* err) {
            if (renameBackFails && !renamed.isEmpty()) {
                *err = QStringLiteral("access denied");
                return false;
            }
            renamed << from + QStringLiteral(" -> ") + to;
            return true;
        };
        l.deleteAtRestart = [&](const QString& p, QString* err) {
            attempted << p;
            *err = QStringLiteral("the registry refused it");
            return false;
        };
        QStringList children;
        l.children = [&] { return children; };
        const QString aside = kExe + QStringLiteral(".removed-0123456789abcdef");

        Rig r;   // renamed back: kept under its own name
        r.svc.installOrConfigure(InstallState::NotInstalled, r.later());
        r.svc.setLifecycle(l);
        const IServiceControl::ProgramRemoval back = r.svc.removeProgram(r.later());
        QCOMPARE(int(back.kind), int(IServiceControl::ProgramRemoval::Kind::Kept));
        QCOMPARE(renamed, (QStringList{kExe + QStringLiteral(" -> ") + aside, aside + QStringLiteral(" -> ") + kExe}));
        QCOMPARE(attempted, QStringList{aside});
        QCOMPARE(back.error, QStringLiteral("it could not be scheduled for deletion when Windows restarts (the registry refused it)"));
        QCOMPARE(back.changes, QStringList{QStringLiteral("kept %1 (%2)").arg(kExe, back.error)});
        children = {QStringLiteral("aether-serial-bridge.exe")};
        QCOMPARE(r.svc.removeProgramFolder(),
                 QStringList{QStringLiteral("kept %1 (it still holds aether-serial-bridge.exe)").arg(kLockDir)});
        QCOMPARE(attempted, QStringList{aside});   // the folder was not scheduled after an unscheduled image

        Rig s;   // not renamed back either: kept under the new name, which is named with the way out
        s.svc.installOrConfigure(InstallState::NotInstalled, s.later());
        renamed.clear();
        attempted.clear();
        renameBackFails = true;
        s.svc.setLifecycle(l);
        const IServiceControl::ProgramRemoval stuck = s.svc.removeProgram(s.later());
        QCOMPARE(int(stuck.kind), int(IServiceControl::ProgramRemoval::Kind::Kept));
        QCOMPARE(renamed, QStringList{kExe + QStringLiteral(" -> ") + aside});
        QVERIFY2(stuck.error.contains(aside) && stuck.error.contains(QStringLiteral("the registry refused it")) &&
                     stuck.error.contains(QStringLiteral("access denied")) &&
                     stuck.error.contains(QStringLiteral("delete it after the next restart")),
                 qPrintable(stuck.error));
        QCOMPARE(stuck.changes, QStringList{QStringLiteral("kept %1 (%2)").arg(aside, stuck.error)});
        QVERIFY(!stuck.changes.join(QLatin1Char('|')).contains(QStringLiteral("deleted when Windows restarts")));
        children = {QStringLiteral("aether-serial-bridge.exe.removed-0123456789abcdef")};
        QCOMPARE(s.svc.removeProgramFolder(),
                 QStringList{QStringLiteral("kept %1 (it still holds aether-serial-bridge.exe.removed-0123456789abcdef)")
                                 .arg(kLockDir)});
        QCOMPARE(attempted, QStringList{aside});
    }

    // WS-16: the folder goes only when nothing is in it; anything else keeps it, named. Never recursive.
    void programFolderRemovedOnlyWhenEmpty()
    {
        Rig r;
        QStringList removedDirs;
        WindowsService::Lifecycle l;
        l.removeDirectory = [&](const QString& p, QString*) {
            removedDirs << p;
            return true;
        };
        QStringList children{QStringLiteral("planted.txt")};
        l.children = [&] { return children; };
        r.svc.setLifecycle(l);
        QCOMPARE(r.svc.removeProgramFolder(), QStringList{QStringLiteral("kept %1 (it still holds planted.txt)").arg(kLockDir)});
        QVERIFY(removedDirs.isEmpty());
        children.clear();
        QCOMPARE(r.svc.removeProgramFolder(), QStringList{QStringLiteral("removed %1").arg(kLockDir)});
        QCOMPARE(removedDirs, QStringList{kLockDir});
    }

    // WS-16: install removes the "*.removed-*" images an earlier uninstall left, and nothing else.
    void installRemovesLeftoverImages()
    {
        Rig r;
        r.io.plantFile(QStringLiteral("aether-serial-bridge.exe.removed-0123456789abcdef"), "old", QStringLiteral("root"));
        r.io.plantFile(QStringLiteral("notes.txt"), "x", QStringLiteral("root"));
        WindowsService::Lifecycle l;
        l.children = [&] { return r.io.names(); };
        r.svc.setLifecycle(l);
        r.svc.installOrConfigure(InstallState::NotInstalled, r.later());
        QVERIFY(!r.io.exists(QStringLiteral("aether-serial-bridge.exe.removed-0123456789abcdef")));
        QVERIFY(r.io.exists(QStringLiteral("notes.txt")));
        QVERIFY(r.io.exists(QStringLiteral("aether-serial-bridge.exe")));
    }

    // WS-16 (D63): the entry's facts are this install's: the folder, the installed exe, this build's version, the size
    // of the copy and the date of the write.
    void appsEntryFactsAreThisInstall()
    {
        struct Recorder final : IUninstallEntry {
            QList<Facts> writes;
            int removes = 0;
            QString write(const Facts& f) override
            {
                writes << f;
                return {};
            }
            QString remove() override
            {
                ++removes;
                return {};
            }
            bool present() override { return removes == 0 && !writes.isEmpty(); }
        } entry;
        Rig r;
        WindowsService::Lifecycle l;
        l.entry = &entry;
        l.version = QStringLiteral("1.1.0");
        l.today = [] { return QDate(2026, 10, 17); };
        r.svc.setLifecycle(l);
        r.svc.installOrConfigure(InstallState::NotInstalled, r.later());
        QVERIFY(r.svc.writeAppsEntry().isEmpty());
        QCOMPARE(entry.writes.size(), 1);
        QCOMPARE(entry.writes[0].installLocation, kLockDir);
        QCOMPARE(entry.writes[0].installedExe, kExe);
        QCOMPARE(entry.writes[0].version, QStringLiteral("1.1.0"));
        QCOMPARE(entry.writes[0].exeBytes, qint64(kImage.size()));
        QCOMPARE(entry.writes[0].date, QDate(2026, 10, 17));
        r.svc.removeAppsEntry(r.later());
        QCOMPARE(entry.removes, 1);
    }

    void installCopiesThenConfiguresDisabledWithRecovery()
    {
        Rig r;
        r.svc.installOrConfigure(InstallState::NotInstalled, r.later());
        QCOMPARE(r.io.contents(QStringLiteral("aether-serial-bridge.exe")), kImage);
        QVERIFY(r.scm.service);
        const auto& s = *r.scm.service;
        QCOMPARE(s.startType, scm::kDisabled);   // created with starts blocked
        QCOMPARE(s.binaryPath, QStringLiteral("\"%1\" --service").arg(kExe));
        QCOMPARE(s.account, QStringLiteral("LocalSystem"));
        QCOMPARE(s.displayName, QStringLiteral("Aether Serial Bridge"));
        QVERIFY(!s.description.isEmpty());
        QCOMPARE(s.failureActions.size(), 3);
        QCOMPARE(s.failureActions[0].delayMs, 5000);
        QCOMPARE(s.failureActions[1].delayMs, 5000);
        QCOMPARE(s.failureActions[2].delayMs, 30000);
        QCOMPARE(s.resetSeconds, 86400);
        QVERIFY(s.failureFlag);
        QCOMPARE(r.scm.log.last(), QStringLiteral("close"));
        QVERIFY(indexOf(r.scm.log, QStringLiteral("flush")) > indexOf(r.scm.log, QStringLiteral("failureFlag")));
        QCOMPARE(r.scm.openHandles, 0);
        // Again over an existing (enabled) service: reconfigured, disabled again.
        r.scm.service->startType = scm::kAutoStart;
        r.scm.log.clear();
        r.svc.installOrConfigure(InstallState::Current, r.later());
        QVERIFY(indexOf(r.scm.log, QStringLiteral("config startType 4")) >= 0);
        QCOMPARE(r.scm.service->startType, scm::kDisabled);
    }

    // INV-41 (tst_Install::corruptedCopyRefused): the hash is of the COPY, read back through its own handle. A copy
    // corrupted after it was written is refused and nothing is published or configured.
    void corruptedCopyRefused()
    {
        FakeFileIo io;
        Dir dir;
        const InstallBinary::Result ok = InstallBinary::install(io, dir, QStringLiteral("x.exe"), kImage, InstallBinary::sha256Hex(kImage));
        QVERIFY2(ok.ok, qPrintable(ok.error));
        QCOMPARE(io.contents(QStringLiteral("x.exe")), kImage);

        FakeFileIo io2;
        const InstallBinary::Result bad = InstallBinary::install(
            io2, dir, QStringLiteral("x.exe"), kImage, InstallBinary::sha256Hex(kImage), [&] {
                const QStringList temps = io2.tempNames();
                QCOMPARE(temps.size(), 1);
                QString err;
                auto f = io2.openExisting(dir, temps.front(), &err);
                QVERIFY(f);
                QVERIFY(f->truncate(&err));
                QVERIFY(f->writeAll(QByteArrayLiteral("MZ swapped"), &err));
            });
        QVERIFY(!bad.ok);
        QVERIFY(!io2.exists(QStringLiteral("x.exe")));
        QVERIFY(io2.tempNames().isEmpty());   // the temp is deleted

        // Through the service: a corrupted copy is refused before the SCM is touched.
        Rig r;
        r.image = QByteArrayLiteral("MZ not this build");
        QVERIFY_THROWS_EXCEPTION(PlatformError, r.svc.installOrConfigure(InstallState::NotInstalled, r.later()));
        QVERIFY(!r.scm.service);
        QVERIFY(!r.io.exists(QStringLiteral("aether-serial-bridge.exe")));
    }

    void requestStopVariants()
    {
        {
            Rig r;
            r.installedQt();   // stopped
            r.svc.requestStop(r.later());   // not running: not an error
        }
        {
            Rig r;
            r.installedQt();
            r.scm.service->status.state = svc::kRunning;
            r.svc.requestStop(r.later());
            QCOMPARE(r.scm.service->status.state, svc::kStopPending);
        }
        {
            Rig r;   // START_PENDING accepts no control: retried until it takes
            r.installedQt();
            r.scm.service->status.state = svc::kStartPending;
            int tries = 0;
            r.scm.onControl = [&](quint32) -> quint32 {
                if (++tries == 1) return scm::kErrorInvalidServiceControl;   // what SCM answers in START_PENDING
                if (tries < 3) return scm::kErrorCannotAcceptControl;
                r.scm.service->status.state = svc::kStopPending;
                return 0;
            };
            r.svc.requestStop(r.later());
            QCOMPARE(tries, 3);
        }
        {
            Rig r;   // ... but never past the deadline
            r.installedQt();
            r.scm.service->status.state = svc::kStartPending;
            r.scm.onControl = [](quint32) -> quint32 { return scm::kErrorCannotAcceptControl; };
            const Deadline d = r.later();
            r.svc.requestStop(d);
            QVERIFY(r.clock.now() >= d);
            QVERIFY(r.clock.now() < d + std::chrono::seconds(1));
        }
        {
            Rig r;
            r.installedQt();
            r.scm.service->status.state = svc::kRunning;
            r.scm.onControl = [](quint32) -> quint32 { return scm::kErrorAccessDenied; };
            QVERIFY_THROWS_EXCEPTION(PlatformError, r.svc.requestStop(r.later()));
        }
    }

    void startAndUninstall()
    {
        Rig r;
        r.installedQt();
        r.svc.start(r.later());
        QCOMPARE(r.svc.query(r.later()).state, ServiceQuery::Running);
        QCOMPARE(r.svc.query(r.later()).pid, qint64(4242));
        r.svc.start(r.later());   // already running: not an error
        r.scm.service->startType = scm::kDisabled;
        r.scm.service->status.state = svc::kStopped;
        QVERIFY_THROWS_EXCEPTION(PlatformError, r.svc.start(r.later()));
        r.svc.uninstall(r.later());
        QVERIFY(!r.scm.service);
        r.svc.uninstall(r.later());   // does not exist: not an error
    }

    // WS-5 round 3: pid 0 is a promise that no engine process exists. A process of the installed exe that
    // outlived its STOPPED report (or its service) is reported.
    void queryReportsAProcessThatOutlivesItsStop()
    {
        Rig r;
        r.installedQt();   // STOPPED, SCM pid 0
        QCOMPARE(r.svc.query(r.later()).pid, qint64(0));
        r.scm.processes[kExe.toLower()] = {777};
        const ServiceQuery q = r.svc.query(r.later());
        QCOMPARE(q.state, ServiceQuery::Stopped);
        QCOMPARE(q.pid, qint64(777));
        r.scm.service.reset();   // deleted under it
        QCOMPARE(r.svc.query(r.later()).state, ServiceQuery::Absent);
        QCOMPARE(r.svc.query(r.later()).pid, qint64(777));
        r.scm.processes.clear();
        QCOMPARE(r.svc.query(r.later()).pid, qint64(0));
    }

    // Review round 1: the survivor enumeration fails closed. A list that could not be completed is a failed
    // query (the Applier's failClosed), never pid 0 ("no engine process"), with SCM's STOPPED and with no service.
    void querySurvivorsUnknownFailsTheQuery()
    {
        Rig r;
        r.installedQt();   // STOPPED, SCM pid 0: the enumeration decides
        r.scm.processes[kExe.toLower()] = {};
        QCOMPARE(r.svc.query(r.later()).pid, qint64(0));   // complete and empty: the promise holds
        r.scm.fail[QStringLiteral("processes")] = scm::kErrorAccessDenied;
        QVERIFY_THROWS_EXCEPTION(PlatformError, r.svc.query(r.later()));
        r.scm.service.reset();   // Absent: the same
        QVERIFY_THROWS_EXCEPTION(PlatformError, r.svc.query(r.later()));
        r.scm.fail.clear();
        QCOMPARE(r.svc.query(r.later()).state, ServiceQuery::Absent);
    }

    // A forward step whose blocking call returned after the deadline changes
    // nothing more. Each row makes one call return late and names the change that must not follow it.
    void forwardStepsStopWhenACallReturnsLate_data()
    {
        QTest::addColumn<QString>("op");
        QTest::addColumn<bool>("installed");
        QTest::addColumn<QString>("late");    // the call (log prefix) that returns after the deadline
        QTest::addColumn<QString>("never");   // a call that must not follow it
        QTest::newRow("allowStarts: late open") << "allowStarts" << true << "open" << "startType 2";
        QTest::newRow("allowStarts: late start type") << "allowStarts" << true << "startType 2" << "flush";
        QTest::newRow("start: late open") << "start" << true << "open" << "start";
        QTest::newRow("uninstall: late open") << "uninstall" << true << "open" << "delete";
        QTest::newRow("install: late open, new service") << "install" << false << "open" << "create";
        QTest::newRow("install: late open, existing service") << "install" << true << "open" << "config";
        QTest::newRow("install: late create") << "install" << false << "create" << "description";
        QTest::newRow("install: late description") << "install" << false << "description" << "failureActions";
        QTest::newRow("install: late recovery actions") << "install" << false << "failureActions" << "failureFlag";
        QTest::newRow("install: late recovery flag") << "install" << false << "failureFlag" << "flush";
    }
    void forwardStepsStopWhenACallReturnsLate()
    {
        QFETCH(QString, op);
        QFETCH(bool, installed);
        QFETCH(QString, late);
        QFETCH(QString, never);
        Rig r;
        if (installed) r.installedQt();
        const Deadline d = r.later();
        bool fired = false;
        r.scm.onCall = [&](const QString& call) {
            if (!fired && call.startsWith(late)) {
                fired = true;
                r.clock.t = d + std::chrono::seconds(1);
            }
        };
        r.scm.log.clear();
        auto run = [&] {
            if (op == QLatin1String("allowStarts")) r.svc.allowStarts(d);
            else if (op == QLatin1String("start")) r.svc.start(d);
            else if (op == QLatin1String("uninstall")) r.svc.uninstall(d);
            else r.svc.installOrConfigure(installed ? InstallState::Current : InstallState::NotInstalled, d);
        };
        QVERIFY_THROWS_EXCEPTION(PlatformError, run());
        QVERIFY(fired);
        QVERIFY2(indexOf(r.scm.log, never, indexOf(r.scm.log, late) + 1) < 0, qPrintable(r.scm.log.join(QLatin1Char('|'))));
        QCOMPARE(r.scm.openHandles, 0);
    }

    // ... the install copy: an image read that returns late writes nothing; a late copy is not published.
    void installCopyStopsWhenLate()
    {
        {
            Rig r;
            const Deadline d = r.later();
            WindowsService svc{r.scm,
                               WindowsService::Install{kLockDir, InstallBinary::sha256Hex(kImage),
                                                       [&](QString*) {
                                                           r.clock.t = d + std::chrono::seconds(1);   // a slow read
                                                           return std::optional<QByteArray>(kImage);
                                                       },
                                                       &r.io, [&r]() -> const DirHandle& { return r.dir; }},
                               r.clock};
            QVERIFY_THROWS_EXCEPTION(PlatformError, svc.installOrConfigure(InstallState::NotInstalled, d));
            QVERIFY2(r.io.log.filter(QStringLiteral("write ")).isEmpty(), qPrintable(r.io.log.join(QLatin1Char('|'))));   // not even a temp
            QVERIFY(r.io.tempNames().isEmpty());
            QVERIFY(!r.io.exists(QStringLiteral("aether-serial-bridge.exe")));
            QVERIFY(r.scm.log.isEmpty());
        }
        {
            Rig r;   // the copy's read-back returns late: verified, but never published
            const Deadline d = r.later();
            r.io.onOp = [&](FakeFileIo::Op op) {
                if (op == FakeFileIo::Op::ReadBack) r.clock.t = d + std::chrono::seconds(1);
            };
            QVERIFY_THROWS_EXCEPTION(PlatformError, r.svc.installOrConfigure(InstallState::NotInstalled, d));
            QVERIFY(!r.io.exists(QStringLiteral("aether-serial-bridge.exe")));
            QVERIFY(r.io.tempNames().isEmpty());
            QVERIFY(r.scm.log.isEmpty());
        }
        {
            FakeFileIo io;
            Dir dir;
            QVERIFY_THROWS_EXCEPTION(PlatformError,
                                     InstallBinary::install(io, dir, QStringLiteral("x.exe"), kImage, InstallBinary::sha256Hex(kImage),
                                                            {}, [] { throw PlatformError("the deadline passed"); }));
            QVERIFY(!io.exists(QStringLiteral("x.exe")));   // verified, but never published
            QVERIFY(io.tempNames().isEmpty());              // and the temp is gone
        }
    }

    // Review round 2: the install copy replaced the program, but the directory flush after the rename
    // failed. The install stops there, before any SCM change, and the failure names what was replaced (the INV-76
    // shape), over a fresh service and over an existing one (a reinstall or upgrade).
    void installStopsWhenTheReplacementIsNotDurable_data()
    {
        QTest::addColumn<bool>("installed");
        QTest::newRow("fresh install") << false;
        QTest::newRow("reinstall over an existing service") << true;
    }
    void installStopsWhenTheReplacementIsNotDurable()
    {
        QFETCH(bool, installed);
        Rig r;
        if (installed) r.installedQt(QByteArrayLiteral("MZ the previous build"));
        r.scm.log.clear();
        r.io.failNext(FakeFileIo::Op::SyncDir, QStringLiteral("injected directory flush failure"));
        bool threw = false;
        try {
            r.svc.installOrConfigure(installed ? InstallState::IncompleteInstall : InstallState::NotInstalled, r.later());
        } catch (const PlatformErrorAfterChanges& e) {
            threw = true;
            QCOMPARE(e.changes, QStringList{QStringLiteral("replaced %1").arg(kExe)});
            QVERIFY2(QString::fromUtf8(e.what()).contains(QLatin1String("not durable")), e.what());
            QVERIFY2(QString::fromUtf8(e.what()).contains(QLatin1String("injected directory flush failure")), e.what());
        }
        QVERIFY(threw);
        QCOMPARE(r.io.contents(QStringLiteral("aether-serial-bridge.exe")), kImage);   // it was replaced
        QVERIFY2(r.scm.log.isEmpty(), qPrintable(r.scm.log.join(QLatin1Char('|'))));    // and nothing followed
    }

    // Review round 3: a service created and then left half configured (a reinstall that failed or ran
    // late after CreateService) is IncompleteInstall, never Current, so Start and Apply refuse it with the advice to
    // finish the install. Every setting installOrConfigure writes is judged.
    // The recovery judgement compares the whole failure-action list, types included: a reboot or a command among
    // restarts with the right delays is not the plan (the native read-back keeps every SC_ACTION type).
    void recoveryWithAnotherActionTypeIsIncomplete()
    {
        using T = ServicePlan::FailureAction::Type;
        for (T other : {T::Reboot, T::RunCommand, T::None, T::Other}) {
            Rig r;
            r.installedQt();
            QCOMPARE(r.svc.detect(r.later()).state, InstallState::Current);
            r.scm.service->failureActions[2].type = other;   // same delay (30 s), another action
            const Installation i = r.svc.detect(r.later());
            QCOMPARE(i.state, InstallState::IncompleteInstall);
            QVERIFY2(i.detail.contains(QLatin1String("recovery")), qPrintable(i.detail));
        }
    }

    void halfConfiguredServiceIsIncomplete_data()
    {
        QTest::addColumn<QString>("stopAt");   // the call that fails during the reinstall
        QTest::newRow("before the description") << "description";
        QTest::newRow("before the recovery actions") << "failureActions";
        QTest::newRow("before the recovery flag") << "failureFlag";
    }
    void halfConfiguredServiceIsIncomplete()
    {
        QFETCH(QString, stopAt);
        Rig r;
        r.installedQt();
        QCOMPARE(r.svc.detect(r.later()).state, InstallState::Current);   // complete
        r.scm.service.reset();                                           // uninstalled; files and lock pair kept
        r.scm.fail[stopAt] = scm::kErrorAccessDenied;
        QVERIFY_THROWS_EXCEPTION(PlatformError, r.svc.installOrConfigure(InstallState::NotInstalled, r.later()));
        QVERIFY(r.scm.service);   // CreateService succeeded
        r.scm.fail.clear();
        const Installation i = r.svc.detect(r.later());
        QCOMPARE(i.state, InstallState::IncompleteInstall);
        QVERIFY2(i.detail.contains(QLatin1String("not fully configured")), qPrintable(i.detail));
        QVERIFY2(i.detail.contains(QLatin1String("recovery")), qPrintable(i.detail));
        // A finished install makes it Current again.
        r.svc.installOrConfigure(InstallState::IncompleteInstall, r.later());
        QCOMPARE(r.svc.detect(r.later()).state, InstallState::Current);
    }
    void eachScmSettingIsJudged_data()
    {
        QTest::addColumn<QString>("setting");
        QTest::newRow("program path") << "program path";
        QTest::newRow("account") << "account";
        QTest::newRow("display name") << "display name";
        QTest::newRow("start type") << "start type";
        QTest::newRow("recovery delays") << "recovery actions";
        QTest::newRow("recovery reset period") << "recovery reset";
        QTest::newRow("recovery flag") << "recovery on a non-zero exit";
    }
    void eachScmSettingIsJudged()
    {
        QFETCH(QString, setting);
        Rig r;
        r.installedQt();
        auto& s = *r.scm.service;
        QString expect = setting;
        if (setting == QLatin1String("program path")) s.binaryPath = QStringLiteral("\"%1\" --service --other").arg(kExe);
        else if (setting == QLatin1String("account")) s.account = QStringLiteral("NT AUTHORITY\\LocalService");
        else if (setting == QLatin1String("display name")) s.displayName = QStringLiteral("Something else");
        else if (setting == QLatin1String("start type")) s.startType = scm::kDemandStart;
        else if (setting == QLatin1String("recovery actions")) s.failureActions.removeLast();
        else if (setting == QLatin1String("recovery reset")) { s.resetSeconds = 0; expect = QStringLiteral("recovery actions"); }
        else s.failureFlag = false;
        const Installation i = r.svc.detect(r.later());
        if (setting == QLatin1String("program path")) {
            QCOMPARE(i.state, InstallState::Foreign);   // another binPath is not this installation at all
            return;
        }
        QCOMPARE(i.state, InstallState::IncompleteInstall);
        QVERIFY2(i.detail.contains(expect), qPrintable(i.detail));
    }

    // ... while the fail-closed calls complete whatever the time: they are what failClosed runs after a deadline.
    void failClosedCallsIgnoreTheDeadline()
    {
        Rig r;
        r.installedQt();
        r.scm.service->status.state = svc::kRunning;
        const Deadline past = r.clock.now() - std::chrono::seconds(1);
        r.svc.inhibitStarts(past);
        QCOMPARE(r.scm.service->startType, scm::kDisabled);
        QVERIFY(indexOf(r.scm.log, QStringLiteral("flush")) >= 0);
        r.svc.requestStop(past);
        QCOMPARE(r.scm.service->status.state, svc::kStopPending);
    }
};

QTEST_GUILESS_MAIN(tst_WindowsService)
#include "tst_WindowsService.moc"
