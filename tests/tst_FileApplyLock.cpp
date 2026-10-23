// WS-6: apply.lock and engine.lock on the recording NtSeams fake (INV-66, INV-97, INV-104, INV-116). The native
// rows (other processes, real users, a killed holder) are tests/win/integration/tst_WinLocksNative.
#include "core/LockIdentity.h"
#include "platform/win/EngineLock.h"
#include "platform/win/FileApplyLock.h"
#include "support/Fakes.h"
#include "support/WinFakeFixture.h"

#include <QTest>

using namespace asb;
using namespace asb::win;
using namespace asb::test;
using namespace asb::test::win6;

namespace {
const QString kApply = kLockDir + QStringLiteral("\\apply.lock");
const QString kEngine = kLockDir + QStringLiteral("\\engine.lock");
const QString kRecord = kLockDir + QStringLiteral("\\engine.lock.id");

QByteArray wide(const QString& owner)
{
    return descriptor(owner, {allow(kSy, sd::kFullControl), allow(kBa, sd::kFullControl), allow(kBu, sd::kFullControl)});
}
}  // namespace

class tst_FileApplyLock : public QObject {
    Q_OBJECT

    struct World {
        FakeNtSeams fs;
        FakeClock clock;
        QStringList reports;
        World() { setUpMachine(fs); }
        FileApplyLock applyLock() { return FileApplyLock(fs, kLockDir, clock, [this](const QString& r) { reports << r; }); }
        std::unique_ptr<EngineLock> engineLock(LockRole role)
        {
            return std::make_unique<EngineLock>(fs, kLockDir, role, clock, [this](const QString& r) { reports << r; });
        }
        void installLocks(const QByteArray& id = "0123456789abcdef0123456789abcdef")
        {
            if (!fs.node(kLockDir)) fs.mkdir(kLockDir, sd::lockDirectoryDescriptor());
            fs.plantFile(kEngine, LockIdentity::format(id), sd::lockFileDescriptor());
            fs.plantFile(kRecord, LockIdentity::format(id), sd::lockFileDescriptor());
        }
    };

private slots:
    // ---- INV-97: created with the protected admin-only DACL --------------------------------------
    void createReceivesProtectedSa()
    {
        World w;
        FileApplyLock lock = w.applyLock();
        std::unique_ptr<LockHold> hold = lock.acquire(std::chrono::seconds(120));
        QVERIFY2(hold, qPrintable(lock.lastError()));
        const QList<FakeNtSeams::Call> dirs = w.fs.callsOf(QStringLiteral("createDirectory"));
        QCOMPARE(dirs.size(), 1);
        QCOMPARE(dirs[0].sd, sd::lockDirectoryDescriptor());
        FakeNtSeams::Call open;
        for (const FakeNtSeams::Call& c : w.fs.callsOf(QStringLiteral("openRelative")))
            if (c.name == QLatin1String("apply.lock")) open = c;
        QCOMPARE(open.sd, sd::lockFileDescriptor());   // in the create call: never an inherited DACL
        QCOMPARE(open.share, 0u);
        QCOMPARE(open.disposition, nt::kFileOpenIf);
        QVERIFY(open.access & nt::kGenericRead);
        QVERIFY(open.access & nt::kGenericWrite);
        QVERIFY(open.flags & nt::kOpenReparsePoint);
        QCOMPARE(w.fs.node(kApply)->sd, sd::lockFileDescriptor());
        QCOMPARE(w.fs.callCount(QStringLiteral("setSecurity")), 0);
        QCOMPARE(w.fs.callCount(QStringLiteral("lockExclusive")), 1);
        QCOMPARE(w.reports, QStringList{QStringLiteral("created the lock directory ") + kLockDir});
    }

    // ---- INV-66: exclusive, released on release --------------------------------------------------
    void secondAcquireWaitsUntilReleased()
    {
        World w;
        FileApplyLock a = w.applyLock(), b = w.applyLock();
        std::unique_ptr<LockHold> first = a.acquire(std::chrono::seconds(120));
        QVERIFY(first);
        const Deadline start = w.clock.now();
        QVERIFY(!b.acquire(std::chrono::seconds(3)));   // polled every 100 ms on the fake clock, then gives up
        QVERIFY(w.clock.now() - start >= std::chrono::seconds(3));
        QVERIFY2(b.lastError().contains(QLatin1String("not free")), qPrintable(b.lastError()));
        int polls = 0;
        w.clock.onSleep = [&] {
            if (++polls == 5) first.reset();   // the holder finishes while the second one waits
        };
        std::unique_ptr<LockHold> second = b.acquire(std::chrono::seconds(120));
        QVERIFY(second);
        QCOMPARE(polls, 5);
    }

    void aSecondHandleIsRefusedWhileHeld()
    {
        World w;
        FileApplyLock a = w.applyLock();
        std::unique_ptr<LockHold> first = a.acquire(std::chrono::seconds(1));
        QVERIFY(first);
        Handle d = 0, h = 0;
        QVERIFY(w.fs.createFile(kLockDir, nt::kListDirectory, nt::kShareAll, nt::kOpenExisting, nt::kFlagBackupSemantics, {}, &d).ok());
        QCOMPARE(w.fs.openRelative(d, QStringLiteral("apply.lock"), nt::kGenericRead, nt::kShareAll, nt::kFileOpen, 0, {}, &h).kind,
                 Status::Kind::SharingViolation);
        w.fs.close(d);
    }

