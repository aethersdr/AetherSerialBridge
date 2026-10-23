// Engine-lock identity (D55, D58, INV-116): the format, the pair-state classification (master §4.6
// table plus the README v10 row "record present / lock missing"), the refusals for normal operations,
// the recovery that install and Repair perform only after the engine-free proof, and crash fixtures
// between every publication step followed by install / Repair.

#include "core/Applier.h"
#include "core/BridgeRunner.h"
#include "core/LockIdentity.h"
#include "support/Fakes.h"

#include <QTest>

using namespace asb;
using namespace asb::test;
using LockIdentity::PairState;

namespace {

const QByteArray kOld = QByteArrayLiteral("5555555555555555aaaaaaaaaaaaaaaa");   // installedRunning's id
const QByteArray kOther = QByteArrayLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

LockFileRead ok(const QByteArray& b)
{
    return LockFileRead{LockFileRead::Status::Ok, b, {}};
}
LockFileRead missing()
{
    return LockFileRead{LockFileRead::Status::Missing, {}, {}};
}

PairState pairOf(Rig& r)
{
    auto read = [&](const QString& n) { return r.world.lockFiles.exists(n) ? ok(r.world.lockFiles.contents(n)) : missing(); };
    return LockIdentity::classify(read(QStringLiteral("engine.lock")), read(QStringLiteral("engine.lock.id")));
}

// An installed station whose lock pair is in `state` (the engine cannot be running unless matching).
void stationWithPair(Rig& r, PairState state)
{
    r.installedRunning(configA());
    r.service.engineRunning = r.service.engineHoldsLock = false;
    r.service.pid = 0;
    r.world.lockFiles.eraseEntry(QStringLiteral("engine.lock"));
    r.world.lockFiles.eraseEntry(QStringLiteral("engine.lock.id"));
    r.world.lockFiles.syncDir(FakeDir(), nullptr);
    switch (state) {
    case PairState::Fresh:
        break;
    case PairState::Partial:
        r.engineLock.plantLock(LockIdentity::format(kOld));
        break;
    case PairState::RecordOnly:
        r.engineLock.plantRecord(LockIdentity::format(kOld));
        break;
    case PairState::Mismatched:
        r.engineLock.plantLock(LockIdentity::format(kOther));
        r.engineLock.plantRecord(LockIdentity::format(kOld));
        break;
    case PairState::Matching:
    case PairState::Unknown:
        r.engineLock.plantPair(kOld);
        r.service.launchEngine();
        break;
    }
    r.world.log.clear();
}

}  // namespace

class tst_LockIdentity : public QObject {
    Q_OBJECT
private slots:
    void formatAndParse_data();
    void formatAndParse();
    void classifyTable_data();
    void classifyTable();
    void recoveryTable();
    void missingRecordRefused();
    void corruptRecordRefused();
    void recreatedLockRefusedAgainstOldRecord();
    void b3MismatchNamesEffects();
    void reinstallIdempotent();
    void pairStates_data();
    void pairStates();
    void recordOnlyNeverRecreatesTheRecordedId();
    void crashBetweenPublicationStepsThenReinstall_data();
    void crashBetweenPublicationStepsThenReinstall();
    void crashAfterRenameBeforeDirectorySync_data();
    void crashAfterRenameBeforeDirectorySync();
    void noRecoveryWithoutEngineFreeProof();
    void engineRefusesAMismatchedPair();
    // WS-5 round 2
    void insecureLockFileReplacedByInstallAndRepair_data();
    void insecureLockFileReplacedByInstallAndRepair();
    void insecureLockDirectoryIsNeverReplaced_data();
    void insecureLockDirectoryIsNeverReplaced();
    void insecureLockFileNeedsTheEngineFreeProof();
    void partialDecidedByTheHeldRead();
};

