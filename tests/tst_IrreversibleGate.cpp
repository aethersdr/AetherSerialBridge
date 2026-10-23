// The one gate for every irreversible step (WS-5 round 3). Audit (b) as tests, generated from the gate's
// own step table (core/IrreversibleGate.h), so a step added to the table without its rows fails here:
//  - judgeRefusesEachMissingPrecondition: the pure check. For every step: the control (every precondition
//    holds: admitted), each declared proof dropped, the deadline expired, an earlier phase's overrun, a phase
//    the step is not declared in, and failClosed's cleanup phase.
//  - stepPreconditionMatrix: the fault-matrix rows, through the real Applier and the fakes. For every step x
//    every precondition (its proofs and its deadline), a world in which that precondition cannot be proven
//    (or in which the call just before the step returns late), then: the step is never performed, and the
//    result is true (also: no stop claimed while an engine process survives, WS-5 round 6). Each row first
//    runs its scenario without the fault and checks that the step does run there, so no row passes vacuously.
//  - noDirectIrreversibleCalls: Applier.cpp and Barrier.cpp reach the platform only through GatedSeams, and
//    every gated GatedSeams method spends the token of its own step first.
//  - stopClaimsOnlyFromTheProof: no string in the files that word results, messages and interims claims a stop;
//    every such claim is engineClaim()'s, made from the operation's evidence.

#include "core/Applier.h"
#include "core/ConfigFiles.h"
#include "core/IrreversibleGate.h"
#include "core/LockIdentity.h"
#include "core/PhaseClock.h"
#include "support/Fakes.h"

#include <QFile>
#include <QRegularExpression>
#include <QTest>

#include <iterator>
#include <memory>

using namespace asb;
using namespace asb::test;