    // ---- an existing lock: validated by handle, repaired through a WRITE_DAC handle -------------
    void wideDaclResetThroughWriteDacHandle()
    {
        World w;
        w.fs.mkdir(kLockDir, sd::lockDirectoryDescriptor());
        w.fs.plantFile(kApply, {}, wide(kBa));
        const FileId id = w.fs.node(kApply)->id;
        FileApplyLock lock = w.applyLock();
        std::unique_ptr<LockHold> hold = lock.acquire(std::chrono::seconds(1));
        QVERIFY2(hold, qPrintable(lock.lastError()));
        QVERIFY(w.fs.node(kApply)->id == id);   // repaired in place, not recreated
        QCOMPARE(w.fs.node(kApply)->sd, sd::lockFileDescriptor());
        const QList<FakeNtSeams::Call> sets = w.fs.callsOf(QStringLiteral("setSecurity"));
        QCOMPARE(sets.size(), 1);
        QCOMPARE(sets[0].sd, sd::lockFileDescriptor());
        // ... through a separate handle opened for READ_CONTROL | WRITE_DAC | WRITE_OWNER, not the lock handle.
        FakeNtSeams::Call mutation;
        for (const FakeNtSeams::Call& c : w.fs.callsOf(QStringLiteral("openRelative")))
            if (c.name == QLatin1String("apply.lock") && (c.access & nt::kWriteDac)) mutation = c;
        QVERIFY(mutation.access & nt::kWriteOwner);
        QCOMPARE(mutation.access & (nt::kGenericRead | nt::kGenericWrite | nt::kReadData | nt::kWriteData), 0u);
        QCOMPARE(w.reports.size(), 1);   // and reported
        QVERIFY(w.reports[0].contains(QLatin1String("apply.lock")));
    }

    // A repair is reported the moment it is made. When the read-back that verifies
    // it then fails, the lock is refused, but the reset permissions stay on disk and are reported.
    void lockRepairReportedWhenTheReadBackFails_data()
    {
        QTest::addColumn<bool>("widenedAgain");   // false: the read-back cannot read the permissions
        QTest::newRow("read-back unreadable") << false;
        QTest::newRow("read-back finds it widened again") << true;
    }
    void lockRepairReportedWhenTheReadBackFails()
    {
        QFETCH(bool, widenedAgain);
        World w;
        w.fs.mkdir(kLockDir, sd::lockDirectoryDescriptor());
        w.fs.plantFile(kApply, {}, wide(kBa));
        bool set = false;
        w.fs.inject = [&](const FakeNtSeams::Call& c) -> std::optional<win::Status> {
            if (c.op == QLatin1String("setSecurity")) set = true;
            else if (set && !widenedAgain && c.op == QLatin1String("getSecurity") && c.path.endsWith(QLatin1String("\\apply.lock")))
                return win::Status::fail(win::Status::Kind::AccessDenied, 5, QStringLiteral("injected"));
            return std::nullopt;
        };
        w.fs.before = [&](const FakeNtSeams::Call& c) {
            if (set && widenedAgain && c.op == QLatin1String("getSecurity") && c.path.endsWith(QLatin1String("\\apply.lock"))) w.fs.setSd(kApply, wide(kBa));
        };
        FileApplyLock lock = w.applyLock();
        QVERIFY(!lock.acquire(std::chrono::seconds(1)));
        QVERIFY2(lock.lastError().contains(QLatin1String("still not secured")), qPrintable(lock.lastError()));
        QCOMPARE(w.fs.callCount(QStringLiteral("setSecurity")), 1);
        QCOMPARE(w.reports.size(), 1);   // the change that was made is reported, although the lock is refused
        QVERIFY(w.reports[0].startsWith(QLatin1String("reset the permissions of ") + kApply));
    }

    // A lock directory this call created is reported, also when it is then refused.
    void createdLockDirectoryReportedWhenItIsThenRefused_data()
    {
        QTest::addColumn<QString>("after");   // what goes wrong once it exists
        QTest::newRow("its reopen fails") << QStringLiteral("reopen");
        QTest::newRow("it is judged insecure") << QStringLiteral("judge");
    }
    void createdLockDirectoryReportedWhenItIsThenRefused()
    {
        QFETCH(QString, after);
        World w;
        bool created = false;
        w.fs.inject = [&](const FakeNtSeams::Call& c) -> std::optional<win::Status> {
            if (c.op == QLatin1String("createDirectory")) created = true;
            else if (created && after == QLatin1String("reopen") && c.op == QLatin1String("createFile") && c.path == kLockDir)
                return win::Status::fail(win::Status::Kind::AccessDenied, 5, QStringLiteral("injected"));
            return std::nullopt;
        };
        w.fs.before = [&](const FakeNtSeams::Call& c) {
            if (created && after == QLatin1String("judge") && c.op == QLatin1String("getSecurity") && c.path.endsWith(QLatin1String("\\AetherSerialBridge")))
                w.fs.setSd(kLockDir, descriptor(kUser, {allow(kBu, sd::kFullControl)}, true));
        };
        FileApplyLock lock = w.applyLock();
        QVERIFY(!lock.acquire(std::chrono::seconds(1)));
        QVERIFY(w.fs.node(kLockDir));   // it stays ...
        QCOMPARE(w.reports, QStringList{QStringLiteral("created the lock directory ") + kLockDir});   // ... and is reported
        QVERIFY(!w.fs.node(kApply));
    }