void tst_LockIdentity::formatAndParse_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<bool>("ok");
    QTest::newRow("valid") << LockIdentity::format(kOld) << true;
    QTest::newRow("upper") << QByteArray(R"({"lockId":"5555555555555555AAAAAAAAAAAAAAAA"})") << false;
    QTest::newRow("short") << QByteArray(R"({"lockId":"5555"})") << false;
    QTest::newRow("spaces") << QByteArray(R"({ "lockId":"5555555555555555aaaaaaaaaaaaaaaa"})") << false;
    QTest::newRow("newline") << LockIdentity::format(kOld) + '\n' << false;
    QTest::newRow("oversized") << LockIdentity::format(kOld) + QByteArray(40, ' ') << false;
    QTest::newRow("empty") << QByteArray() << false;
}

void tst_LockIdentity::formatAndParse()
{
    QFETCH(QByteArray, bytes);
    QFETCH(bool, ok);
    const auto id = LockIdentity::parse(bytes);
    QCOMPARE(id.has_value(), ok);
    if (ok) QCOMPARE(*id, kOld);
    QVERIFY(LockIdentity::newId() != LockIdentity::newId());
    QVERIFY(LockIdentity::parse(LockIdentity::format(LockIdentity::newId())));
}

void tst_LockIdentity::classifyTable_data()
{
    QTest::addColumn<int>("lockStatus");
    QTest::addColumn<QByteArray>("lock");
    QTest::addColumn<int>("recordStatus");
    QTest::addColumn<QByteArray>("record");
    QTest::addColumn<int>("state");
    using S = LockFileRead::Status;
    const QByteArray a = LockIdentity::format(kOld), b = LockIdentity::format(kOther), junk("junk");
    QTest::newRow("fresh") << int(S::Missing) << QByteArray() << int(S::Missing) << QByteArray() << int(PairState::Fresh);
    QTest::newRow("partial") << int(S::Ok) << a << int(S::Missing) << QByteArray() << int(PairState::Partial);
    QTest::newRow("record only") << int(S::Missing) << QByteArray() << int(S::Ok) << a << int(PairState::RecordOnly);
    QTest::newRow("corrupt record only") << int(S::Missing) << QByteArray() << int(S::Ok) << junk << int(PairState::RecordOnly);
    QTest::newRow("ids differ") << int(S::Ok) << a << int(S::Ok) << b << int(PairState::Mismatched);
    QTest::newRow("corrupt record") << int(S::Ok) << a << int(S::Ok) << junk << int(PairState::Mismatched);
    QTest::newRow("corrupt lock") << int(S::Ok) << junk << int(S::Ok) << a << int(PairState::Mismatched);
    QTest::newRow("corrupt lock, no record") << int(S::Ok) << junk << int(S::Missing) << QByteArray() << int(PairState::Mismatched);
    QTest::newRow("record wrong owner") << int(S::Ok) << a << int(S::Failed) << QByteArray() << int(PairState::Mismatched);
    QTest::newRow("lock wrong owner") << int(S::Failed) << QByteArray() << int(S::Ok) << a << int(PairState::Mismatched);
    QTest::newRow("matching") << int(S::Ok) << a << int(S::Ok) << a << int(PairState::Matching);
    QTest::newRow("unreadable lock, valid record") << int(S::Unreadable) << QByteArray() << int(S::Ok) << a << int(PairState::Unknown);
    QTest::newRow("unreadable lock, no record") << int(S::Unreadable) << QByteArray() << int(S::Missing) << QByteArray()
                                                << int(PairState::Partial);
}

void tst_LockIdentity::classifyTable()
{
    QFETCH(int, lockStatus);
    QFETCH(QByteArray, lock);
    QFETCH(int, recordStatus);
    QFETCH(QByteArray, record);
    QFETCH(int, state);
    QCOMPARE(int(LockIdentity::classify(LockFileRead{LockFileRead::Status(lockStatus), lock, {}},
                                        LockFileRead{LockFileRead::Status(recordStatus), record, {}})),
             state);
}

