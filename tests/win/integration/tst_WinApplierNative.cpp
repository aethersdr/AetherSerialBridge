// WS-6 native rows on windows-latest, label win-integration: the WS-5 deferred INV-116 Windows lock-pair rows,
// the Applier (WS-5) running against the REAL data folder, apply.lock, engine.lock and FileIo (WS-6), with fakes
// only for SCM, the firewall and status (WS-7).
#include "WinNativeSupport.h"

#include "core/Applier.h"
#include "core/LockIdentity.h"
#include "core/StartToken.h"
#include "core/TrustedConfig.h"
#include "platform/win/EngineLock.h"
#include "platform/win/FileApplyLock.h"
#include "platform/win/NativeSeams.h"
#include "platform/win/WindowsDataFolder.h"
#include "support/Fakes.h"

#include <QProcess>
#include <QTest>

using namespace asb;
using namespace asb::win;
using namespace asb::test;
using namespace asb::test::native;
using LockIdentity::PairState;

namespace {
const QByteArray kA = "aaaaaaaaaaaaaaaa1111111111111111";
const QByteArray kB = "bbbbbbbbbbbbbbbb2222222222222222";

// The surviving engine of old_engine_on_unlinked_lock_blocks_repair: asb_win_lock_helper takes engine.lock as the
// service would (share mode 0) and keeps it until it is killed.
bool startEngineHolder(QProcess& p, const QString& lockDir)
{
    p.start(qEnvironmentVariable("ASB_WIN_LOCK_HELPER"), {QStringLiteral("hold-engine"), lockDir});
    if (!p.waitForStarted(10000)) return false;
    for (int i = 0; i < 300 && !p.canReadLine(); ++i) p.waitForReadyRead(100);
    return p.readLine().trimmed() == "held";
}

struct NativeRig {
    QString root = scratch(QStringLiteral("ap"));
    QString data = root + QStringLiteral("\\data");
    QString lockDir = root + QStringLiteral("\\AetherSerialBridge");
    FakeWorld world;
    FakeService service{world};
    FakeFirewall firewall{world};
    FakeStatus status{service};
    WindowsDataFolder folder{nativeSeams(), data};
    WinFileIo io{nativeSeams(), CreateSecurity::dataFolder()};
    FileApplyLock applyLock{nativeSeams(), lockDir, world.clock};
    EngineLock engineLock{nativeSeams(), lockDir, LockRole::Applier, world.clock};
    FileStartToken token{folder, io};
    Platform platform{folder, service, firewall, applyLock, engineLock, token, status, io};

    NativeRig()
    {
        world.checkLock = false;   // the apply lock here is the real one
        FileApplyLock first(nativeSeams(), lockDir);
        first.acquire(std::chrono::seconds(5));   // creates the lock directory as the Applier does
    }
    ~NativeRig() { removeTree(root); }

    void publish(const char* name, const QByteArray& id)
    {
        EngineLock e(nativeSeams(), lockDir, LockRole::Applier);
        const WriteResult w = LockIdentity::publish(e.io(), e.dir(), QString::fromLatin1(name), id);
        QVERIFY2(w.replaced && w.durable, qPrintable(w.error));
    }
    // What an interrupted publication leaves: a secured temp with an id, never renamed.
    void plantTemp(const char* base)
    {
        const QString name = QString::fromLatin1(base) + QStringLiteral(".0123456789abcdef0123456789abcdef.asbtmp");
        QVERIFY(writeFile(lockDir + QLatin1Char('\\') + name, LockIdentity::format(kB), QStringLiteral("O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)")));
    }
    PairState pairOnDisk()
    {
        EngineLock e(nativeSeams(), lockDir, LockRole::Applier);
        LockResult why = LockResult::Busy;
        std::unique_ptr<LockHold> hold = e.tryAcquire(&why);
        if (!hold) return why == LockResult::Missing ? LockIdentity::classify({LockFileRead::Status::Missing, {}, {}}, e.readRecord())
                                                     : PairState::Unknown;
        return LockIdentity::classify(e.readHeldLock(), e.readRecord());
    }
    QByteArray file(const char* name) { return readFile(lockDir + QLatin1Char('\\') + QLatin1String(name)).value_or("<missing>"); }
    int temps() { return int(QDir(lockDir).entryList({QStringLiteral("*.asbtmp")}, QDir::Files).size()); }
};
}  // namespace