namespace {

const QString kConfig = QString::fromLatin1(ConfigFiles::kConfig);
const QString kPending = QString::fromLatin1(ConfigFiles::kPending);
const QString kToken = QString::fromLatin1(ConfigFiles::kStartToken);
const QByteArray kOtherId = QByteArrayLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
const QByteArray kThirdId = QByteArrayLiteral("cccccccccccccccccccccccccccccccc");
const QString kVerified = QStringLiteral("verified: engine lock acquired");

// The first call of `event` (a log entry prefix) returns `by` after it began.
void lateOnce(Rig& r, const QString& event, std::chrono::seconds by)
{
    auto done = std::make_shared<bool>(false);
    r.world.log.onAdd = [&r, event, by, done](const QString& e) {
        if (*done || !e.startsWith(event)) return;
        *done = true;
        r.world.clock.advance(by);
    };
}

// ---------------------------------------------------------------------------------------------------
// The pure check
// ---------------------------------------------------------------------------------------------------
Proofs everything(Phase phase, Deadline deadline)
{
    Proofs p;
    p.phase = phase;
    p.phaseDeadline = deadline;
    p.trustedFolder = p.engineLockHeld = p.engineFree = p.notInstalled = p.lockMatching = p.committed = true;
    p.firewallConnected = true;
    p.programGone = true;
    return p;
}

void drop(Proofs& p, Need n)
{
    switch (n) {
    case NeedTrustedFolder: p.trustedFolder = false; break;
    case NeedEngineStopped: p.engineLockHeld = p.engineFree = p.notInstalled = false; break;
    case NeedEngineFree: p.engineFree = false; break;
    case NeedLockMatching: p.lockMatching = false; break;
    case NeedCommitted: p.committed = false; break;
    case NeedFirewallConnected: p.firewallConnected = false; break;
    case NeedProgramGone: p.programGone = false; break;
    }
}

QString needId(Need n)
{
    switch (n) {
    case NeedTrustedFolder: return QStringLiteral("TrustedFolder");
    case NeedEngineStopped: return QStringLiteral("EngineStopped");
    case NeedEngineFree: return QStringLiteral("EngineFree");
    case NeedLockMatching: return QStringLiteral("LockMatching");
    case NeedCommitted: return QStringLiteral("Committed");
    case NeedFirewallConnected: return QStringLiteral("FirewallConnected");
    case NeedProgramGone: return QStringLiteral("ProgramGone");
    }
    return {};
}

const char* kindName(GateRefusal::Kind k)
{
    switch (k) {
    case GateRefusal::Kind::Late: return "Late";
    case GateRefusal::Kind::Missing: return "Missing";
    case GateRefusal::Kind::WrongPhase: return "WrongPhase";
    case GateRefusal::Kind::WrongToken: return "WrongToken";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------------
// The behavioural rows
// ---------------------------------------------------------------------------------------------------
enum class Op { RunN, InstallN, Start, Repair, ForceRecreateInstall, Edit, Discard, SaveAndRemoveN, Uninstall, UninstallSettings };

Result runOp(Rig& r, PhaseClock& pc, Op op)
{
    SaveRequest req;
    req.config = configB();
    req.source.kind = Source::Committed;
    req.source.sha256 = digest(configA());
    Applier a(*r.platform, r.world.clock, &pc);
    switch (op) {
    case Op::RunN: return a.run(req, false);
    case Op::InstallN: return a.run(req, true);
    case Op::Start: return a.start();
    case Op::Repair: return a.repair();
    case Op::ForceRecreateInstall: return a.run(req, true, true);
    case Op::Edit: return a.edit([](BridgeConfig& c) { c.ports[0].tcpPort = 4005; });
    case Op::Discard: return a.discardPending();
    case Op::SaveAndRemoveN: return a.saveAndRemove(req);
    case Op::Uninstall: return a.uninstall({});
    case Op::UninstallSettings: return a.uninstall({/*removeSettings=*/true});
    }
    return {};
}

void untrustedFolder(Rig& r)
{
    r.world.files.plantFile(QStringLiteral("planted.txt"), "x");   // an untrusted child: Recreate
}

// An operation that performs the step on its normal path. `detect` is the event that shows the step was
// made; `start` (optional) is the step's first event, found backwards from `detect` (a pending write begins
// with its temp).
struct Scenario {
    const char* name;
    Op op;
    std::function<void(Rig&)> setup;
    QString detect;
    QString start = {};
};

QList<Scenario> scenariosFor(Step s)
{
    const auto none = [](Rig&) {};
    const QString commitPending = QStringLiteral("io:commitAs config.pending.json");
    const QString temp = QStringLiteral("io:createTemp");
    switch (s) {
    case Step::FolderSecure: return {{"Run", Op::RunN, none, QStringLiteral("folder:normalise")}};
    case Step::FolderRepair:
        return {{"Repair, untrusted folder", Op::Repair, untrustedFolder, QStringLiteral("folder:renameAside")},
                {"--install --force-recreate, untrusted folder", Op::ForceRecreateInstall, untrustedFolder,
                 QStringLiteral("folder:renameAside")}};
    case Step::TempSweep:
        return {{"Run, a stale temp of ours", Op::RunN,
                 [](Rig& r) {
                     r.world.files.plantFile(QStringLiteral("config.json.0123456789abcdef0123456789abcdef.asbtmp"), "{",
                                             QStringLiteral("root"));
                 },
                 QStringLiteral("io:remove config.json.")}};
    case Step::TokenRevoke: return {{"Run", Op::RunN, none, QStringLiteral("io:openExisting start.token")}};
    case Step::BlockStarts: return {{"Run", Op::RunN, none, QStringLiteral("service:inhibitStarts")}};
    case Step::StopRequest: return {{"Run", Op::RunN, none, QStringLiteral("service:requestStop")}};
    case Step::InstallService: return {{"--install", Op::InstallN, none, QStringLiteral("service:installOrConfigure")}};
    case Step::LockPublish:
        return {{"--install, mismatched pair", Op::InstallN,
                 [](Rig& r) { r.engineLock.plantRecord(LockIdentity::format(kOtherId)); },
                 QStringLiteral("lockio:commitAs engine.lock"), QStringLiteral("lockio:createTemp")}};
    case Step::StagePending:
        return {{"Run", Op::RunN, none, commitPending, temp},
                // the pair is recovered first, so R4 follows the re-acquired lock's identity check
                {"--install, mismatched pair", Op::InstallN,
                 [](Rig& r) { r.engineLock.plantRecord(LockIdentity::format(kOtherId)); }, commitPending, temp}};
    case Step::SavePending: return {{"Edit", Op::Edit, none, commitPending, temp}};
    case Step::DiscardPending:
        return {{"DiscardPending", Op::Discard,
                 [](Rig& r) { r.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configB(), QStringLiteral("root")); },
                 QStringLiteral("io:remove config.pending.json")}};
    case Step::FirewallSync: return {{"Run", Op::RunN, none, QStringLiteral("firewall:sync")}};
    case Step::ConfigCommit: return {{"Run", Op::RunN, none, QStringLiteral("io:commitAs config.json")}};
    case Step::DropPending: return {{"Run", Op::RunN, none, QStringLiteral("io:remove config.pending.json")}};
    case Step::SettleJobs: return {{"Run", Op::RunN, none, QStringLiteral("service:reconcileOutstandingJobs")}};
    case Step::FirewallSweep: return {{"Run", Op::RunN, none, QStringLiteral("firewall:sweep")}};
    case Step::TokenPublish: return {{"Run", Op::RunN, none, QStringLiteral("io:commitAs start.token"), temp}};
    case Step::AllowStarts: return {{"Run", Op::RunN, none, QStringLiteral("service:allowStarts")}};
    case Step::ServiceStart: return {{"Run", Op::RunN, none, QStringLiteral("service:start")}};
    case Step::FirewallRemove: return {{"SaveAndRemove", Op::SaveAndRemoveN, none, QStringLiteral("firewall:removeAll")}};
    case Step::Uninstall:
        return {{"SaveAndRemove", Op::SaveAndRemoveN, none, QStringLiteral("service:uninstall")},
                {"Uninstall", Op::Uninstall, none, QStringLiteral("service:uninstall")}};
    // WS-16
    case Step::EntryWrite: return {{"--install", Op::InstallN, none, QStringLiteral("service:writeAppsEntry")}};
    case Step::LockDisposition: return {{"Uninstall", Op::Uninstall, none, QStringLiteral("applyLock:markForDeletion")}};
    case Step::ProgramRemove: return {{"Uninstall", Op::Uninstall, none, QStringLiteral("service:removeProgram")}};
    case Step::LockRecordRemove: return {{"Uninstall", Op::Uninstall, none, QStringLiteral("lockio:remove engine.lock.id")}};
    case Step::SettingsRemove:
        return {{"Uninstall --remove-settings", Op::UninstallSettings, none, QStringLiteral("io:remove config.json")}};
    case Step::EntryRemove: return {{"Uninstall", Op::Uninstall, none, QStringLiteral("service:removeAppsEntry")}};
    }
    return {};
}

// A world in which one proof cannot be established: the precondition is dropped where it would be proven.
struct Breaker {
    const char* name;
    std::function<void(Rig&)> apply;
    std::function<void(Rig&, const Result&)> verify = {};   // what else the row must find (QVERIFY inside)
    // The operation succeeds without the step by design: uninstall keeps the settings of an untrusted folder and says so
    // (INV-125: kept settings are not a failure). `verify` then checks what the result says.
    bool okWithoutStep = false;
};

// WS-5 round 6: an early barrier call (B1, blocking starts) returns after the barrier's deadline,
// before the stop was requested. The barrier ends at B4, and the cleanup must still request the stop and
// report only what it proves; here its stop succeeds.
Breaker earlyBarrierCallLate()
{
    return {"an early barrier call returns late",
            [](Rig& r) { lateOnce(r, QStringLiteral("service:inhibitStarts"), std::chrono::seconds(31)); },
            [](Rig& r, const Result& res) {
                const QString context = res.message + QStringLiteral("\npostcondition: ") + res.postcondition;
                QVERIFY2(r.service.stopRequested, qPrintable(QStringLiteral("the stop was never requested: ") + context));
                QVERIFY2(!r.service.engineRunning && !r.service.engineHoldsLock, qPrintable(context));
                QVERIFY2(res.postcondition.contains(kVerified) ||
                             res.postcondition.contains(QStringLiteral("could not prove the bridge stopped")),
                         qPrintable(context));
            }};
}

// WS-5 round 6: engine.lock is replaced between the prologue (which found the pair matching) and
// B3, while the old engine survives its stop request on the unlinked inode it holds. B3 acquires the new
// file and rejects its identity: nothing proves the stop, so nothing may claim it.
Breaker lockReplacedAfterThePrologue()
{
    return {"engine.lock is replaced after the prologue, an old engine still runs", [](Rig& r) {
                auto done = std::make_shared<bool>(false);
                r.service.onCall = [&r, done](const QString& m) {
                    if (*done || m != QLatin1String("requestStop")) return;
                    *done = true;
                    r.engineLock.plantLock(LockIdentity::format(kThirdId));
                    r.service.engineHoldsLock = false;   // it holds the unlinked inode, not this file
                    r.service.neverReleases = true;
                };
            }};
}

// `op`: install and Repair recover a mismatched pair themselves, so there the identity proof is dropped by
// making the recovery's publication fail instead.
QList<Breaker> breakersFor(Need n, Op op)
{
    switch (n) {
    case NeedTrustedFolder:
        if (op == Op::UninstallSettings)
            return {{"the data folder is not trusted", untrustedFolder,
                     [](Rig& r, const Result& res) {
                         QVERIFY2(res.message.contains(QStringLiteral("nothing in it was touched")), qPrintable(res.message));
                         QVERIFY(r.has(QStringLiteral("planted.txt")));
                         QVERIFY(r.has(kConfig));
                     },
                     /*okWithoutStep=*/true}};
        return {{"the data folder is not trusted", untrustedFolder}};
    case NeedProgramGone:
        return {{"the program is kept (in use by another process)",
                 [](Rig& r) { r.service.fail[QStringLiteral("removeProgram!")] = QStringLiteral("in use"); },
                 [](Rig& r, const Result& res) {
                     QVERIFY(r.service.programPresent);
                     QVERIFY(r.service.entryNamesExistingProgram());
                     QVERIFY2(res.message.contains(QStringLiteral("was kept")), qPrintable(res.message));
                 }}};
    case NeedEngineStopped:
    case NeedEngineFree:
        // An old engine outlives its stop request on a lock no hold can prove anything about.
        return {{"engine.lock missing, an old engine still runs",
                 [](Rig& r) {
                     r.world.lockFiles.eraseEntry(QStringLiteral("engine.lock"));   // it holds the unlinked inode
                     r.service.neverReleases = true;
                 }},
                {"engine.lock an insecure file, an old engine still runs",
                 [](Rig& r) {
                     r.engineLock.plantInsecureLock(r.engineLock.lockContent());
                     r.service.neverReleases = true;
                 }},
                // The barrier ACQUIRES engine.lock, but it is a replacement whose id is not the recorded one,
                // while the old engine survives on the unlinked inode it holds. The id is a third
                // one, so it matches neither the installed record nor a scenario's planted kOtherId.
                {"an acquired engine.lock that is not the recorded one, an old engine still runs",
                 [](Rig& r) {
                     r.engineLock.plantLock(LockIdentity::format(kThirdId));
                     r.service.engineHoldsLock = false;   // it holds the unlinked inode, not this file
                     r.service.neverReleases = true;
                 }},
                lockReplacedAfterThePrologue(), earlyBarrierCallLate()};
    case NeedLockMatching:
        return {{"the lock identity is read after the barrier's deadline",
                 [](Rig& r) {   // B3 acquires in time; the read through the hold returns late
                     auto done = std::make_shared<bool>(false);
                     r.engineLock.onRead = [&r, done](const QString& which) {
                         if (*done || which != QLatin1String("held")) return;
                         *done = true;
                         r.world.clock.advance(std::chrono::seconds(31));
                     };
                 }},
                op == Op::InstallN
                    ? Breaker{"engine.lock.id cannot be replaced by the recovery",
                              [](Rig& r) {
                                  static_cast<SpyFileIo&>(r.engineLock.io()).refuse = [](const QString& o, const QString& name) {
                                      return o == QLatin1String("commitAs") && name == QLatin1String("engine.lock.id")
                                                 ? QStringLiteral("access denied")
                                                 : QString();
                                  };
                              }}
                    : Breaker{"engine.lock.id does not match",
                              [](Rig& r) { r.engineLock.plantRecord(LockIdentity::format(kOtherId)); }},
                lockReplacedAfterThePrologue(), earlyBarrierCallLate()};
    case NeedCommitted:
        return {{"the commit's directory sync fails", [](Rig& r) {
                     r.firewall.onCall = [&r](const QString& m) {
                         if (m == QLatin1String("sync")) r.world.files.failNext(FakeFileIo::Op::SyncDir);
                     };
                 }}};
    case NeedFirewallConnected:
        // Windows: COM setup or ConnectServer fails. FakeFirewall fails every other call without a connection,
        // after logging it, so an operation that skips the connect performs the step here.
        return {{"the firewall cannot be connected",
                 [](Rig& r) { r.firewall.fail[QStringLiteral("connect")] = QStringLiteral("RPC_E_DISCONNECTED"); }}};
    }
    return {};
}

// One generated row: the step, the precondition (a Need bit, or 0 for its deadline), the scenario and
// breaker indexes.
struct GenRow {
    Step step;
    unsigned need;
    int scenario;
    int breaker;
};

QList<GenRow> generatedRows(QStringList* names)
{
    QList<GenRow> rows;
    for (const StepRule& rule : stepTable()) {
        const QList<Scenario> scenarios = scenariosFor(rule.step);
        QList<unsigned> preconditions{0u};   // 0 = the deadline (OnTime), required of every step
        for (Need n : kNeeds)
            if (rule.needs & n) preconditions << unsigned(n);
        for (int si = 0; si < qMax(1, int(scenarios.size())); ++si) {
            for (unsigned need : preconditions) {
                const Op op = scenarios.isEmpty() ? Op::RunN : scenarios[si].op;
                const int breakers = need == 0 ? 1 : int(breakersFor(Need(need), op).size());
                for (int bi = 0; bi < qMax(1, breakers); ++bi) {
                    rows << GenRow{rule.step, need, si, bi};
                    const QString sc = scenarios.isEmpty() ? QStringLiteral("NO SCENARIO")
                                                           : QString::fromLatin1(scenarios[si].name);
                    QString pre;
                    if (need == 0) pre = QStringLiteral("OnTime: the call before it returns late");
                    else if (breakers == 0) pre = QStringLiteral("%1: NO BREAKER").arg(needId(Need(need)));
                    else pre = QStringLiteral("%1: %2").arg(needId(Need(need)), QString::fromLatin1(breakersFor(Need(need), op)[bi].name));
                    *names << QStringLiteral("%1 | %2 | %3").arg(stepId(rule.step), sc, pre);
                }
            }
        }
    }
    return rows;
}

// The first event of the step (not failClosed's cleanup repeat of it), or -1.
int stepIndex(const EventLog& log, const Scenario& sc)
{
    const QStringList all = log.all();
    for (int i = 0; i < all.size(); ++i) {
        if (!all[i].startsWith(sc.detect) || log.phaseAt(i) == int(Phase::Cleanup)) continue;
        if (sc.start.isEmpty()) return i;
        for (int j = i; j >= 0; --j)
            if (all[j].startsWith(sc.start)) return j;
        return i;
    }
    return -1;
}

bool performed(const EventLog& log, const QString& detect)
{
    const QStringList all = log.all();
    for (int i = 0; i < all.size(); ++i)
        if (all[i].startsWith(detect) && log.phaseAt(i) != int(Phase::Cleanup)) return true;
    return false;
}

struct Snapshot {
    QByteArray config, token, lock, record;
    std::optional<QByteArray> pending;
    bool startsBlocked = false, engineRunning = false;
    QStringList rules;
};

Snapshot snapshot(Rig& r)
{
    Snapshot s;
    s.config = r.file(kConfig);
    s.token = r.file(kToken);
    if (r.has(kPending)) s.pending = r.file(kPending);
    s.lock = r.engineLock.lockContent();
    s.record = r.engineLock.recordContent();
    s.startsBlocked = r.service.startsBlocked;
    s.engineRunning = r.service.engineRunning;
    s.rules = r.firewall.rules;
    return s;
}

}  // namespace

class tst_IrreversibleGate : public QObject {
    Q_OBJECT
private slots:
    void tableIsComplete();
    void stepTableIsTheReviewedOne();
    void judgeRefusesEachMissingPrecondition_data();
    void judgeRefusesEachMissingPrecondition();
    void tokenIsSpentOnceForItsOwnStep();
    void stepPreconditionMatrix_data();
    void stepPreconditionMatrix();
    void noDirectIrreversibleCalls();
    void stopClaimsOnlyFromTheProof();
};

// Every step has one row, a name, a phase; the barrier steps (and only they) run again in failClosed.
void tst_IrreversibleGate::tableIsComplete()
{
    QCOMPARE(int(stepTable().size()), kStepCount);
    for (int i = 0; i < kStepCount; ++i) {
        const StepRule& r = stepTable()[size_t(i)];
        QCOMPARE(int(r.step), i);
        QVERIFY(!stepName(r.step).isEmpty());
        QVERIFY(r.phase != Phase::None && r.phase != Phase::Cleanup);
        const bool barrier = r.step == Step::TokenRevoke || r.step == Step::BlockStarts || r.step == Step::StopRequest;
        // EntryRemove: a stopped uninstall removes the entry of a program that is gone (WS-16, INV-126).
        QCOMPARE(r.cleanup, barrier || r.step == Step::EntryRemove);
    }
}

// The table as reviewed in WS-5 round 3, with FirewallConnected added in round 4 and the WS-16 uninstall steps and ProgramGone (WS-5 doc, "One gate for every irreversible step"). The rows below
// are generated from the table, so a need dropped from it would drop its rows with it: this pins the
// declaration itself.
void tst_IrreversibleGate::stepTableIsTheReviewedOne()
{
    struct Expected {
        Step step;
        unsigned needs;
        Phase phase, otherPhase;
    };
    const unsigned TF = NeedTrustedFolder, ES = NeedEngineStopped, EF = NeedEngineFree, LM = NeedLockMatching,
                   CO = NeedCommitted, FC = NeedFirewallConnected, PG = NeedProgramGone;
    const Expected expected[] = {
        {Step::FolderSecure, 0, Phase::Prologue, Phase::None},
        {Step::FolderRepair, ES, Phase::Folder, Phase::None},
        {Step::TempSweep, TF, Phase::Prologue, Phase::Folder},
        {Step::TokenRevoke, TF, Phase::Barrier, Phase::None},
        {Step::BlockStarts, 0, Phase::Barrier, Phase::None},
        {Step::StopRequest, 0, Phase::Barrier, Phase::None},
        {Step::InstallService, ES, Phase::Install, Phase::None},
        {Step::LockPublish, EF, Phase::Install, Phase::None},
        {Step::StagePending, TF | LM, Phase::Stage, Phase::None},
        {Step::SavePending, TF, Phase::Stage, Phase::None},
        {Step::DiscardPending, TF, Phase::Stage, Phase::None},
        {Step::FirewallSync, LM | FC, Phase::Firewall, Phase::None},
        {Step::ConfigCommit, TF | LM, Phase::Commit, Phase::None},
        {Step::DropPending, TF | LM | CO, Phase::Commit, Phase::None},
        {Step::SettleJobs, ES, Phase::Firewall, Phase::Remove},
        {Step::FirewallSweep, LM | CO | FC, Phase::Firewall, Phase::None},
        {Step::TokenPublish, TF | LM | CO, Phase::Activate, Phase::None},
        {Step::AllowStarts, CO, Phase::Activate, Phase::None},
        {Step::ServiceStart, CO, Phase::Activate, Phase::None},
        {Step::FirewallRemove, ES | FC, Phase::Remove, Phase::None},
        {Step::Uninstall, ES, Phase::Remove, Phase::None},
        // WS-16 (D63, D69; INV-125, INV-126)
        {Step::EntryWrite, ES, Phase::Install, Phase::None},
        {Step::LockDisposition, ES, Phase::Remove, Phase::None},
        {Step::ProgramRemove, ES, Phase::Remove, Phase::None},
        {Step::LockRecordRemove, ES | PG, Phase::Remove, Phase::None},
        {Step::SettingsRemove, TF | ES | PG, Phase::Remove, Phase::None},
        {Step::EntryRemove, ES | PG, Phase::Remove, Phase::None},
    };
    QCOMPARE(int(std::size(expected)), kStepCount);
    for (const Expected& e : expected) {
        const StepRule& r = ruleFor(e.step);
        const QString what = stepId(e.step);
        QVERIFY2(r.needs == e.needs, qPrintable(what + QStringLiteral(": needs ") + needNames(r.needs)));
        QVERIFY2(r.phase == e.phase, qPrintable(what + QStringLiteral(": phase ") + phaseName(r.phase)));
        QVERIFY2(r.otherPhase == e.otherPhase, qPrintable(what + QStringLiteral(": other phase ") + phaseName(r.otherPhase)));
    }
}

// The pure check, generated from the table.
void tst_IrreversibleGate::judgeRefusesEachMissingPrecondition_data()
{
    QTest::addColumn<int>("step");
    QTest::addColumn<QString>("variant");
    for (const StepRule& rule : stepTable()) {
        QStringList variants{QStringLiteral("control"), QStringLiteral("late"), QStringLiteral("overrun"),
                             QStringLiteral("wrong phase"), QStringLiteral("cleanup")};
        if (rule.otherPhase != Phase::None) variants << QStringLiteral("other phase");
        for (Need n : kNeeds)
            if (rule.needs & n) variants << QStringLiteral("drop %1").arg(needId(n));
        // A hold whose identity did not match proves neither.
        if (rule.needs & (NeedEngineStopped | NeedEngineFree)) variants << QStringLiteral("unvalidated hold");
        for (const QString& v : variants)
            QTest::newRow(qPrintable(QStringLiteral("%1 | %2").arg(stepId(rule.step), v))) << int(rule.step) << v;
    }
}

void tst_IrreversibleGate::judgeRefusesEachMissingPrecondition()
{
    QFETCH(int, step);
    QFETCH(QString, variant);
    const Step s = Step(step);
    const StepRule& rule = ruleFor(s);
    const Deadline now = Deadline(std::chrono::seconds(1000));
    Proofs p = everything(rule.phase, now + std::chrono::seconds(10));
    auto expect = [&](std::optional<GateRefusal::Kind> kind, Phase phase = Phase::None, unsigned missing = 0) {
        const std::optional<GateRefusal> r = judge(s, p, now);
        if (!kind) {
            QVERIFY2(!r, r ? kindName(r->kind) : "");
            return;
        }
        QVERIFY2(r, "admitted");
        QCOMPARE(kindName(r->kind), kindName(*kind));
        QCOMPARE(int(r->step), step);
        if (*kind == GateRefusal::Kind::Late) QCOMPARE(phaseName(r->phase), phaseName(phase));
        if (*kind == GateRefusal::Kind::Missing) QCOMPARE(r->missing, missing);
        QVERIFY(!r->describe().isEmpty());
    };
    if (variant == QLatin1String("control")) {
        expect(std::nullopt);
    } else if (variant == QLatin1String("other phase")) {
        p.phase = rule.otherPhase;
        expect(std::nullopt);
    } else if (variant == QLatin1String("late")) {
        p.phaseDeadline = now - std::chrono::milliseconds(1);
        expect(GateRefusal::Kind::Late, rule.phase);
    } else if (variant == QLatin1String("overrun")) {
        p.overrun = Phase::Prologue;   // an earlier phase was left late; this phase's own deadline is fine
        expect(GateRefusal::Kind::Late, Phase::Prologue);
    } else if (variant == QLatin1String("unvalidated hold")) {
        // engine.lock acquired, but its id is not the recorded one; no engine-free proof, a service installed.
        p.engineLockHeld = true;
        p.lockMatching = p.engineFree = p.notInstalled = false;
        expect(GateRefusal::Kind::Missing, Phase::None, rule.needs & (NeedEngineStopped | NeedEngineFree | NeedLockMatching));
    } else if (variant == QLatin1String("wrong phase")) {
        p.phase = Phase::Ack;          // no step is declared there
        expect(GateRefusal::Kind::WrongPhase);
    } else if (variant == QLatin1String("cleanup")) {
        // failClosed: no deadline and no overrun apply, but only the barrier steps may run there.
        p.phase = Phase::Cleanup;
        p.phaseDeadline = now - std::chrono::seconds(5);
        p.overrun = Phase::Barrier;
        if (rule.cleanup) expect(std::nullopt);
        else expect(GateRefusal::Kind::WrongPhase);
        if (rule.cleanup && (rule.needs & NeedTrustedFolder)) {
            p.trustedFolder = false;   // the needs still apply in the cleanup
            expect(GateRefusal::Kind::Missing, Phase::None, NeedTrustedFolder);
        }
    } else {
        Need n = NeedTrustedFolder;
        for (Need k : kNeeds)
            if (variant == QStringLiteral("drop ") + needId(k)) n = k;
        drop(p, n);
        expect(GateRefusal::Kind::Missing, Phase::None, unsigned(n));
    }
}

// A token is minted only by admit(), and spends once, for its own step.
void tst_IrreversibleGate::tokenIsSpentOnceForItsOwnStep()
{
    FakeClock clock;
    Proofs p = everything(Phase::Commit, clock.now() + std::chrono::seconds(10));
    IrreversibleGate gate(clock, [&p] { return p; });
    QVERIFY(!gate.changedBeyondPrologue());
    {
        GateToken t = gate.admit(Step::ConfigCommit);
        QVERIFY(gate.changedBeyondPrologue());
        bool refused = false;
        try {
            t.spend(Step::DropPending);   // another step's token
        } catch (const GateRefusal& r) {
            refused = r.kind == GateRefusal::Kind::WrongToken;
        }
        QVERIFY(refused);
        t.spend(Step::ConfigCommit);
        refused = false;
        try {
            t.spend(Step::ConfigCommit);  // spent
        } catch (const GateRefusal& r) {
            refused = r.kind == GateRefusal::Kind::WrongToken;
        }
        QVERIFY(refused);
    }
    // P4/P5 alone do not count as a change: a stop there is NothingChanged.
    p.phase = Phase::Prologue;
    IrreversibleGate g2(clock, [&p] { return p; });
    g2.admit(Step::FolderSecure).spend(Step::FolderSecure);
    g2.admit(Step::TempSweep).spend(Step::TempSweep);
    QVERIFY(!g2.changedBeyondPrologue());
    // A refusal is thrown, and is not a std::exception (nothing around a native call may swallow it).
    p.lockMatching = false;
    p.phase = Phase::Stage;
    bool thrown = false;
    try {
        g2.admit(Step::StagePending);
    } catch (const std::exception&) {
        QFAIL("a gate refusal must not be a std::exception");
    } catch (const GateRefusal& r) {
        thrown = r.kind == GateRefusal::Kind::Missing && r.missing == NeedLockMatching;
    }
    QVERIFY(thrown);
}

// ---------------------------------------------------------------------------------------------------
void tst_IrreversibleGate::stepPreconditionMatrix_data()
{
    QTest::addColumn<int>("row");
    QStringList names;
    const QList<GenRow> rows = generatedRows(&names);
    for (int i = 0; i < rows.size(); ++i) QTest::newRow(qPrintable(names[i])) << i;
}

void tst_IrreversibleGate::stepPreconditionMatrix()
{
    QFETCH(int, row);
    QStringList names;
    const GenRow g = generatedRows(&names).at(row);
    const QList<Scenario> scenarios = scenariosFor(g.step);
    QVERIFY2(!scenarios.isEmpty(), "a step of the table has no scenario that reaches it");
    const Scenario& sc = scenarios[g.scenario];

    // The control: without the fault, the scenario performs the step (so the row cannot pass vacuously).
    int at = -1;
    QString target;
    int occurrence = 0;
    {
        Rig r;
        PhaseClock pc;
        r.installedRunning(configA());
        r.world.log.phaseOf = [&pc] { return int(pc.load().phase); };
        sc.setup(r);
        const Result res = runOp(r, pc, sc.op);
        at = stepIndex(r.world.log, sc);
        QVERIFY2(at > 0, qPrintable(QStringLiteral("control: the step did not run (%1: %2)\n%3")
                                        .arg(enumName(res.outcome), res.message, r.world.log.all().join(QLatin1Char('\n')))));
        const QStringList all = r.world.log.all();
        target = all[at - 1];
        for (int i = 0; i < at; ++i)
            if (all[i] == target) ++occurrence;
    }

    Rig r;
    PhaseClock pc;
    r.installedRunning(configA());
    r.world.log.phaseOf = [&pc] { return int(pc.load().phase); };
    sc.setup(r);
    QString what;
    std::function<void(Rig&, const Result&)> verify;
    bool okWithoutStep = false;
    // The uninstall rows start from a station whose program and Apps & features entry are installed.
    r.service.entry = {true, r.service.installedExePath, r.service.version};
    if (g.need == 0) {
        // The deadline: the call just before the step returns after every phase budget.
        auto seen = std::make_shared<int>(0);
        r.world.log.onAdd = [&r, target, occurrence, seen](const QString& e) {
            if (e == target && ++*seen == occurrence) r.world.clock.advance(std::chrono::seconds(400));
        };
        what = QStringLiteral("late: ") + target;
    } else {
        const QList<Breaker> breakers = breakersFor(Need(g.need), sc.op);
        QVERIFY2(!breakers.isEmpty(), "a precondition of the table has no way to drop it");
        breakers[g.breaker].apply(r);
        verify = breakers[g.breaker].verify;
        okWithoutStep = breakers[g.breaker].okWithoutStep;
        what = QString::fromLatin1(breakers[g.breaker].name);
    }
    const Snapshot before = snapshot(r);
    const Result res = runOp(r, pc, sc.op);
    r.world.log.onAdd = {};
    const QString context = QStringLiteral("%1\n%2 / %3: %4\npostcondition: %5\n%6")
                                .arg(what, enumName(res.outcome), enumName(res.persisted), res.message, res.postcondition,
                                     r.world.log.all().join(QLatin1Char('\n')));

    // 1. The step was not performed.
    QVERIFY2(!performed(r.world.log, sc.detect), qPrintable(QStringLiteral("the step ran: ") + context));
    // 2. The result is true. Never Ok without the step (unless the operation succeeds without it by design) ...
    if (!okWithoutStep) QVERIFY2(res.outcome != Outcome::Ok, qPrintable(context));
    // ... a kept Apps & features entry names a program that exists (INV-126) ...
    QVERIFY2(r.service.entryNamesExistingProgram(), qPrintable(QStringLiteral("the entry names a removed program: ") + context));
    // ... persisted and its digest are what is on disk ...
    if (res.persisted == Persisted::None) QVERIFY2(res.persistedSha256.isEmpty(), qPrintable(context));
    if (res.persisted == Persisted::Staged) QCOMPARE(digest(r.file(kPending)), res.persistedSha256);
    if (res.persisted == Persisted::Committed) QCOMPARE(digest(r.file(kConfig)), res.persistedSha256);
    // ... a claim about config.json holds ...
    if (res.message.contains(QStringLiteral("config.json was not replaced"))) QCOMPARE(r.file(kConfig), before.config);
    // ... and NothingChanged means exactly that (the folder's own P4/P5 changes are listed in fileChanges).
    if (res.outcome == Outcome::NothingChanged) {
        QCOMPARE(r.file(kConfig), before.config);
        QCOMPARE(r.has(kPending), before.pending.has_value());
        if (before.pending) QCOMPARE(r.file(kPending), *before.pending);
        QCOMPARE(r.file(kToken), before.token);
        QCOMPARE(r.engineLock.lockContent(), before.lock);
        QCOMPARE(r.engineLock.recordContent(), before.record);
        QCOMPARE(r.service.startsBlocked, before.startsBlocked);
        QCOMPARE(r.service.engineRunning, before.engineRunning);
        QCOMPARE(r.firewall.rules, before.rules);
    }
    // ... and no stop is claimed ("stopped", "verified", "the bridge is off") while an engine process
    // survives: every such claim is engineClaim()'s, from a validated hold or the engine-free proof (also
    // "stopped, not activated" after a rejected lock identity).
    const bool engineAlive =
        r.service.engineRunning || r.service.engineHoldsLock || (r.service.stopRequested && r.service.neverReleases);
    if (engineAlive) {
        QVERIFY2(!claimsStop(res.postcondition), qPrintable(QStringLiteral("a stop claimed: ") + context));
        QVERIFY2(!claimsStop(res.message), qPrintable(QStringLiteral("a stop claimed: ") + context));
    }
    if (verify) {
        verify(r, res);
        if (QTest::currentTestFailed()) return;
    }
    // Every seam call under apply.lock, and both locks released.
    QVERIFY2(r.world.violations.isEmpty(), qPrintable(r.world.violations.join(QLatin1Char(','))));
    QVERIFY(!r.world.applyLock.held());
    QVERIFY(!r.engineLock.appliersHold());
}

// ---------------------------------------------------------------------------------------------------
// Bypass prevention, the part a compiler cannot check: Applier.cpp and Barrier.cpp hold no Platform and call
// no seam directly. GatedSeams is their only route, and GatedSeams' irreversible methods need a GateToken,
// which only IrreversibleGate::admit can construct.
namespace {
QString source(const QString& rel)
{
    QFile f(QStringLiteral(ASB_SOURCE_DIR "/") + rel);
    if (!f.open(QIODevice::ReadOnly)) return {};
    // Comments are not code: drop // ... to the end of each line.
    QString text = QString::fromUtf8(f.readAll());
    static const QRegularExpression comment(QStringLiteral("//[^\n]*"));
    return text.replace(comment, QString());
}
QStringList matches(const QString& text, const QRegularExpression& re)
{
    QStringList out;
    for (auto it = re.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const int line = int(text.left(m.capturedStart()).count(QLatin1Char('\n'))) + 1;
        out << QStringLiteral("line %1: %2").arg(line).arg(m.captured(0));
    }
    return out;
}
}  // namespace

void tst_IrreversibleGate::noDirectIrreversibleCalls()
{
    // A Platform field reached through anything ("x.service.", "x.io.", ...).
    const QRegularExpression field(QStringLiteral(
        R"(\.\s*(folder|service|firewall|applyLock|engineLock|token|status|io)\s*(\.|->|\())"));
    // A raw seam mutator, or a primitive that writes in the data folder or the lock directory.
    const QRegularExpression mutator(QStringLiteral(
        R"((?<!Applier::)\b(renameAside|createLockedDown|normalise|revoke|writeFresh|inhibitStarts|allowStarts|requestStop|)"
        R"(installOrConfigure|uninstall|removeAll|sweep|rollback|commitAs|writePending|atomicWrite|)"
        R"(reconcileOutstandingJobs|removeByHandle|truncate|writeAll|createTemp|acquireUntil|syncDir|)"
        // WS-16: the uninstall's own seams (D69, INV-125, INV-126)
        R"(markForDeletion|clearDeletion|removeProgram|removeProgramFolder|removeIfEmpty|writeAppsEntry|removeAppsEntry|)"
        R"(knownFiles)\s*\(|)"
        R"((\.|->)\s*(start|sync|remove|publish|connect|acquire)\s*\(|)"
        R"(LockIdentity::publish\s*\(|TrustedConfig::(open|read\w*)\s*\(|ConfigFiles::(writePending|read\w*)\s*\()"));
    // Platform appears only where the Applier hands it to the Txn, and the Txn to GatedSeams.
    const QRegularExpression platform(QStringLiteral(R"(\bPlatform\b|\bm_p\b)"));
    const QRegularExpression platformAllowed(QStringLiteral(
        R"(^\s*(Applier::Applier\(Platform& p, IClock& clock, PhaseClock\* phases\) : m_p\(p\), .*|)"
        R"(Txn\(Platform& platform, IClock& c, PhaseClock\* pc\)|)"
        R"(Txn t\(m_p, m_clock, m_phases\);|)"
        R"(Result untick\(Platform& p, IClock& clock, PhaseClock\* phases, .*|)"
        R"(return untick\(m_p, m_clock, m_phases, req, (true|false)(, true)?\);)\s*$)"));
    for (const QString& file : {QStringLiteral("src/core/Applier.cpp"), QStringLiteral("src/core/Barrier.cpp")}) {
        const QString text = source(file);
        QVERIFY2(!text.isEmpty(), qPrintable(QStringLiteral("cannot read ") + file));
        const QStringList f = matches(text, field);
        QVERIFY2(f.isEmpty(), qPrintable(file + QStringLiteral(": a platform field outside GatedSeams:\n") + f.join(QLatin1Char('\n'))));
        const QStringList m = matches(text, mutator);
        QVERIFY2(m.isEmpty(), qPrintable(file + QStringLiteral(": a direct mutation outside GatedSeams:\n") + m.join(QLatin1Char('\n'))));
        const QStringList lines = text.split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i)
            if (lines[i].contains(platform))
                QVERIFY2(lines[i].contains(platformAllowed),
                         qPrintable(QStringLiteral("%1 line %2 reaches the Platform: %3").arg(file).arg(i + 1).arg(lines[i])));
    }
    // GatedSeams hands out no seam: no public member returns an interface reference.
    const QString header = source(QStringLiteral("src/core/GatedSeams.h"));
    QVERIFY(!header.isEmpty());
    const QStringList leaks = matches(header, QRegularExpression(QStringLiteral(
        R"((IDataFolder|IEngineLock|FileIo|IServiceControl|IFirewall|IStartToken|IApplyLock|IStatusReader|Platform)\s*&\s*\w+\s*\()")));
    QVERIFY2(leaks.isEmpty(), qPrintable(leaks.join(QLatin1Char('\n'))));
    // Every method that takes a GateToken spends it, for a step, before anything else.
    const QString impl = source(QStringLiteral("src/core/GatedSeams.cpp"));
    QVERIFY(!impl.isEmpty());
    static const QRegularExpression gated(QStringLiteral(R"(GatedSeams::(\w+)\(GateToken t[^)]*\)\s*\{\s*([^\n]*))"));
    int n = 0;
    for (auto it = gated.globalMatch(impl); it.hasNext(); ++n) {
        const QRegularExpressionMatch m = it.next();
        QVERIFY2(m.captured(2).startsWith(QStringLiteral("t.spend(Step::")),
                 qPrintable(m.captured(1) + QStringLiteral(" does not spend its token first: ") + m.captured(2)));
    }
    QCOMPARE(n, 26);   // the gated methods of GatedSeams.h (openMutating admits per change instead)
}