void tst_LockIdentity::recoveryTable()
{
    using R = LockIdentity::Recovery;
    QCOMPARE(LockIdentity::recoveryFor(PairState::Fresh), R::PublishLockThenRecord);
    QCOMPARE(LockIdentity::recoveryFor(PairState::Partial), R::PublishRecordFromHeldLock);
    QCOMPARE(LockIdentity::recoveryFor(PairState::RecordOnly), R::ReplaceBoth);
    QCOMPARE(LockIdentity::recoveryFor(PairState::Mismatched), R::ReplaceBoth);
    QCOMPARE(LockIdentity::recoveryFor(PairState::Matching), R::None);
}

// INV-116. Normal operations refuse a missing record (Linux prologue: NothingChanged, before any change).
void tst_LockIdentity::missingRecordRefused()
{
    Rig r;
    stationWithPair(r, PairState::Partial);
    const Result res = Applier(*r.platform, r.world.clock).start();
    QCOMPARE(res.outcome, Outcome::NothingChanged);
    QVERIFY(res.message.contains(QStringLiteral("lock record mismatch")));
    QVERIFY(!r.world.log.contains(QStringLiteral("io:")));
    QVERIFY(!r.world.lockFiles.exists(QStringLiteral("engine.lock.id")));
}

void tst_LockIdentity::corruptRecordRefused()
{
    Rig r;
    r.installedRunning(configA());
    r.engineLock.plantRecord("{\"lockId\":\"zz\"}");
    const Result res = Applier(*r.platform, r.world.clock).run(std::nullopt, false);
    QCOMPARE(res.outcome, Outcome::NothingChanged);
    QCOMPARE(r.engineLock.recordContent(), QByteArray("{\"lockId\":\"zz\"}"));
    QVERIFY(r.service.engineRunning);
}

// INV-116. Kills: refreshing engine.lock.id whenever the lock is acquired (a recreated lock would then
// be adopted while an old engine may still hold the unlinked inode).
void tst_LockIdentity::recreatedLockRefusedAgainstOldRecord()
{
    for (bool unreadable : {false, true}) {
        Rig r;
        r.installedRunning(configA());
        r.engineLock.peekUnreadable = unreadable;   // Linux prologue / Windows B3
        r.engineLock.plantLock(LockIdentity::format(kOther));
        const Result res = Applier(*r.platform, r.world.clock).run(std::nullopt, false);
        QVERIFY(res.outcome == Outcome::NothingChanged || res.outcome == Outcome::StateUnknown);
        QCOMPARE(r.engineLock.recordContent(), LockIdentity::format(kOld));
        QCOMPARE(r.engineLock.lockContent(), LockIdentity::format(kOther));
        QVERIFY(!r.world.log.contains(QStringLiteral("lockio:")));
        QVERIFY(!r.world.log.contains(QStringLiteral("firewall:")));
    }
}

// INV-116. Kills: reporting a B3 mismatch as NothingChanged.
void tst_LockIdentity::b3MismatchNamesEffects()
{
    Rig r;
    r.installedRunning(configA());
    r.engineLock.peekUnreadable = true;
    r.engineLock.plantRecord(LockIdentity::format(kOther));
    const Result res = Applier(*r.platform, r.world.clock).start();
    QCOMPARE(res.outcome, Outcome::StateUnknown);
    QVERIFY(res.message.contains(QStringLiteral("start token revoked")));
    QVERIFY(res.message.contains(QStringLiteral("starts blocked")));
    QVERIFY(res.message.contains(QStringLiteral("stop requested")));
    QVERIFY(!res.message.contains(QStringLiteral("not changed")));
    QCOMPARE(res.persisted, Persisted::None);
}

void tst_LockIdentity::reinstallIdempotent()
{
    Rig r;
    r.installedRunning(configA());
    const QByteArray lock = r.engineLock.lockContent(), record = r.engineLock.recordContent();
    const Result res = Applier(*r.platform, r.world.clock).run(std::nullopt, true);
    QVERIFY2(res.outcome == Outcome::Ok, qPrintable(res.message));
    QCOMPARE(r.engineLock.lockContent(), lock);
    QCOMPARE(r.engineLock.recordContent(), record);
    QVERIFY(!r.world.log.contains(QStringLiteral("lockio:")));
}