class tst_WinApplierNative : public QObject {
    Q_OBJECT

private slots:
    void lock_pair_crash_between_steps_then_reinstall_windows_data()
    {
        QTest::addColumn<QString>("state");
        QTest::newRow("temp lock written") << QStringLiteral("temp-lock");
        QTest::newRow("lock renamed, no record") << QStringLiteral("lock");
        QTest::newRow("lock, temp record written") << QStringLiteral("lock+temp-record");
        QTest::newRow("record only") << QStringLiteral("record");
        QTest::newRow("both, different ids") << QStringLiteral("mismatched");
        QTest::newRow("matching (reinstall is a no-op)") << QStringLiteral("matching");
        // WS-5 round 2: a fresh install over a leftover corrupt engine.lock with no record.
        QTest::newRow("corrupt lock, no record (fresh install)") << QStringLiteral("corrupt");
    }
    void lock_pair_crash_between_steps_then_reinstall_windows()
    {
        QFETCH(QString, state);
        NativeRig r;
        if (state == QLatin1String("temp-lock")) r.plantTemp(LockIdentity::kLockName);
        if (state.startsWith(QLatin1String("lock"))) r.publish(LockIdentity::kLockName, kA);
        if (state == QLatin1String("lock+temp-record")) r.plantTemp(LockIdentity::kRecordName);
        if (state == QLatin1String("record")) r.publish(LockIdentity::kRecordName, kA);
        if (state == QLatin1String("mismatched")) {
            r.publish(LockIdentity::kLockName, kB);
            r.publish(LockIdentity::kRecordName, kA);
        }
        if (state == QLatin1String("matching")) {
            r.publish(LockIdentity::kLockName, kA);
            r.publish(LockIdentity::kRecordName, kA);
        }
        if (state == QLatin1String("corrupt"))
            QVERIFY(writeFile(r.lockDir + QStringLiteral("\\engine.lock"), "{\"lockId\":\"corrupt\"}",
                              QStringLiteral("O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)")));
        // An interrupted install: the service exists, starts are not yet proven blocked, no engine runs (the
        // corrupt row: no service at all).
        r.service.installation.state = state == QLatin1String("corrupt") ? InstallState::NotInstalled : InstallState::IncompleteInstall;
        const Result res = Applier(r.platform, r.world.clock).run(std::nullopt, true);
        QVERIFY2(res.persisted == Persisted::Committed, qPrintable(res.step + QStringLiteral(": ") + res.message));
        QCOMPARE(r.pairOnDisk(), PairState::Matching);
        QCOMPARE(r.temps(), 0);   // leftovers of the interrupted publication were removed under the proof
        if (state == QLatin1String("matching") || state == QLatin1String("lock") || state == QLatin1String("lock+temp-record"))
            QCOMPARE(LockIdentity::parse(r.file(LockIdentity::kRecordName)).value_or(QByteArray()), kA);   // the held lock's id
        if (state == QLatin1String("record"))
            QVERIFY(LockIdentity::parse(r.file(LockIdentity::kRecordName)).value_or(kA) != kA);   // never the recorded id
        if (state == QLatin1String("corrupt"))
            QVERIFY(LockIdentity::parse(r.file(LockIdentity::kLockName)).has_value());   // both files replaced
        QCOMPARE(sddlOf(r.lockDir + QStringLiteral("\\engine.lock")), QStringLiteral("O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)"));
        QCOMPARE(sddlOf(r.lockDir + QStringLiteral("\\engine.lock.id")), QStringLiteral("O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)"));
    }

    // WS-5 round-1 hand-off gate: Install/Repair over a mismatched pair on Windows. The core releases its own share-0
    // hold before replacing engine.lock, publishes the lock then the record, re-acquires by name and re-checks.
    void lock_pair_mismatch_recovered_windows_data()
    {
        QTest::addColumn<bool>("repair");
        QTest::newRow("install") << false;
        QTest::newRow("Repair") << true;
    }
    void lock_pair_mismatch_recovered_windows()
    {
        QFETCH(bool, repair);
        NativeRig r;
        r.publish(LockIdentity::kLockName, kB);
        r.publish(LockIdentity::kRecordName, kA);
        r.service.installation.state = InstallState::Current;   // installed: the Barrier holds engine.lock first
        QVERIFY(TrustedConfig::open(r.folder, TrustMode::Mutating).usable);
        const Result res = repair ? Applier(r.platform, r.world.clock).repair() : Applier(r.platform, r.world.clock).run(std::nullopt, true);
        QVERIFY2(res.step != QLatin1String("publish engine.lock") && res.step != QLatin1String("verify the engine lock"),
                 qPrintable(res.step + QStringLiteral(": ") + res.message));
        QCOMPARE(r.pairOnDisk(), PairState::Matching);
        const QByteArray id = LockIdentity::parse(r.file(LockIdentity::kLockName)).value_or(QByteArray());
        QVERIFY(id != kA && id != kB);   // a new id: neither the old lock's nor the recorded one
        QCOMPARE(r.temps(), 0);
        QCOMPARE(sddlOf(r.lockDir + QStringLiteral("\\engine.lock")), QStringLiteral("O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)"));
    }