    // A directory someone else created in the meantime is not reported as ours.
    void lockDirectoryCreatedByAnotherIsNotReported()
    {
        World w;
        w.fs.before = [&](const FakeNtSeams::Call& c) {
            if (c.op == QLatin1String("createDirectory") && !w.fs.node(kLockDir)) w.fs.mkdir(kLockDir, sd::lockDirectoryDescriptor());
        };
        FileApplyLock lock = w.applyLock();
        QVERIFY2(lock.acquire(std::chrono::seconds(1)), qPrintable(lock.lastError()));
        QVERIFY(w.reports.isEmpty());
    }

    void repairRefusedWhenTheMutationHandleIsAnotherFile()
    {
        World w;
        w.fs.mkdir(kLockDir, sd::lockDirectoryDescriptor());
        w.fs.plantFile(kApply, {}, wide(kBa));
        w.fs.before = [&](const FakeNtSeams::Call& c) {
            if (c.op == QLatin1String("openRelative") && (c.access & nt::kWriteDac))
                w.fs.replace(kApply, w.fs.plantFile(kLockDir + QStringLiteral("\\other"), {}, wide(kBa)));
        };
        FileApplyLock lock = w.applyLock();
        QVERIFY(!lock.acquire(std::chrono::seconds(1)));
        QCOMPARE(w.fs.callCount(QStringLiteral("setSecurity")), 0);
    }

    void untrustedOwnerOrLinkRefused_data()
    {
        QTest::addColumn<QString>("what");
        QTest::newRow("lock owned by a user") << QStringLiteral("owner");
        QTest::newRow("lock is a symlink") << QStringLiteral("link");
        QTest::newRow("lock is a directory") << QStringLiteral("dir");
    }
    void untrustedOwnerOrLinkRefused()
    {
        QFETCH(QString, what);
        World w;
        w.fs.mkdir(kLockDir, sd::lockDirectoryDescriptor());
        if (what == QLatin1String("owner")) w.fs.plantFile(kApply, {}, sd::build(kUser, parsed(sd::lockFileDescriptor())));
        if (what == QLatin1String("link")) {
            w.fs.plantFile(QStringLiteral("C:\\ProgramData\\x"), "X");
            w.fs.plantLink(kApply, QStringLiteral("C:\\ProgramData\\x"), false);
        }
        if (what == QLatin1String("dir")) w.fs.mkdir(kApply);
        FileApplyLock lock = w.applyLock();
        QVERIFY(!lock.acquire(std::chrono::seconds(1)));
        QVERIFY(!lock.lastError().isEmpty());
        QCOMPARE(w.fs.callCount(QStringLiteral("setSecurity")), 0);
    }

    void nullDaclWithTrustedOwnerIsRepaired()
    {
        World w;
        w.fs.mkdir(kLockDir, sd::lockDirectoryDescriptor());
        QByteArray b = sd::build(kBa);
        b[2] = char(quint8(b[2]) | sd::kDaclPresent);   // present, no ACL: everyone may open it
        w.fs.plantFile(kApply, {}, b);
        QVERIFY(parsed(w.fs.node(kApply)->sd).daclNull);
        FileApplyLock lock = w.applyLock();
        QVERIFY2(lock.acquire(std::chrono::seconds(1)), qPrintable(lock.lastError()));
        QCOMPARE(w.fs.node(kApply)->sd, sd::lockFileDescriptor());
        QCOMPARE(w.reports.size(), 1);
    }

    // ---- the lock directory --------------------------------------------------------------------
    void lockDirectoryJudged_data()
    {
        QTest::addColumn<QByteArray>("dirSd");
        QTest::addColumn<bool>("accepted");
        const quint8 oici = sd::kObjectInherit | sd::kContainerInherit;
        QTest::newRow("ours") << sd::lockDirectoryDescriptor() << true;
        QTest::newRow("Program Files default inheritance (TrustedInstaller, CREATOR OWNER inherit-only)") << QByteArray() << true;
        QTest::newRow("planted by a standard user") << sd::build(kUser, parsed(sd::lockDirectoryDescriptor())) << false;
        QTest::newRow("Users may add files") << descriptor(kBa, {allow(kSy, sd::kFullControl, oici), allow(kBu, 0x2, oici)}, true) << false;
        QTest::newRow("CREATOR OWNER on the folder itself (not inherit-only)")
            << descriptor(kBa, {allow(kSy, sd::kFullControl, oici), allow(QString::fromLatin1(sd::kCreatorOwner), sd::kFullControl)}, true)
            << false;
        QTest::newRow("callback ACE") << withAceType(sd::lockDirectoryDescriptor(), 0, 9) << false;
    }
    void lockDirectoryJudged()
    {
        QFETCH(QByteArray, dirSd);
        QFETCH(bool, accepted);
        World w;
        w.fs.mkdir(kLockDir, dirSd);   // empty = inherited from C:\Program Files, as CreateDirectoryW(path, NULL)
        FileApplyLock lock = w.applyLock();
        std::unique_ptr<LockHold> hold = lock.acquire(std::chrono::seconds(1));
        QVERIFY2(bool(hold) == accepted, qPrintable(lock.lastError()));
        if (!accepted) QVERIFY(!w.fs.node(kApply));   // nothing created in a directory that is not ours
        // The Applier reports why (not "another operation is still running"): a refusal is not a timeout.
        QCOMPARE(lock.refusalReason().isEmpty(), accepted);
    }