// WS-5 round 6 (and the class audit): the files that word results, service messages and interim
// notifications hold no string that claims a stop. engineClaim() (EngineClaim.cpp) is the one place such
// wording exists, and it words only the evidence it is given, which the Applier derives from the gate's own
// proofs (Proofs::stopEvidence).
void tst_IrreversibleGate::stopClaimsOnlyFromTheProof()
{
    // A string literal (moc cannot read a raw string holding a quote, so this one is escaped).
    static const QRegularExpression literal(QStringLiteral("\"(?:[^\"\\\\\\n]|\\\\.)*\""));
    for (const QString& file : {QStringLiteral("src/core/Applier.cpp"), QStringLiteral("src/core/Barrier.cpp"),
                                QStringLiteral("src/core/PhaseClock.cpp"), QStringLiteral("src/core/OperationHost.cpp")}) {
        const QString text = source(file);
        QVERIFY2(!text.isEmpty(), qPrintable(QStringLiteral("cannot read ") + file));
        QStringList claims;
        for (const QString& m : matches(text, literal))
            if (claimsStop(m)) claims << m;
        QVERIFY2(claims.isEmpty(), qPrintable(file + QStringLiteral(": a stop worded outside engineClaim():\n") +
                                              claims.join(QLatin1Char('\n'))));
    }
    // The scan would pass vacuously if it could not see a claim: engineClaim's own file holds them.
    QStringList own;
    for (const QString& m : matches(source(QStringLiteral("src/core/EngineClaim.cpp")), literal))
        if (claimsStop(m)) own << m;
    QVERIFY2(own.size() >= 4, qPrintable(own.join(QLatin1Char('\n'))));
    // Unproven wording is not a claim; proven wording is.
    QVERIFY(!claimsStop(engineClaim(StopEvidence::None, EngineClaim::Disabled, QStringLiteral("stop requested"))));
    QVERIFY(!claimsStop(engineClaim(StopEvidence::None, EngineClaim::StopSentence)));
    QVERIFY(engineClaim(StopEvidence::None, EngineClaim::OffSentence).isEmpty());
    QVERIFY(!claimsStop(engineClaim(StopEvidence::None, EngineClaim::Interim)));
    for (StopEvidence e : {StopEvidence::ValidatedHold, StopEvidence::EngineFree}) {
        QVERIFY(claimsStop(engineClaim(e, EngineClaim::Disabled)));
        QVERIFY(claimsStop(engineClaim(e, EngineClaim::StartTypeKept)));
        QVERIFY(claimsStop(engineClaim(e, EngineClaim::StopSentence)));
        QVERIFY(claimsStop(engineClaim(e, EngineClaim::OffSentence)));
        QVERIFY(claimsStop(engineClaim(e, EngineClaim::Interim)));
    }
    QVERIFY(engineClaim(StopEvidence::ValidatedHold, EngineClaim::Disabled).contains(kVerified));
    QVERIFY(!engineClaim(StopEvidence::EngineFree, EngineClaim::Disabled).contains(kVerified));
}

QTEST_GUILESS_MAIN(tst_IrreversibleGate)
#include "tst_IrreversibleGate.moc"