    // WS-5 round-2 hand-off gates (INV-116). An engine.lock with a foreign owner, in a lock directory that passes
    // its checks, is InsecureFile: the service refuses it, and install/Repair replace it after the engine-free
    // proof, so the advised recovery is not a dead end on Windows.
    void insecure_lock_file_replaced_windows_data()
    {
        QTest::addColumn<bool>("repair");
        QTest::newRow("install") << false;
        QTest::newRow("Repair") << true;
    }
    void insecure_lock_file_replaced_windows()
    {
        QFETCH(bool, repair);
        NativeRig r;
        r.publish(LockIdentity::kLockName, kA);
        r.publish(LockIdentity::kRecordName, kA);
        const QString lock = r.lockDir + QStringLiteral("\\engine.lock");
        {
            Privilege restore(SE_RESTORE_NAME);   // to give the file an owner that is not the caller
            QVERIFY(restore.ok());
            QVERIFY(setSddl(lock, QStringLiteral("O:BU"), OWNER_SECURITY_INFORMATION));
        }
        QCOMPARE(sddlOf(lock), QStringLiteral("O:BUD:P(A;;FA;;;SY)(A;;FA;;;BA)"));
        {
            EngineLock service(nativeSeams(), r.lockDir, LockRole::Service);
            LockResult why = LockResult::Acquired;
            QVERIFY(!service.tryAcquire(&why));
            QCOMPARE(why, LockResult::InsecureFile);   // the directory is trusted; the file is what fails
        }
        r.service.installation.state = InstallState::Current;
        QVERIFY(TrustedConfig::open(r.folder, TrustMode::Mutating).usable);
        const Result res = repair ? Applier(r.platform, r.world.clock).repair() : Applier(r.platform, r.world.clock).run(std::nullopt, true);
        QVERIFY2(res.step != QLatin1String("publish engine.lock") && res.step != QLatin1String("verify the engine lock"),
                 qPrintable(res.step + QStringLiteral(": ") + res.message));
        QCOMPARE(r.pairOnDisk(), PairState::Matching);
        const QByteArray id = LockIdentity::parse(r.file(LockIdentity::kLockName)).value_or(QByteArray());
        QVERIFY(!id.isEmpty() && id != kA);   // replaced with a new id
        QCOMPARE(sddlOf(lock), QStringLiteral("O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)"));
        QCOMPARE(r.temps(), 0);
        EngineLock service(nativeSeams(), r.lockDir, LockRole::Service);
        LockResult why = LockResult::Busy;
        QVERIFY(service.tryAcquire(&why));   // and the service accepts the new one
    }

    // An insecure lock DIRECTORY is never replaced into. On Windows apply.lock lives in the same directory, so P1
    // already refuses it: nothing is written and the directory keeps the permissions it had.
    void insecure_lock_directory_refused_windows_data()
    {
        QTest::addColumn<bool>("repair");
        QTest::newRow("install") << false;
        QTest::newRow("Repair") << true;
    }
    void insecure_lock_directory_refused_windows()
    {
        QFETCH(bool, repair);
        NativeRig r;
        r.publish(LockIdentity::kLockName, kA);
        r.publish(LockIdentity::kRecordName, kA);
        const QString widened = QStringLiteral("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;BU)");
        QVERIFY(setSddl(r.lockDir, widened, DACL_SECURITY_INFORMATION));
        const QString before = sddlOf(r.lockDir);
        {
            EngineLock service(nativeSeams(), r.lockDir, LockRole::Service);
            LockResult why = LockResult::Acquired;
            QVERIFY(!service.tryAcquire(&why));
            QCOMPARE(why, LockResult::Insecure);   // the directory fails: never InsecureFile
        }
        r.service.installation.state = InstallState::Current;
        const QStringList namesBefore = QDir(r.lockDir).entryList(QDir::Files | QDir::Hidden | QDir::System);
        const Result res = repair ? Applier(r.platform, r.world.clock).repair() : Applier(r.platform, r.world.clock).run(std::nullopt, true);
        QCOMPARE(res.outcome, Outcome::NothingChanged);
        QVERIFY2(res.message.contains(QLatin1String("is not secured")), qPrintable(res.message));
        QCOMPARE(LockIdentity::parse(r.file(LockIdentity::kLockName)).value_or(QByteArray()), kA);
        QCOMPARE(LockIdentity::parse(r.file(LockIdentity::kRecordName)).value_or(QByteArray()), kA);
        QCOMPARE(QDir(r.lockDir).entryList(QDir::Files | QDir::Hidden | QDir::System), namesBefore);
        QCOMPARE(sddlOf(r.lockDir), before);
        QVERIFY(!r.service.startsBlocked);
    }