    void aTimeoutIsNotARefusal()
    {
        World w;
        FileApplyLock first = w.applyLock();
        std::unique_ptr<LockHold> held = first.acquire(std::chrono::seconds(1));
        QVERIFY(held);
        FileApplyLock second = w.applyLock();
        QVERIFY(!second.acquire(std::chrono::seconds(1)));
        QVERIFY(second.lastError().contains(QLatin1String("not free")));
        QVERIFY(second.refusalReason().isEmpty());   // another holder: the Applier's "still running" message
    }

    void lockDirectoryLinkRefused()
    {
        World w;
        w.fs.mkdir(QStringLiteral("C:\\ProgramData\\planted"));
        w.fs.plantLink(kLockDir, QStringLiteral("C:\\ProgramData\\planted"), true);
        FileApplyLock lock = w.applyLock();
        QVERIFY(!lock.acquire(std::chrono::seconds(1)));
        QVERIFY(!w.fs.node(QStringLiteral("C:\\ProgramData\\planted\\apply.lock")));
    }

    void unsupportedVolumeRefused()
    {
        World w;
        w.fs.volumeInfo = {QStringLiteral("FAT32"), false};
        FileApplyLock lock = w.applyLock();
        QVERIFY(!lock.acquire(std::chrono::seconds(1)));
        w.installLocks();
        auto e = w.engineLock(LockRole::Applier);
        QString why;
        QVERIFY(!e->filesystemSupported(&why));
        QVERIFY2(why.contains(QLatin1String("FAT32")), qPrintable(why));
        LockResult r = LockResult::Acquired;
        QVERIFY(!e->tryAcquire(&r));
        QCOMPARE(r, LockResult::Unsupported);
    }