// Master §4.6 pair-state table: every state x {normal, install, Repair} x {Linux, Windows}. The Windows
// model: engine.lock cannot be read unheld (the prologue sees "unknown" where it exists), and a held
// engine.lock cannot be replaced (share mode 0, the Applier's own hold included).
void tst_LockIdentity::pairStates_data()
{
    QTest::addColumn<int>("state");
    QTest::addColumn<int>("op");   // 0 normal (Start), 1 install, 2 Repair
    QTest::addColumn<bool>("windows");
    const char* states[] = {"fresh", "partial", "record only", "mismatched", "matching"};
    const PairState values[] = {PairState::Fresh, PairState::Partial, PairState::RecordOnly, PairState::Mismatched,
                                PairState::Matching};
    const char* ops[] = {"normal", "install", "Repair"};
    for (int w = 0; w < 2; ++w)
        for (int s = 0; s < 5; ++s)
            for (int o = 0; o < 3; ++o)
                QTest::addRow("%s x %s%s", states[s], ops[o], w ? " (Windows)" : "") << int(values[s]) << o << bool(w);
}

void tst_LockIdentity::pairStates()
{
    QFETCH(int, state);
    QFETCH(int, op);
    QFETCH(bool, windows);
    Rig r;
    stationWithPair(r, PairState(state));
    if (windows) r.engineLock.windows();
    Applier a(*r.platform, r.world.clock);
    const Result res = op == 0 ? a.start() : op == 1 ? a.run(std::nullopt, true) : a.repair();
    if (op == 0) {
        if (PairState(state) == PairState::Matching) {
            QCOMPARE(res.outcome, Outcome::Ok);
        } else if (windows && PairState(state) == PairState::Mismatched) {
            QCOMPARE(res.outcome, Outcome::StateUnknown);     // refused at B3, naming the B0-B2 effects
            QVERIFY(!r.world.log.contains(QStringLiteral("lockio:")));
        } else {
            QCOMPARE(res.outcome, Outcome::NothingChanged);   // refused (read-only prologue)
            QVERIFY(!r.world.log.contains(QStringLiteral("lockio:")));
        }
        return;
    }
    QVERIFY2(res.outcome == Outcome::Ok, qPrintable(res.message + QLatin1Char('\n') + r.world.log.all().join('\n')));
    QCOMPARE(pairOf(r), PairState::Matching);
    // Recovery published only after the engine-free proof: starts blocked and the service queried.
    if (PairState(state) != PairState::Matching) {
        const int published = r.world.log.indexOf(QStringLiteral("lockio:commitAs"));
        QVERIFY(published > r.world.log.indexOf(QStringLiteral("service:inhibitStarts")));
        QVERIFY(published > r.world.log.indexOf(QStringLiteral("service:query")));
        QVERIFY(r.world.lockFiles.tempNames().isEmpty());
    } else {
        QVERIFY(!r.world.log.contains(QStringLiteral("lockio:")));
    }
    if (op == 2) QVERIFY(r.service.startsBlocked);   // Repair leaves the service disabled
}

// README v10 nit: record present / lock missing. Kills: re-creating the lock with the recorded id.
void tst_LockIdentity::recordOnlyNeverRecreatesTheRecordedId()
{
    Rig r;
    stationWithPair(r, PairState::RecordOnly);
    QCOMPARE(Applier(*r.platform, r.world.clock).run(std::nullopt, true).outcome, Outcome::Ok);
    const auto id = LockIdentity::parse(r.engineLock.lockContent());
    QVERIFY(id);
    QVERIFY(*id != kOld);
    QCOMPARE(LockIdentity::parse(r.engineLock.recordContent()), id);
}