    // WS-5 deferred gate (INV-116, rounds 3 and 4), the Windows half of old_engine_on_unlinked_lock_blocks_repair:
    // an old engine process outlives its stop request and keeps engine.lock, a real share-0 hold in another
    // process. The SCM fake reports that process's pid (WS-7's adapter: STOP_PENDING with the pid). Repair,
    // --install --force-recreate and --install neither move the data folder nor run R3 while it lives, and no
    // result calls that stop verified; once it exits, the same operation repairs the folder and installs. On
    // Windows the old engine's engine.lock can be neither deleted nor replaced while it is held (share mode 0),
    // so the POSIX rows' "unlinked" and "replaced while the old inode is held" states cannot arise here: the row
    // checks that too, with the holder alive.
    void old_engine_on_unlinked_lock_blocks_repair_data()
    {
        QTest::addColumn<int>("op");   // 0 Repair, 1 --install --force-recreate, 2 --install
        QTest::newRow("Repair") << 0;
        QTest::newRow("--install --force-recreate") << 1;
        QTest::newRow("--install") << 2;
    }
    void old_engine_on_unlinked_lock_blocks_repair()
    {
        QFETCH(int, op);
        NativeRig r;
        r.publish(LockIdentity::kLockName, kA);
        r.publish(LockIdentity::kRecordName, kA);
        r.service.installation.state = InstallState::Current;
        const bool repairsFolder = op != 2;
        if (repairsFolder) {   // untrusted (Everyone: full control), so Repair and --force-recreate would move it aside
            QVERIFY(CreateDirectoryW(W(r.data), nullptr));
            QVERIFY(setSddl(r.data, QStringLiteral("D:PAI(A;OICI;FA;;;WD)"), DACL_SECURITY_INFORMATION));
            QVERIFY(writeFile(r.data + QStringLiteral("\\planted.txt"), "x"));
        } else {
            QVERIFY(TrustedConfig::open(r.folder, TrustMode::Mutating).usable);
        }
        QProcess engine;
        QVERIFY(startEngineHolder(engine, r.lockDir));
        r.service.engineRunning = true;
        r.service.pid = qint64(engine.processId());
        r.service.neverReleases = true;   // it outlives its stop request

        // The held lock cannot be unlinked or replaced (the states the POSIX rows plant).
        const QString lock = r.lockDir + QStringLiteral("\\engine.lock");
        QVERIFY(!DeleteFileW(W(lock)));
        QCOMPARE(GetLastError(), DWORD(ERROR_SHARING_VIOLATION));
        const QString other = r.lockDir + QStringLiteral("\\replacement.tmp");
        QVERIFY(writeFile(other, LockIdentity::format(kB), QStringLiteral("O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)")));
        QVERIFY(!MoveFileExW(W(other), W(lock), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
        const DWORD moveError = GetLastError();
        QVERIFY2(moveError == ERROR_SHARING_VIOLATION || moveError == ERROR_ACCESS_DENIED, qPrintable(QString::number(moveError)));
        QVERIFY(DeleteFileW(W(other)));

        auto runIt = [&] {
            Applier a(r.platform, r.world.clock);
            return op == 0 ? a.repair() : a.run(std::nullopt, true, op == 1);
        };
        auto asides = [&] {
            return QDir(r.root).entryList({QStringLiteral("*.untrusted-*")}, QDir::Dirs | QDir::Hidden | QDir::System);
        };
        const Result res = runIt();
        const QString said = res.step + QStringLiteral(": ") + res.message + QStringLiteral(" | ") + res.postcondition;
        QVERIFY2(res.outcome == Outcome::StateUnknown, qPrintable(said));
        QVERIFY2(!res.message.contains(QLatin1String("verified")) && !res.postcondition.contains(QLatin1String("verified")),
                 qPrintable(said));
        QVERIFY2(!claimsStop(res.message) && !claimsStop(res.postcondition), qPrintable(said));
        QVERIFY2(!r.world.log.contains(QStringLiteral("service:installOrConfigure")), qPrintable(r.world.log.all().join(QLatin1Char(' '))));
        QVERIFY(res.fileChanges.isEmpty());
        QVERIFY(r.service.startsBlocked);
        QVERIFY(asides().isEmpty());
        if (repairsFolder) QCOMPARE(readFile(r.data + QStringLiteral("\\planted.txt")).value_or("<missing>"), QByteArray("x"));
        QCOMPARE(LockIdentity::parse(r.file(LockIdentity::kRecordName)).value_or(QByteArray()), kA);
        QCOMPARE(r.temps(), 0);

        // The old process exits: the same operation now goes through.
        engine.kill();
        QVERIFY(engine.waitForFinished(10000));
        r.service.neverReleases = false;
        r.service.engineRunning = false;
        r.service.pid = 0;
        r.world.log.clear();
        const Result again = runIt();
        const QString saidAgain = again.step + QStringLiteral(": ") + again.message;
        if (op == 0) QVERIFY2(again.outcome == Outcome::Ok, qPrintable(saidAgain));
        else QVERIFY2(r.world.log.contains(QStringLiteral("service:installOrConfigure")), qPrintable(saidAgain));
        QCOMPARE(r.pairOnDisk(), PairState::Matching);
        if (repairsFolder) {
            const QStringList aside = asides();
            QCOMPARE(aside.size(), 1);
            QCOMPARE(readFile(r.root + QLatin1Char('\\') + aside[0] + QStringLiteral("\\planted.txt")).value_or("<missing>"),
                     QByteArray("x"));
        }
    }

    void lock_pair_mismatch_refused_windows()
    {
        NativeRig r;
        r.publish(LockIdentity::kLockName, kA);
        r.publish(LockIdentity::kRecordName, kA);
        r.service.installation.state = InstallState::Current;
        QVERIFY(TrustedConfig::open(r.folder, TrustMode::Mutating).usable);
        QVERIFY(r.token.writeFresh(FirewallGeneration{"0123456789abcdef"}, {}).replaced);
        r.publish(LockIdentity::kRecordName, kB);   // the record no longer names the lock
        const Result res = Applier(r.platform, r.world.clock).start();
        // Windows cannot read engine.lock before holding it: B3 decides, after B0-B2 already happened.
        QCOMPARE(res.outcome, Outcome::StateUnknown);
        QCOMPARE(res.persisted, Persisted::None);
        QVERIFY2(res.message.contains(QLatin1String("start token revoked")), qPrintable(res.message));
        QVERIFY(res.message.contains(QLatin1String("starts blocked")));
        QVERIFY(res.message.contains(QLatin1String("stop requested")));
        QCOMPARE(LockIdentity::parse(r.file(LockIdentity::kLockName)).value_or(QByteArray()), kA);   // nothing refreshed
        QCOMPARE(LockIdentity::parse(r.file(LockIdentity::kRecordName)).value_or(QByteArray()), kB);
        QCOMPARE(GetFileAttributesW(W(r.data + QStringLiteral("\\start.token"))), INVALID_FILE_ATTRIBUTES);
    }

    void partial_pair_refused_before_any_change()
    {
        NativeRig r;
        r.publish(LockIdentity::kLockName, kA);   // record missing
        r.service.installation.state = InstallState::Current;
        QVERIFY(TrustedConfig::open(r.folder, TrustMode::Mutating).usable);
        QVERIFY(r.token.writeFresh(FirewallGeneration{"0123456789abcdef"}, {}).replaced);
        const Result res = Applier(r.platform, r.world.clock).start();
        QCOMPARE(res.outcome, Outcome::NothingChanged);
        QVERIFY2(res.message.contains(QLatin1String("lock record mismatch")), qPrintable(res.message));
        QVERIFY(GetFileAttributesW(W(r.data + QStringLiteral("\\start.token"))) != INVALID_FILE_ATTRIBUTES);   // untouched
        QVERIFY(!r.service.startsBlocked);
    }
};

QTEST_GUILESS_MAIN(tst_WinApplierNative)
#include "tst_WinApplierNative.moc"