    // INV-116 fails closed: the lock directory is never created when the volume it would be
    // created on cannot be determined.
    void missingLockDirectoryOnAnUnknownVolumeIsNeverCreated_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("deny");   // the exact path whose open is refused ("" = none)
        QTest::newRow("parent open denied") << kLockDir << QStringLiteral("C:\\Program Files");
        QTest::newRow("path has no parent") << QStringLiteral("AetherSerialBridge") << QString();
    }
    void missingLockDirectoryOnAnUnknownVolumeIsNeverCreated()
    {
        QFETCH(QString, path);
        QFETCH(QString, deny);
        World w;
        w.fs.inject = [&](const FakeNtSeams::Call& c) -> std::optional<win::Status> {
            if (!deny.isEmpty() && c.op == QLatin1String("createFile") && c.path == deny)
                return win::Status::fail(win::Status::Kind::AccessDenied, 5, QStringLiteral("injected"));
            return std::nullopt;
        };
        LockDirectory dir(w.fs, path, LockRole::Applier, {});
        QString why;
        QCOMPARE(dir.open(/*create*/ true, &why), LockResult::Unsupported);
        QVERIFY2(why.contains(QLatin1String("cannot be determined")), qPrintable(why));
        FileApplyLock lock(w.fs, path, w.clock);
        QVERIFY(!lock.acquire(std::chrono::seconds(1)));
        QCOMPARE(w.fs.callCount(QStringLiteral("createDirectory")), 0);
        QVERIFY(!w.fs.node(kLockDir));
    }

    // ---- WS-5 round 2: InsecureFile (the file failed, the directory passed) vs Insecure (never replaced) ----
    void engineLockResultNamesWhatFailed_data()
    {
        QTest::addColumn<int>("role");      // LockRole
        QTest::addColumn<QString>("setup");
        QTest::addColumn<int>("result");    // LockResult
        using R = LockResult;
        const int app = int(LockRole::Applier), svc = int(LockRole::Service);
        QTest::newRow("service: widened DACL") << svc << QStringLiteral("wide") << int(R::InsecureFile);
        QTest::newRow("service: foreign owner") << svc << QStringLiteral("owner") << int(R::InsecureFile);
        QTest::newRow("applier: foreign owner") << app << QStringLiteral("owner") << int(R::InsecureFile);
        QTest::newRow("applier: repair handle cannot be opened") << app << QStringLiteral("repair-open") << int(R::InsecureFile);
        QTest::newRow("applier: repair fails") << app << QStringLiteral("repair-set") << int(R::InsecureFile);
        QTest::newRow("applier: repaired in place") << app << QStringLiteral("wide") << int(R::Acquired);
        QTest::newRow("applier: lock directory widened") << app << QStringLiteral("dir") << int(R::Insecure);
        QTest::newRow("service: lock directory widened") << svc << QStringLiteral("dir") << int(R::Insecure);
        QTest::newRow("applier: lock permissions unreadable") << app << QStringLiteral("unreadable") << int(R::Insecure);
        QTest::newRow("applier: engine.lock is a directory") << app << QStringLiteral("notfile") << int(R::Insecure);
    }
    void engineLockResultNamesWhatFailed()
    {
        QFETCH(int, role);
        QFETCH(QString, setup);
        QFETCH(int, result);
        World w;
        if (setup == QLatin1String("notfile")) {
            w.fs.mkdir(kLockDir, sd::lockDirectoryDescriptor());
            w.fs.mkdir(kEngine);
        } else {
            w.installLocks();
        }
        if (setup == QLatin1String("wide") || setup.startsWith(QLatin1String("repair"))) w.fs.setSd(kEngine, wide(kBa));
        if (setup == QLatin1String("owner")) w.fs.setSd(kEngine, sd::build(kUser, parsed(sd::lockFileDescriptor())));
        if (setup == QLatin1String("dir")) w.fs.setSd(kLockDir, descriptor(kBa, {allow(kSy, sd::kFullControl), allow(kBa, sd::kFullControl), allow(kBu, sd::kFullControl)}, true));
        bool locked = false;   // the lock handle is open: the next opens of engine.lock are the repair's
        w.fs.inject = [&](const FakeNtSeams::Call& c) -> std::optional<win::Status> {
            const bool engine = c.path.endsWith(QLatin1String("\\engine.lock"));
            const auto denied = win::Status::fail(win::Status::Kind::AccessDenied, 5, QStringLiteral("injected"));
            if (setup == QLatin1String("unreadable") && engine && c.op == QLatin1String("getSecurity")) return denied;
            if (setup == QLatin1String("repair-set") && c.op == QLatin1String("setSecurity")) return denied;
            if (setup == QLatin1String("repair-open") && engine && c.op == QLatin1String("openRelative") && locked) return denied;
            if (engine && c.op == QLatin1String("openRelative")) locked = true;
            return std::nullopt;
        };
        auto lock = w.engineLock(LockRole(role));
        LockResult r = LockResult::Busy;
        std::unique_ptr<LockHold> hold = lock->tryAcquire(&r);
        QCOMPARE(int(r), result);
        QCOMPARE(bool(hold), result == int(LockResult::Acquired));
    }

    // ---- engine.lock (INV-104) -------------------------------------------------------------------
    void serviceRefusesAWidenedEngineLock()
    {
        World w;
        w.installLocks();
        w.fs.setSd(kEngine, wide(kBa));
        auto service = w.engineLock(LockRole::Service);
        LockResult r = LockResult::Acquired;
        QVERIFY(!service->tryAcquire(&r));
        QCOMPARE(r, LockResult::InsecureFile);   // the runner exits 4; install/Repair may replace it
        QCOMPARE(w.fs.callCount(QStringLiteral("setSecurity")), 0);
        QCOMPARE(w.fs.node(kEngine)->sd, wide(kBa));
    }

    void applierRepairsAWidenedEngineLockAndReports()
    {
        World w;
        w.installLocks();
        w.fs.setSd(kEngine, wide(kBa));
        auto applier = w.engineLock(LockRole::Applier);
        LockResult r = LockResult::Busy;
        std::unique_ptr<LockHold> hold = applier->acquireUntil(w.clock.now() + std::chrono::seconds(30), &r);
        QVERIFY2(hold, qPrintable(applier->lastError()));
        QCOMPARE(w.fs.node(kEngine)->sd, sd::lockFileDescriptor());
        QCOMPARE(w.reports.size(), 1);
        hold.reset();
        auto service = w.engineLock(LockRole::Service);
        QVERIFY(service->tryAcquire(&r));
    }

    void engineLockShareZero()
    {
        World w;
        w.installLocks();
        auto applier = w.engineLock(LockRole::Applier);
        auto service = w.engineLock(LockRole::Service);
        LockResult r = LockResult::Acquired;
        std::unique_ptr<LockHold> engine = service->tryAcquire(&r);
        QVERIFY(engine);
        FakeNtSeams::Call open;
        for (const FakeNtSeams::Call& c : w.fs.callsOf(QStringLiteral("openRelative")))
            if (c.name == QLatin1String("engine.lock") && (c.access & nt::kGenericRead)) open = c;
        QCOMPARE(open.share, 0u);
        QCOMPARE(open.disposition, nt::kFileOpen);   // existing file only: the engine never creates it
        QVERIFY(open.flags & nt::kOpenReparsePoint);
        QVERIFY(!applier->tryAcquire(&r));
        QCOMPARE(r, LockResult::Busy);
        // B3: polled every 100 ms until the deadline.
        const Deadline start = w.clock.now();
        QVERIFY(!applier->acquireUntil(start + std::chrono::seconds(30), &r));
        QCOMPARE(r, LockResult::Busy);
        QVERIFY(w.clock.now() - start >= std::chrono::seconds(30));
        engine.reset();   // process exit
        QVERIFY(applier->acquireUntil(w.clock.now() + std::chrono::seconds(30), &r));
    }

    void missingLockOrDirectory()
    {
        World w;
        auto service = w.engineLock(LockRole::Service);
        LockResult r = LockResult::Acquired;
        QVERIFY(!service->tryAcquire(&r));
        QCOMPARE(r, LockResult::Missing);
        QCOMPARE(service->peekLock().status, LockFileRead::Status::Missing);
        QCOMPARE(service->readRecord().status, LockFileRead::Status::Missing);
        w.fs.mkdir(kLockDir, sd::lockDirectoryDescriptor());
        auto service2 = w.engineLock(LockRole::Service);
        QVERIFY(!service2->tryAcquire(&r));
        QCOMPARE(r, LockResult::Missing);
        QVERIFY(!w.fs.node(kEngine));   // never created by an open
    }

    void identityReadThroughTheHeldHandle()
    {
        World w;
        w.installLocks("00112233445566778899aabbccddeeff");
        auto applier = w.engineLock(LockRole::Applier);
        QCOMPARE(applier->readHeldLock().status, LockFileRead::Status::Failed);   // not held
        LockResult r = LockResult::Busy;
        std::unique_ptr<LockHold> hold = applier->tryAcquire(&r);
        QVERIFY(hold);
        const LockFileRead held = applier->readHeldLock();
        QCOMPARE(held.status, LockFileRead::Status::Ok);
        QCOMPARE(LockIdentity::parse(held.bytes).value_or(QByteArray()), QByteArray("00112233445566778899aabbccddeeff"));
        QCOMPARE(w.fs.callsOf(QStringLiteral("read")).constLast().handle, static_cast<WinLockHold*>(hold.get())->native());
        QCOMPARE(LockIdentity::classify(held, applier->readRecord()), LockIdentity::PairState::Matching);
        hold.reset();
        QCOMPARE(applier->readHeldLock().status, LockFileRead::Status::Failed);
    }

    void peekNeverOpensTheContent()
    {
        World w;
        w.installLocks();
        auto service = w.engineLock(LockRole::Service);
        auto applier = w.engineLock(LockRole::Applier);
        LockResult r = LockResult::Acquired;
        std::unique_ptr<LockHold> engine = service->tryAcquire(&r);
        QVERIFY(engine);
        w.fs.calls.clear();
        QCOMPARE(applier->peekLock().status, LockFileRead::Status::Unreadable);   // not "Busy", not an error
        for (const FakeNtSeams::Call& c : w.fs.callsOf(QStringLiteral("openRelative")))
            if (c.name == QLatin1String("engine.lock")) QCOMPARE(c.access, nt::kReadAttributes);
        QCOMPARE(LockIdentity::classify(applier->peekLock(), applier->readRecord()), LockIdentity::PairState::Unknown);
    }

    void recordValidated_data()
    {
        QTest::addColumn<QByteArray>("recordSd");
        QTest::addColumn<int>("status");
        QTest::newRow("secure") << sd::lockFileDescriptor() << int(LockFileRead::Status::Ok);
        QTest::newRow("Users may write") << wide(kBa) << int(LockFileRead::Status::Failed);
        QTest::newRow("owned by a user") << sd::build(kUser, parsed(sd::lockFileDescriptor())) << int(LockFileRead::Status::Failed);
        QTest::newRow("inherited (not protected)") << sd::ownerOnlyDescriptor(kBa) << int(LockFileRead::Status::Failed);
    }
    void recordValidated()
    {
        QFETCH(QByteArray, recordSd);
        QFETCH(int, status);
        World w;
        w.installLocks();
        w.fs.setSd(kRecord, recordSd);
        auto applier = w.engineLock(LockRole::Applier);
        QCOMPARE(int(applier->readRecord().status), status);
        // A record reader never blocks the record's replacement: it shares read, write and delete.
        for (const FakeNtSeams::Call& c : w.fs.callsOf(QStringLiteral("openRelative")))
            if (c.name == QLatin1String("engine.lock.id")) QCOMPARE(c.share, nt::kShareAll);
    }

    // ---- publication through io(): the lock files' security in the create call ------------------
    void publicationGetsTheLockSecurity()
    {
        World w;
        w.fs.mkdir(kLockDir, sd::lockDirectoryDescriptor());
        w.fs.defaultOwner = kUser;   // object-creator policy
        auto applier = w.engineLock(LockRole::Applier);
        const QByteArray id = LockIdentity::newId();
        for (const char* name : {LockIdentity::kLockName, LockIdentity::kRecordName}) {
            const WriteResult p = LockIdentity::publish(applier->io(), applier->dir(), QString::fromLatin1(name), id);
            QVERIFY2(p.replaced && p.durable, qPrintable(p.error));
        }
        QCOMPARE(w.fs.node(kEngine)->sd, sd::lockFileDescriptor());
        QCOMPARE(w.fs.node(kRecord)->sd, sd::lockFileDescriptor());
        auto service = w.engineLock(LockRole::Service);
        LockResult r = LockResult::Busy;
        std::unique_ptr<LockHold> engine = service->tryAcquire(&r);
        QVERIFY2(engine, qPrintable(service->lastError()));
        QCOMPARE(LockIdentity::classify(service->readHeldLock(), service->readRecord()), LockIdentity::PairState::Matching);
    }

    // The WS-5 ordering fold: a held engine.lock cannot be replaced; release, replace, re-acquire can.
    void replaceNeedsTheHoldReleased()
    {
        World w;
        w.installLocks("0123456789abcdef0123456789abcdef");
        auto applier = w.engineLock(LockRole::Applier);
        LockResult r = LockResult::Busy;
        std::unique_ptr<LockHold> hold = applier->tryAcquire(&r);
        QVERIFY(hold);
        const QByteArray fresh = "fedcba9876543210fedcba9876543210";
        WriteResult p = LockIdentity::publish(applier->io(), applier->dir(), QString::fromLatin1(LockIdentity::kLockName), fresh);
        QVERIFY(!p.replaced);   // share mode 0: sharing violation
        hold.reset();
        p = LockIdentity::publish(applier->io(), applier->dir(), QString::fromLatin1(LockIdentity::kLockName), fresh);
        QVERIFY2(p.replaced && p.durable, qPrintable(p.error));
        hold = applier->tryAcquire(&r);
        QVERIFY(hold);
        QCOMPARE(LockIdentity::parse(applier->readHeldLock().bytes).value_or(QByteArray()), fresh);
    }

    void childrenListTheLockDirectory()
    {
        World w;
        w.installLocks();
        w.fs.plantFile(kLockDir + QStringLiteral("\\engine.lock.0123456789abcdef0123456789abcdef.asbtmp"), "x", sd::lockFileDescriptor());
        auto applier = w.engineLock(LockRole::Applier);
        QStringList names = applier->children();
        names.sort();
        QCOMPARE(names, (QStringList{QStringLiteral("engine.lock"), QStringLiteral("engine.lock.0123456789abcdef0123456789abcdef.asbtmp"),
                                     QStringLiteral("engine.lock.id")}));
    }

    // ---- v1.1 INV-128 (WS-17): the lock directory's handle per acquisition ---------------------------------------
    // v1.0 opened and judged the directory once per EngineLock and kept the handle for the object's life, so the window
    // (whose Platform lives as long as it does) held a handle in %ProgramFiles%\AetherSerialBridge between operations.
    // Now: a hold keeps it and its release closes it; peek, record, children and the volume check close it after the
    // call; dir() keeps it until closeIdle(). Every acquisition opens and judges it again.
    void directoryHandlePerAcquisition()
    {
        World w;
        w.installLocks();
        std::unique_ptr<EngineLock> engine = w.engineLock(LockRole::Applier);
        QCOMPARE(w.fs.openHandles(), 0);
        const int dirOpensBefore = int(w.fs.callsOf(QStringLiteral("createFile")).size());
        LockResult why = LockResult::Busy;
        std::unique_ptr<LockHold> hold = engine->tryAcquire(&why);
        QVERIFY(hold);
        QCOMPARE(w.fs.openHandles(), 2);   // the directory and engine.lock, while held
        (void)engine->dir();                // the hold's directory, for publication (no new handle)
        QCOMPARE(w.fs.openHandles(), 2);
        hold.reset();
        QCOMPARE(w.fs.openHandles(), 0);   // the release closes both
        hold = engine->tryAcquire(&why);   // judged again
        QVERIFY(hold);
        QVERIFY(int(w.fs.callsOf(QStringLiteral("createFile")).size()) >= dirOpensBefore + 2);
        hold.reset();
        QCOMPARE(w.fs.openHandles(), 0);
        // Reads without a hold close after themselves.
        (void)engine->peekLock();
        QCOMPARE(w.fs.openHandles(), 0);
        QCOMPARE(int(engine->readRecord().status), int(LockFileRead::Status::Ok));
        QCOMPARE(w.fs.openHandles(), 0);
        QVERIFY(!engine->children().isEmpty());
        QCOMPARE(w.fs.openHandles(), 0);
        QString fsWhy;
        QVERIFY2(engine->filesystemSupported(&fsWhy), qPrintable(fsWhy));
        QCOMPARE(w.fs.openHandles(), 0);
        // dir() opens it for the caller; closeIdle() (after every operation) closes it.
        (void)engine->dir();
        QCOMPARE(w.fs.openHandles(), 1);
        engine->closeIdle();
        QCOMPARE(w.fs.openHandles(), 0);
        // The v1.0 control (tst_GuiNative's handle-probe control flag) keeps it.
        engine->setKeepDirectory(true);
        (void)engine->peekLock();
        QCOMPARE(w.fs.openHandles(), 1);
        engine->closeIdle();
        QCOMPARE(w.fs.openHandles(), 1);
    }

    // ---- WS-16 (D69): the held lock files marked for deletion on close -----------------------------------------
    // Both Applier opens ask for DELETE (the service's engine.lock open does not); marked with the POSIX disposition, the
    // file goes when the hold closes; cleared again, it stays.
    void heldLocksMarkedForDeletionOnClose()
    {
        World w;
        w.installLocks();
        FileApplyLock apply = w.applyLock();
        std::unique_ptr<EngineLock> engine = w.engineLock(LockRole::Applier);
        std::unique_ptr<LockHold> a = apply.acquire(std::chrono::seconds(120));
        LockResult why = LockResult::Busy;
        std::unique_ptr<LockHold> e = engine->tryAcquire(&why);
        QVERIFY(a && e);
        QString err;
        QVERIFY2(a->markForDeletion(&err), qPrintable(err));
        QVERIFY2(e->markForDeletion(&err), qPrintable(err));
        QCOMPARE(int(static_cast<WinLockHold&>(*a).marked()), int(WinLockHold::Marked::Posix));
        QVERIFY(w.fs.node(kApply));   // still there while held
        a.reset();
        e.reset();
        QVERIFY(!w.fs.node(kApply));
        QVERIFY(!w.fs.node(kEngine));
        QVERIFY(w.fs.node(kRecord));   // the record is the Applier's own step (LockRecordRemove)

        World k;   // marked, then cleared (the program was kept): both stay
        k.installLocks();
        FileApplyLock apply2 = k.applyLock();
        std::unique_ptr<LockHold> a2 = apply2.acquire(std::chrono::seconds(120));
        QVERIFY(a2->markForDeletion(&err));
        QVERIFY2(a2->clearDeletion(&err), qPrintable(err));
        a2.reset();
        QVERIFY(k.fs.node(kApply));

        World s;   // the service's hold has no DELETE: it can never be marked
        s.installLocks();
        std::unique_ptr<EngineLock> svc = s.engineLock(LockRole::Service);
        std::unique_ptr<LockHold> h = svc->tryAcquire(&why);
        QVERIFY(h);
        QVERIFY(!h->markForDeletion(&err));
        h.reset();
        QVERIFY(s.fs.node(kEngine));
    }

    // D69: where the POSIX disposition is not supported (ERROR_INVALID_PARAMETER, ERROR_NOT_SUPPORTED) the classic
    // FileDispositionInfo on the same handle; clearing undoes that form. Any other refusal is not retried, and when both
    // fail nothing is marked. (WS-16 doc: tst_Applier::uninstallClassicDispositionFallback; the decision is this
    // adapter's, so the row is here, over the recording NtSeams.)
    void uninstallClassicDispositionFallback()
    {
        for (const quint32 unsupported : {87u, 50u}) {
            World w;
            w.installLocks();
            w.fs.inject = [unsupported](const FakeNtSeams::Call& c) -> std::optional<Status> {
                if (c.op == QLatin1String("setDeleteOnClose") && c.name == QLatin1String("posix"))
                    return Status::fail(Status::Kind::Other, unsupported, QStringLiteral("not supported"));
                return std::nullopt;
            };
            FileApplyLock apply = w.applyLock();
            std::unique_ptr<LockHold> a = apply.acquire(std::chrono::seconds(120));
            QString err;
            QVERIFY2(a->markForDeletion(&err), qPrintable(err));
            QCOMPARE(int(static_cast<WinLockHold&>(*a).marked()), int(WinLockHold::Marked::Classic));
            QVERIFY(a->clearDeletion(&err));
            const QList<FakeNtSeams::Call> calls = w.fs.callsOf(QStringLiteral("setDeleteOnClose"));
            QCOMPARE(calls.size(), 3);
            QCOMPARE(calls[1].name, QStringLiteral("classic"));
            QCOMPARE(calls[2].name, QStringLiteral("classic"));   // the clear uses the form that was set
            QCOMPARE(calls[2].flags, 0u);
            a.reset();
            QVERIFY(w.fs.node(kApply));
        }
        {
            World w;   // access denied: not a "not supported", so no classic attempt
            w.installLocks();
            w.fs.inject = [](const FakeNtSeams::Call& c) -> std::optional<Status> {
                if (c.op == QLatin1String("setDeleteOnClose")) return Status::fail(Status::Kind::AccessDenied, 5, QStringLiteral("denied"));
                return std::nullopt;
            };
            FileApplyLock apply = w.applyLock();
            std::unique_ptr<LockHold> a = apply.acquire(std::chrono::seconds(120));
            QString err;
            QVERIFY(!a->markForDeletion(&err));
            QCOMPARE(w.fs.callCount(QStringLiteral("setDeleteOnClose")), 1);
            a.reset();
            QVERIFY(w.fs.node(kApply));
        }
        {
            World w;   // both forms fail: nothing marked, the file stays
            w.installLocks();
            w.fs.inject = [](const FakeNtSeams::Call& c) -> std::optional<Status> {
                if (c.op != QLatin1String("setDeleteOnClose")) return std::nullopt;
                return c.name == QLatin1String("posix") ? Status::fail(Status::Kind::Other, 50, QStringLiteral("not supported"))
                                                         : Status::fail(Status::Kind::AccessDenied, 5, QStringLiteral("denied"));
            };
            FileApplyLock apply = w.applyLock();
            std::unique_ptr<LockHold> a = apply.acquire(std::chrono::seconds(120));
            QString err;
            QVERIFY(!a->markForDeletion(&err));
            QVERIFY2(err.contains(QStringLiteral("classic")), qPrintable(err));
            a.reset();
            QVERIFY(w.fs.node(kApply));
        }
    }
};

QTEST_GUILESS_MAIN(tst_FileApplyLock)
#include "tst_FileApplyLock.moc"