// INV-116 (N9-3). A crash after each publication step — temp lock written, lock renamed, directory
// synced, temp record written, record renamed — then install, and separately Repair, converge to a
// matching pair.
void tst_LockIdentity::crashBetweenPublicationStepsThenReinstall_data()
{
    QTest::addColumn<int>("crashAt");   // the n-th lock-directory operation of a fresh publication
    QTest::addColumn<bool>("repair");
    // A fresh publication through the lock directory's FileIo: createTemp, write, flush, commitAs
    // (rename + directory sync), then the same for the record.
    for (int n = 1; n <= 8; ++n) {
        QTest::addRow("crash before op %d, install", n) << n << false;
        QTest::addRow("crash before op %d, Repair", n) << n << true;
    }
}

void tst_LockIdentity::crashBetweenPublicationStepsThenReinstall()
{
    QFETCH(int, crashAt);
    QFETCH(bool, repair);
    Rig r;
    stationWithPair(r, PairState::Fresh);
    static_cast<SpyFileIo&>(r.engineLock.io()).crashAtOperation(crashAt);
    bool crashed = false;
    try {
        Applier(*r.platform, r.world.clock).run(std::nullopt, true);
    } catch (const SimulatedCrash&) {
        crashed = true;
    }
    QVERIFY(crashed);
    r.world.files.crash();
    r.world.lockFiles.crash();
    Applier a(*r.platform, r.world.clock);
    const Result res = repair ? a.repair() : a.run(std::nullopt, true);
    QVERIFY2(res.outcome == Outcome::Ok, qPrintable(res.message));
    QCOMPARE(pairOf(r), PairState::Matching);
    QVERIFY(r.world.lockFiles.tempNames().isEmpty());
}

// INV-116 (N9-3): the states between a publication's rename and its directory sync — "lock renamed" and
// "record renamed", not yet durable — then a power loss, then install / Repair converge to matching.
void tst_LockIdentity::crashAfterRenameBeforeDirectorySync_data()
{
    QTest::addColumn<QString>("file");
    QTest::addColumn<bool>("repair");
    for (const char* f : {"engine.lock", "engine.lock.id"}) {
        QTest::addRow("%s renamed, install", f) << QString::fromLatin1(f) << false;
        QTest::addRow("%s renamed, Repair", f) << QString::fromLatin1(f) << true;
    }
}

void tst_LockIdentity::crashAfterRenameBeforeDirectorySync()
{
    QFETCH(QString, file);
    QFETCH(bool, repair);
    Rig r;
    stationWithPair(r, PairState::Fresh);
    bool armed = false;
    static_cast<SpyFileIo&>(r.engineLock.io()).beforeHook = [&](const QString& op, const QString& name) {
        if (op == QLatin1String("commitAs") && name == file && !armed) {
            armed = true;
            r.world.lockFiles.failNext(FakeFileIo::Op::SyncDir);   // renamed, but the rename is not durable
        }
    };
    const Result first = Applier(*r.platform, r.world.clock).run(std::nullopt, true);
    QVERIFY(armed);
    QCOMPARE(first.outcome, Outcome::Uncommitted);   // the publication was not durable: never reported done
    r.world.files.crash();
    r.world.lockFiles.crash();
    r.service.engineRunning = r.service.engineHoldsLock = false;
    static_cast<SpyFileIo&>(r.engineLock.io()).beforeHook = {};
    Applier a(*r.platform, r.world.clock);
    const Result res = repair ? a.repair() : a.run(std::nullopt, true);
    QVERIFY2(res.outcome == Outcome::Ok, qPrintable(res.message));
    QCOMPARE(pairOf(r), PairState::Matching);
}

// INV-116. Kills: publishing the lock record without the engine-free proof.
void tst_LockIdentity::noRecoveryWithoutEngineFreeProof()
{
    Rig r;
    stationWithPair(r, PairState::RecordOnly);
    // An old engine still runs on the previous (unlinked) lock inode: the service has a process and
    // ignores the stop.
    r.service.engineRunning = true;
    r.service.pid = 4242;
    r.service.neverReleases = true;
    const QByteArray record = r.engineLock.recordContent();
    const Result res = Applier(*r.platform, r.world.clock).run(std::nullopt, true);
    QCOMPARE(res.outcome, Outcome::StateUnknown);
    QVERIFY(!r.world.lockFiles.exists(QStringLiteral("engine.lock")));
    QCOMPARE(r.engineLock.recordContent(), record);
    QVERIFY(!r.world.log.contains(QStringLiteral("lockio:commitAs")));
}

// The engine side of D58: after acquiring the lock and before the token, the ids are compared.
void tst_LockIdentity::engineRefusesAMismatchedPair()
{
    Rig r;
    r.installedRunning(configA());
    r.service.engineRunning = r.service.engineHoldsLock = false;
    r.engineLock.plantRecord(LockIdentity::format(kOther));
    r.world.checkLock = false;
    FakeEngine engine(r.world.log);
    FakeSideChannel side(r.world.log);
    NoFirewallMonitor monitor;
    FakeWatchdog watchdog(r.world.log, r.world.clock);
    BridgeRunner runner(r.engineLock, *r.token, r.folder, engine, side, monitor, watchdog, "id", r.world.clock);
    QCOMPARE(runner.start(), RunnerStart::EngineLockBusy);
    QCOMPARE(exitCode(RunnerStart::EngineLockBusy), 4);
    QVERIFY(r.folder.reads.isEmpty());
    QVERIFY(!engine.started);
}

// An engine.lock with the wrong owner or permissions inside a trusted lock directory: normal
// operations refuse it and advise --install or Repair, and both replace it after the engine-free proof
// (release, publish, re-acquire, re-check), on both OS models. Kills: LockFileInsecure refused like an
// insecure directory (the advised recovery would stop at the same refusal).
void tst_LockIdentity::insecureLockFileReplacedByInstallAndRepair_data()
{
    QTest::addColumn<bool>("repair");
    QTest::addColumn<bool>("windows");
    for (bool w : {false, true})
        for (bool rep : {false, true}) QTest::addRow("%s%s", rep ? "Repair" : "install", w ? " (Windows)" : "") << rep << w;
}

void tst_LockIdentity::insecureLockFileReplacedByInstallAndRepair()
{
    QFETCH(bool, repair);
    QFETCH(bool, windows);
    Rig r;
    r.installedRunning(configA());
    r.service.engineRunning = r.service.engineHoldsLock = false;
    r.service.pid = 0;
    if (windows) r.engineLock.windows();
    r.engineLock.plantInsecureLock(LockIdentity::format(kOld));
    r.world.log.clear();
    // The service refuses it (exit 4), and so does a normal operation, advising the recovery.
    r.service.launchEngine();
    QVERIFY(r.world.log.contains(QStringLiteral("engine:exit 4")));
    const Result refused = Applier(*r.platform, r.world.clock).start();
    QVERIFY(refused.outcome == Outcome::NothingChanged || refused.outcome == Outcome::StateUnknown);
    QVERIFY2(refused.message.contains(QStringLiteral("--install or Repair")), qPrintable(refused.message));
    QVERIFY(!r.world.log.contains(QStringLiteral("lockio:")));
    // The advised recovery succeeds from exactly that state.
    r.world.log.clear();
    Applier a(*r.platform, r.world.clock);
    const Result res = repair ? a.repair() : a.run(std::nullopt, true);
    QVERIFY2(res.outcome == Outcome::Ok, qPrintable(res.message + QLatin1Char('\n') + r.world.log.all().join('\n')));
    QVERIFY(!r.engineLock.lockFileInsecure());
    QCOMPARE(pairOf(r), PairState::Matching);
    QVERIFY(*LockIdentity::parse(r.engineLock.lockContent()) != kOld);   // a new id, never the old one
    const int published = r.world.log.indexOf(QStringLiteral("lockio:commitAs engine.lock"));
    QVERIFY(published > r.world.log.indexOf(QStringLiteral("service:inhibitStarts")));
    QVERIFY(published > r.world.log.indexOf(QStringLiteral("service:query")));
    // ... and the original intent then works.
    QCOMPARE(Applier(*r.platform, r.world.clock).start().outcome, Outcome::Ok);
    QVERIFY(r.service.engineRunning);
}

// The boundary of that fix: an insecure lock DIRECTORY is not something a replacement of engine.lock
// fixes, so install and Repair refuse it too, write nothing in it, and the advice is the manual step.
void tst_LockIdentity::insecureLockDirectoryIsNeverReplaced_data()
{
    QTest::addColumn<int>("op");   // 0 Start, 1 install, 2 Repair
    QTest::addColumn<bool>("windows");
    const char* ops[] = {"Start", "install", "Repair"};
    for (bool w : {false, true})
        for (int o = 0; o < 3; ++o) QTest::addRow("%s%s", ops[o], w ? " (Windows)" : "") << o << w;
}

void tst_LockIdentity::insecureLockDirectoryIsNeverReplaced()
{
    QFETCH(int, op);
    QFETCH(bool, windows);
    Rig r;
    r.installedRunning(configA());
    if (windows) r.engineLock.windows();
    r.engineLock.insecure = true;
    r.world.log.clear();
    Applier a(*r.platform, r.world.clock);
    const Result res = op == 0 ? a.start() : op == 1 ? a.run(std::nullopt, true) : a.repair();
    QVERIFY2(res.outcome != Outcome::Ok, qPrintable(res.message));
    QVERIFY(!r.world.log.contains(QStringLiteral("lockio:")));
    QVERIFY2(res.message.contains(QStringLiteral("An administrator must restore the engine lock directory")),
             qPrintable(res.message));
    QVERIFY(!res.message.contains(QStringLiteral("--install or Repair")));
    QCOMPARE(r.engineLock.lockContent(), LockIdentity::format(kOld));
}

// The review's precondition: an insecure engine.lock is replaced only after the engine-free proof. Here a
// process outlives the stop request, so nothing is published.
void tst_LockIdentity::insecureLockFileNeedsTheEngineFreeProof()
{
    for (bool repair : {false, true}) {
        Rig r;
        r.installedRunning(configA());
        r.engineLock.plantInsecureLock(LockIdentity::format(kOld));
        r.service.engineRunning = true;
        r.service.engineHoldsLock = false;
        r.service.pid = 4242;
        r.service.neverReleases = true;
        Applier a(*r.platform, r.world.clock);
        const Result res = repair ? a.repair() : a.run(std::nullopt, true);
        QVERIFY2(res.outcome == Outcome::StateUnknown, qPrintable(res.message));
        QVERIFY(r.engineLock.lockFileInsecure());
        QVERIFY(!r.world.log.contains(QStringLiteral("lockio:commitAs")));
    }
}

// The second guard: a "partial" pair is recovered from the lock as read THROUGH the hold. When
// that read is not a valid lock (here it changed after the prologue's unheld read), the pair is mismatched
// and both files are replaced: never a "corrupt" refusal that the same recovery repeats.
void tst_LockIdentity::partialDecidedByTheHeldRead()
{
    Rig r;
    r.installedRunning(configA());
    r.service.installation.state = InstallState::NotInstalled;
    r.service.engineRunning = r.service.engineHoldsLock = false;
    r.service.pid = 0;
    r.world.lockFiles.eraseEntry(QStringLiteral("engine.lock.id"));
    r.world.lockFiles.syncDir(FakeDir(), nullptr);
    r.service.onCall = [&r](const QString& m) {
        if (m == QLatin1String("installOrConfigure")) r.engineLock.plantLock(QByteArrayLiteral("{\"lockId\":\"x\"}"));
    };
    const Result res = Applier(*r.platform, r.world.clock).run(std::nullopt, true);
    QVERIFY2(res.outcome == Outcome::Ok, qPrintable(res.message + QLatin1Char('\n') + r.world.log.all().join('\n')));
    QCOMPARE(pairOf(r), PairState::Matching);
    QVERIFY(*LockIdentity::parse(r.engineLock.lockContent()) != kOld);
}

QTEST_GUILESS_MAIN(tst_LockIdentity)
#include "tst_LockIdentity.moc"
