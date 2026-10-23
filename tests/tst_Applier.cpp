// The Applier transaction (master §4.6), against the fakes of every platform seam. The WS-5 fault matrix (`faultMatrix`, with
// `outcomeByStep` and `persistedByStep` over the same rows) asserts outcome, persisted + digest,
// config.json and the service postcondition for every injected fault.

#include "core/Applier.h"
#include "core/ConfigFiles.h"
#include "core/ConfigJson.h"
#include "core/LockIdentity.h"
#include "core/StartToken.h"
#include "core/TrustedConfig.h"
#include "support/Fakes.h"

#include <QTest>

#include <future>
#include <memory>
#include <thread>

using namespace asb;
using namespace asb::test;

namespace {

const QString kConfig = QString::fromLatin1(ConfigFiles::kConfig);
const QString kPending = QString::fromLatin1(ConfigFiles::kPending);
const QString kToken = QString::fromLatin1(ConfigFiles::kStartToken);

SaveRequest request(const QByteArray& bytes, Source::Kind kind = Source::Committed, const QByteArray& sourceBytes = {},
                    bool supersede = false)
{
    SaveRequest r;
    r.config = bytes;
    r.source.kind = kind;
    r.source.sha256 = digest(sourceBytes.isEmpty() ? configA() : sourceBytes);
    r.supersedePending = supersede;
    return r;
}

Applier applier(Rig& r)
{
    return Applier(*r.platform, r.world.clock);
}

QString events(const Rig& r)
{
    return r.world.log.all().join(QLatin1Char('\n'));
}

// The index of the first event starting with `prefix`, failing the test if there is none.
int at(const Rig& r, const QString& prefix, int from = 0)
{
    const int i = r.world.log.indexOf(prefix, from);
    if (i < 0) qWarning("missing event %s in:\n%s", qPrintable(prefix), qPrintable(events(r)));
    return i;
}

// What a viewer sees after a reload (ReadOnly): committed bytes, and pending bytes if any.
struct Reloaded {
    QByteArray committed;
    std::optional<QByteArray> pending;
};
Reloaded reload(Rig& r)
{
    const bool check = r.world.checkLock;
    r.world.checkLock = false;
    const OpenedFolder o = TrustedConfig::open(r.folder, TrustMode::ReadOnly);
    Reloaded out;
    if (o.usable) {
        out.committed = TrustedConfig::readCommitted(r.folder, o.inspection).bytes;
        const ConfigFiles::Loaded p = TrustedConfig::readPending(r.folder, o.inspection);
        if (p.present) out.pending = p.bytes;
    }
    r.world.checkLock = check;
    return out;
}

// A power loss: only durable state survives; the dead process's engine is gone too.
void powerLoss(Rig& r)
{
    r.world.files.crash();
    r.world.lockFiles.crash();
    r.service.engineRunning = false;
    r.service.engineHoldsLock = false;
}

// ---------------------------------------------------------------------------------------------------
// The fault matrix (WS-5 doc). Every row: a setup on an installed, running station with config A
// committed, then Save & apply of N = B (unless the row says otherwise).
// ---------------------------------------------------------------------------------------------------
enum class Op { RunN, RunInstallN, Start, Edit, Repair, SaveAndRemoveN, DeactivateN };

struct Row {
    const char* name;
    Op op;
    std::function<void(Rig&)> setup;
    Outcome outcome;
    Persisted persisted;
    QByteArray committedAfter;          // config.json bytes afterwards
    const char* postcondition;          // a substring of Result::postcondition ("" = not checked)
    std::function<void(Rig&, const Result&)> extra = {};
};

const char* kVerified = "verified: engine lock acquired";
const char* kUnproven = "could not prove the bridge stopped";

QList<Row> rows()
{
    QList<Row> t;
    t << Row{"apply-lock timeout", Op::RunN, [](Rig& r) { r.world.applyLock.alwaysTimeout = true; }, Outcome::NothingChanged,
             Persisted::None, configA(), "", [](Rig& r, const Result& res) {
                 QVERIFY(res.message.contains(QStringLiteral("another operation is still running")));
                 QVERIFY(!r.world.log.contains(QStringLiteral("service:")));
             }};
    t << Row{"apply lock refused (insecure lock directory)", Op::RunN,
             [](Rig& r) { r.world.applyLock.refuse = QStringLiteral("C:\\Program Files\\AetherSerialBridge is not secured"); },
             Outcome::NothingChanged, Persisted::None, configA(), "", [](Rig& r, const Result& res) {
                 QVERIFY2(res.message.contains(QStringLiteral("AetherSerialBridge is not secured")), qPrintable(res.message));
                 QVERIFY(!res.message.contains(QStringLiteral("another operation")));
                 QVERIFY(!r.world.log.contains(QStringLiteral("service:")));
             }};
    t << Row{"installation query fails", Op::RunN, [](Rig& r) { r.service.fail[QStringLiteral("detect")] = QStringLiteral("SCM down"); },
             Outcome::NothingChanged, Persisted::None, configA(), "", [](Rig& r, const Result&) {
                 QVERIFY(!r.world.log.contains(QStringLiteral("service:inhibitStarts")));
                 QVERIFY(!r.world.log.contains(QStringLiteral("io:")));
             }};
    t << Row{"Foreign", Op::RunN, [](Rig& r) { r.service.installation.state = InstallState::Foreign; }, Outcome::NothingChanged,
             Persisted::None, configA(), "", [](Rig& r, const Result&) { QVERIFY(!r.world.log.contains(QStringLiteral("firewall:"))); }};
    t << Row{"folder untrusted", Op::RunN, [](Rig& r) { r.folder.owner = QStringLiteral("attacker"); }, Outcome::NothingChanged,
             Persisted::None, configA(), "", [](Rig& r, const Result& res) {
                 QVERIFY(res.message.contains(QStringLiteral("Repair")));
                 QVERIFY(!r.world.log.contains(QStringLiteral("service:inhibitStarts")));
                 QCOMPARE(r.folder.renames, 0);
             }};
    t << Row{"precedence conflict", Op::RunN,
             [](Rig& r) { r.world.files.plantFile(kPending, configC(), QStringLiteral("root")); }, Outcome::NothingChanged,
             Persisted::None, configA(), "", [](Rig& r, const Result&) {
                 QCOMPARE(r.file(kPending), configC());
                 QVERIFY(!r.world.log.contains(QStringLiteral("service:inhibitStarts")));
             }};
    t << Row{"block-starts call fails, B3 still proves the stop", Op::RunN,
             [](Rig& r) { r.service.fail[QStringLiteral("inhibitStarts")] = QStringLiteral("access denied"); },
             Outcome::Uncommitted, Persisted::None, configA(), kVerified};
    t << Row{"block-starts call fails, engine never stops", Op::RunN,
             [](Rig& r) {
                 r.service.fail[QStringLiteral("inhibitStarts")] = QStringLiteral("access denied");
                 r.service.neverReleases = true;
             },
             Outcome::Uncommitted, Persisted::None, configA(), kUnproven};
    t << Row{"engine lock never released (alive past the deadline)", Op::RunN, [](Rig& r) { r.service.neverReleases = true; },
             Outcome::StateUnknown, Persisted::None, configA(), kUnproven, [](Rig& r, const Result&) {
                 for (const char* e : {"service:installOrConfigure", "io:createTemp", "firewall:", "io:commitAs"})
                     QVERIFY2(!r.world.log.contains(QString::fromLatin1(e)), e);
             }};
    t << Row{"engine lock released only after polls (STOPPED-before-exit)", Op::RunN,
             [](Rig& r) { r.service.releaseAfterPolls = 20; }, Outcome::Ok, Persisted::Committed, configB(), "running"};
    t << Row{"partial install (created auto+running, then throws)", Op::RunInstallN,
             [](Rig& r) { r.service.partialInstallRunning = true; }, Outcome::Uncommitted, Persisted::None, configA(), kVerified,
             [](Rig& r, const Result&) { QVERIFY(r.service.startsBlocked); }};
    t << Row{"R4 staging write fails", Op::RunN, [](Rig& r) { r.world.files.failNext(FakeFileIo::Op::CreateTemp); },
             Outcome::Uncommitted, Persisted::None, configA(), kVerified,
             [](Rig& r, const Result&) { QVERIFY(!r.has(kPending)); }};
    t << Row{"temp read-back fails", Op::RunN, [](Rig& r) { r.world.files.failNext(FakeFileIo::Op::ReadBack); },
             Outcome::Uncommitted, Persisted::Staged, configA(), kVerified};
    t << Row{"temp read-back invalid", Op::RunN,
             [](Rig& r) { r.io.tamperReadBack = [](const QByteArray&) { return QByteArray("{\"version\":1,\"ports\":[{"); }; },
             Outcome::Uncommitted, Persisted::Staged, configA(), kVerified,
             [](Rig& r, const Result&) { QVERIFY(!r.world.log.contains(QStringLiteral("firewall:sync"))); }};
    t << Row{"firewall fails", Op::RunN, [](Rig& r) { r.firewall.fail[QStringLiteral("sync")] = QStringLiteral("WBEM_E_FAILED"); },
             Outcome::Uncommitted, Persisted::Staged, configA(), kVerified,
             [](Rig& r, const Result&) { QVERIFY(r.world.log.contains(QStringLiteral("firewall:rollback"))); }};
    t << Row{"firewall connect fails", Op::RunN,
             [](Rig& r) { r.firewall.fail[QStringLiteral("connect")] = QStringLiteral("RPC unavailable"); }, Outcome::Uncommitted,
             Persisted::Staged, configA(), kVerified};
    t << Row{"rename fails after sync (rules new, no token)", Op::RunN,
             [](Rig& r) {
                 r.firewall.onCall = [&r](const QString& m) {
                     if (m == QLatin1String("sync")) r.world.files.failNext(FakeFileIo::Op::Rename);
                 };
             },
             Outcome::Uncommitted, Persisted::Staged, configA(), kVerified,
             [](Rig& r, const Result&) {
                 QVERIFY(!r.has(kToken));
                 QVERIFY(r.service.startsBlocked);
                 QCOMPARE(r.file(kPending), configB());   // INV-99: committed A, pending B
             }};
    t << Row{"directory durability fails after the rename", Op::RunN,
             [](Rig& r) {
                 r.firewall.onCall = [&r](const QString& m) {
                     if (m == QLatin1String("sync")) r.world.files.failNext(FakeFileIo::Op::SyncDir);
                 };
             },
             Outcome::Uncommitted, Persisted::Staged, configB(), kVerified,
             [](Rig& r, const Result&) { QCOMPARE(r.file(kPending), configB()); }};
    t << Row{"token write fails in R9", Op::RunN,
             [](Rig& r) {
                 r.firewall.onCall = [&r](const QString& m) {
                     if (m == QLatin1String("sweep")) r.world.files.failNext(FakeFileIo::Op::CreateTemp);
                 };
             },
             Outcome::CommittedNotRunning, Persisted::Committed, configB(), kVerified,
             [](Rig& r, const Result&) { QVERIFY(!r.has(kToken)); }};
    t << Row{"R8 pending delete fails (continues)", Op::RunN,
             [](Rig& r) {
                 r.firewall.onCall = [&r](const QString& m) {
                     if (m == QLatin1String("sync")) r.world.files.failNext(FakeFileIo::Op::Remove);
                 };
             },
             Outcome::Ok, Persisted::Committed, configB(), "running",
             [](Rig& r, const Result&) { QCOMPARE(r.file(kPending), configB()); }};
    t << Row{"enable fails", Op::RunN, [](Rig& r) { r.service.fail[QStringLiteral("allowStarts")] = QStringLiteral("denied"); },
             Outcome::CommittedNotRunning, Persisted::Committed, configB(), kVerified};
    t << Row{"start throws after the service came up", Op::RunN, [](Rig& r) { r.service.startThrowsAfterRunning = true; },
             Outcome::CommittedNotRunning, Persisted::Committed, configB(), kVerified, [](Rig& r, const Result&) {
                 QVERIFY(!r.service.engineRunning);
                 QVERIFY(r.service.startsBlocked);
                 QVERIFY(!r.has(kToken));
             }};
    t << Row{"ACK with configError", Op::RunN, [](Rig& r) { r.service.ack = AckMode::ConfigError; }, Outcome::CommittedNotRunning,
             Persisted::Committed, configB(), kVerified};
    t << Row{"ACK timeout", Op::RunN, [](Rig& r) { r.service.ack = AckMode::Never; }, Outcome::StateUnknown, Persisted::Committed,
             configB(), "unconfirmed", [](Rig& r, const Result&) { QVERIFY(r.service.engineRunning); }};
    t << Row{"stale status with the old token", Op::RunN, [](Rig& r) { r.service.ack = AckMode::StaleToken; },
             Outcome::StateUnknown, Persisted::Committed, configB(), "unconfirmed"};
    t << Row{"lock-id mismatch at B3", Op::RunN,
             [](Rig& r) {
                 r.engineLock.peekUnreadable = true;   // Windows: the prologue cannot see it
                 r.engineLock.plantRecord(QByteArrayLiteral("{\"lockId\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}"));
             },
             Outcome::StateUnknown, Persisted::None, configA(), kUnproven, [](Rig& r, const Result& res) {
                 QVERIFY2(res.message.contains(QStringLiteral("start token revoked, starts blocked, stop requested")),
                          qPrintable(res.message));
                 // The lock's identity was rejected: nothing proves the stop, so nothing claims it (WS-5 round 6,
                 // This said "stopped, not activated"). The barrier's effects are kept.
                 QVERIFY2(res.postcondition.contains(QStringLiteral("start token revoked, starts blocked, stop requested")),
                          qPrintable(res.postcondition));
                 QVERIFY2(!claimsStop(res.postcondition) && !claimsStop(res.message), qPrintable(res.postcondition));
                 QVERIFY(!res.message.contains(QStringLiteral("not changed")));
                 QVERIFY(!r.world.log.contains(QStringLiteral("io:createTemp")));
             }};
    t << Row{"failClosed barrier deadline", Op::RunN,
             [](Rig& r) {
                 r.service.startThrowsAfterRunning = true;
                 r.service.onCall = [&r](const QString& m) {
                     if (m == QLatin1String("start")) r.service.neverReleases = true;
                 };
             },
             Outcome::CommittedNotRunning, Persisted::Committed, configB(), kUnproven};
    t << Row{"cleanup stop fails (the engine stays running)", Op::RunN,
             [](Rig& r) {
                 r.service.fail[QStringLiteral("requestStop")] = QStringLiteral("access denied");
             },
             Outcome::Uncommitted, Persisted::None, configA(), kUnproven, [](Rig& r, const Result&) {
                 QVERIFY(r.service.engineRunning);
             }};
    t << Row{"Edit: pending write fails", Op::Edit, [](Rig& r) { r.world.files.failNext(FakeFileIo::Op::CreateTemp); },
             Outcome::NothingChanged, Persisted::None, configA(), "", [](Rig& r, const Result&) {
                 QVERIFY(!r.world.log.contains(QStringLiteral("service:inhibitStarts")));
                 QVERIFY(r.service.engineRunning);
             }};
    t << Row{"Repair: rename aside fails", Op::Repair,
             [](Rig& r) {
                 r.world.files.plantFile(QStringLiteral("planted.txt"), "x");   // untrusted child
                 r.folder.failRenameAside = QStringLiteral("access denied");
             },
             Outcome::NothingChanged, Persisted::None, configA(), kVerified, [](Rig& r, const Result& res) {
                 QVERIFY(!res.fileChanges.join(QLatin1Char(' ')).contains(QStringLiteral("moved")));
                 QVERIFY(r.service.startsBlocked);
             }};
    t << Row{"SaveAndRemove: pending write fails", Op::SaveAndRemoveN,
             [](Rig& r) { r.world.files.failNext(FakeFileIo::Op::CreateTemp); }, Outcome::NothingChanged, Persisted::None,
             configA(), "", [](Rig& r, const Result&) {
                 QVERIFY(!r.world.log.contains(QStringLiteral("service:inhibitStarts")));
                 QVERIFY(r.service.engineRunning);
             }};
    t << Row{"Deactivate: pending write fails", Op::DeactivateN,
             [](Rig& r) { r.world.files.failNext(FakeFileIo::Op::CreateTemp); }, Outcome::NothingChanged, Persisted::None,
             configA(), ""};
    t << Row{"SaveAndRemove: uninstall fails after the rules were removed", Op::SaveAndRemoveN,
             [](Rig& r) { r.service.fail[QStringLiteral("uninstall")] = QStringLiteral("marked for deletion"); },
             Outcome::SavedNotApplied, Persisted::Staged, configA(), kVerified, [](Rig& r, const Result& res) {
                 QVERIFY(res.postcondition.contains(QStringLiteral("service not removed")));
                 QVERIFY(r.firewall.rules.isEmpty());
                 QCOMPARE(r.file(kPending), configB());   // INV-99: committed A, pending B
             }};
    t << Row{"P4 created the folder, then a later step fails", Op::RunInstallN,
             [](Rig& r) {
                 r.world.files.eraseEntry(kConfig);
                 r.world.files.eraseEntry(kToken);
                 r.folder.state = FakeFolder::DirState::Missing;
                 r.service.installation.state = InstallState::NotInstalled;
                 r.service.engineRunning = r.service.engineHoldsLock = false;
                 r.world.lockFiles.eraseEntry(QStringLiteral("engine.lock"));
                 r.world.lockFiles.eraseEntry(QStringLiteral("engine.lock.id"));
                 r.firewall.fail[QStringLiteral("sync")] = QStringLiteral("WBEM_E_FAILED");
             },
             Outcome::Uncommitted, Persisted::Staged, QByteArray(), kVerified, [](Rig&, const Result& res) {
                 QVERIFY(res.fileChanges.contains(QStringLiteral("created the data folder")));
                 QVERIFY(res.message.contains(QStringLiteral("created the data folder")));
             }};
    t << Row{"P4 normalise changes some ACLs, then fails", Op::RunN,
             [](Rig& r) {
                 r.folder.normaliseChanges = {QStringLiteral("reset the permissions of config.json")};
                 r.folder.failNormalise = QStringLiteral("could not reset the permissions of start.token");
             },
             Outcome::NothingChanged, Persisted::None, configA(), "", [](Rig& r, const Result& res) {
                 QCOMPARE(res.fileChanges, QStringList{QStringLiteral("reset the permissions of config.json")});
                 QVERIFY2(res.message.contains(QStringLiteral("Folder changes: reset the permissions of config.json")),
                          qPrintable(res.message));
                 QVERIFY(!r.world.log.contains(QStringLiteral("service:inhibitStarts")));
             }};
    t << Row{"B0 revocation flush fails", Op::RunN, [](Rig& r) { r.world.files.failNext(FakeFileIo::Op::FlushExisting); },
             Outcome::Uncommitted, Persisted::None, configA(), "may already be invalidated", [](Rig& r, const Result& res) {
                 // Nothing asked the bridge to stop, and the postcondition says so (WS-5 round 6 audit).
                 QVERIFY2(res.postcondition.contains(QStringLiteral("a running bridge was left running")), qPrintable(res.postcondition));
                 QVERIFY(!claimsStop(res.postcondition));
                 QVERIFY(!r.world.log.contains(QStringLiteral("service:inhibitStarts")));
                 QVERIFY(!r.world.log.contains(QStringLiteral("service:requestStop")));
             }};
    t << Row{"token missing at B0 (continues)", Op::RunN, [](Rig& r) { r.world.files.eraseEntry(kToken); }, Outcome::Ok,
             Persisted::Committed, configB(), "running"};
    t << Row{"token written but not durable in R9", Op::RunN,
             [](Rig& r) {
                 r.firewall.onCall = [&r](const QString& m) {
                     if (m == QLatin1String("sweep")) r.world.files.failNext(FakeFileIo::Op::SyncDir);
                 };
             },
             Outcome::Ok, Persisted::Committed, configB(), "running",
             [](Rig&, const Result& res) { QVERIFY(res.message.contains(QStringLiteral("power loss"))); }};
    t << Row{"native call returns after its deadline (firewall)", Op::RunN,
             [](Rig& r) {
                 r.firewall.onCall = [&r](const QString& m) {
                     if (m == QLatin1String("sync")) r.world.clock.advance(std::chrono::seconds(61));
                 };
             },
             Outcome::StateUnknown, Persisted::Staged, configA(), kVerified, [](Rig& r, const Result& res) {
                 QCOMPARE(res.step, phaseName(Phase::Firewall));
                 QVERIFY(!r.world.log.contains(QStringLiteral("io:commitAs config.json")));
             }};
    // A fresh install (no barrier) over a leftover mismatched pair on Windows, where the prologue
    // cannot read engine.lock ("unknown"): the ids are compared through the held lock, and the pair is
    // recovered under the engine-free proof instead of being installed as is (the engine would exit 4).
    t << Row{"NotInstalled x peekUnreadable x mismatched", Op::RunInstallN,
             [](Rig& r) {
                 r.service.installation.state = InstallState::NotInstalled;
                 r.service.engineRunning = r.service.engineHoldsLock = false;
                 r.service.pid = 0;
                 r.engineLock.windows();
                 r.engineLock.plantLock(LockIdentity::format(QByteArrayLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")));
             },
             Outcome::Ok, Persisted::Committed, configB(), "running", [](Rig& r, const Result&) {
                 const auto lock = LockIdentity::parse(r.engineLock.lockContent());
                 QVERIFY(lock);
                 QCOMPARE(LockIdentity::parse(r.engineLock.recordContent()), lock);
                 QVERIFY(*lock != QByteArrayLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
                 QVERIFY(*lock != QByteArrayLiteral("5555555555555555aaaaaaaaaaaaaaaa"));
                 QVERIFY(!r.world.log.contains(QStringLiteral("engine:exit 4")));
                 QVERIFY(r.world.log.indexOf(QStringLiteral("lockio:commitAs")) >
                         r.world.log.indexOf(QStringLiteral("engineLock:readHeld")));
             }};
    // What a late call did on disk is recorded before lateness is judged. A commit (R7) that
    // succeeded durably but returned after its deadline is Committed, and the late result says so; the same
    // for R4's staging. A rename that happened without its directory sync is named as not durable.
    auto lateCommit = [](const char* target, bool syncFails) {
        return [target, syncFails](Rig& r) {
            r.io.beforeHook = [&r, target, syncFails](const QString& op, const QString& name) {
                if (op != QLatin1String("commitAs") || name != QLatin1String(target)) return;
                r.world.clock.advance(std::chrono::seconds(31));   // the call itself outlives its 30 s phase
                if (syncFails) r.world.files.failNext(FakeFileIo::Op::SyncDir);
            };
        };
    };
    t << Row{"durable commit returns late", Op::RunN, lateCommit("config.json", false), Outcome::StateUnknown,
             Persisted::Committed, configB(), kVerified, [](Rig& r, const Result& res) {
                 QCOMPARE(res.step, phaseName(Phase::Commit));
                 QVERIFY2(res.message.contains(QStringLiteral("The new settings are saved")), qPrintable(res.message));
                 QVERIFY(!r.has(kToken));   // nothing from R8 on ran
                 QVERIFY(r.service.startsBlocked);
             }};
    t << Row{"commit renamed but not durable, returns late", Op::RunN, lateCommit("config.json", true),
             Outcome::StateUnknown, Persisted::Staged, configB(), kVerified, [](Rig& r, const Result& res) {
                 QVERIFY2(res.message.contains(QStringLiteral("may not survive a power loss")), qPrintable(res.message));
                 QCOMPARE(r.file(kPending), configB());
             }};
    t << Row{"durable staging returns late", Op::RunN, lateCommit("config.pending.json", false), Outcome::StateUnknown,
             Persisted::Staged, configA(), kVerified, [](Rig& r, const Result& res) {
                 QCOMPARE(res.step, phaseName(Phase::Stage));
                 QVERIFY2(res.message.contains(QStringLiteral("saved but not applied")), qPrintable(res.message));
                 QVERIFY2(res.message.contains(QStringLiteral("config.json was not replaced")), qPrintable(res.message));
                 QCOMPARE(r.file(kPending), configB());
                 QVERIFY(!r.world.log.contains(QStringLiteral("firewall:sync")));   // nothing after R4 ran
             }};
    // Audit: a late install call may have succeeded; the result says what it may have left, not "nothing".
    t << Row{"install returns late", Op::RunInstallN,
             [](Rig& r) {
                 r.service.onCall = [&r](const QString& m) {
                     if (m == QLatin1String("installOrConfigure")) r.world.clock.advance(std::chrono::seconds(61));
                 };
             },
             Outcome::StateUnknown, Persisted::None, configA(), kVerified, [](Rig& r, const Result& res) {
                 QCOMPARE(res.step, phaseName(Phase::Install));
                 QVERIFY2(res.message.contains(QStringLiteral("returned after its deadline")), qPrintable(res.message));
                 QVERIFY2(res.message.contains(QStringLiteral("config.json was not replaced")), qPrintable(res.message));
                 QVERIFY(!res.message.contains(QStringLiteral("nothing was applied")));
                 QVERIFY(!r.world.log.contains(QStringLiteral("io:createTemp")));
             }};
    t << Row{"Start: config.json unreadable", Op::Start,
             [](Rig& r) { r.world.files.plantFile(kConfig, QByteArrayLiteral("{\"version\":1,\"ports\":["), QStringLiteral("root")); },
             Outcome::Uncommitted, Persisted::None, QByteArray(), kVerified, [](Rig& r, const Result& res) {
                 QCOMPARE(res.step, QStringLiteral("read the saved config"));
                 QVERIFY(!r.world.log.contains(QStringLiteral("firewall:sync")));
             }};
    t << Row{"Start: firewall fails", Op::Start, [](Rig& r) { r.firewall.fail[QStringLiteral("sync")] = QStringLiteral("x"); },
             Outcome::Uncommitted, Persisted::None, configA(), kVerified};
    // The same fresh install over a corrupt engine.lock with NO record. The prologue cannot read
    // the lock (Windows), so it classifies the pair "partial"; the recovery is chosen from the lock as read
    // through the hold ("mismatched": both files replaced), before the engine-free proof, never from the
    // prologue's guess (which refused "corrupt" on every retry: a dead end).
    t << Row{"NotInstalled x peekUnreadable x corrupt lock, no record", Op::RunInstallN,
             [](Rig& r) {
                 r.service.installation.state = InstallState::NotInstalled;
                 r.service.engineRunning = r.service.engineHoldsLock = false;
                 r.service.pid = 0;
                 r.engineLock.windows();
                 r.engineLock.plantLock(QByteArrayLiteral("{\"lockId\":\"corrupt\"}"));
                 r.world.lockFiles.eraseEntry(QStringLiteral("engine.lock.id"));
                 r.world.lockFiles.syncDir(FakeDir(), nullptr);
                 // What the prologue sees: an unreadable lock and no record, which is "partial".
                 QCOMPARE(int(LockIdentity::classify(r.engineLock.peekLock(), r.engineLock.readRecord())),
                          int(LockIdentity::PairState::Partial));
                 r.world.log.clear();
             },
             Outcome::Ok, Persisted::Committed, configB(), "running", [](Rig& r, const Result&) {
                 const auto lock = LockIdentity::parse(r.engineLock.lockContent());
                 QVERIFY(lock);
                 QCOMPARE(LockIdentity::parse(r.engineLock.recordContent()), lock);
                 QVERIFY(!r.world.log.contains(QStringLiteral("engine:exit 4")));
                 // Judged through the hold before the engine-free proof; published only after it.
                 const int held = r.world.log.indexOf(QStringLiteral("engineLock:readHeld"));
                 const int proof = r.world.log.indexOf(QStringLiteral("service:query"),
                                                       r.world.log.indexOf(QStringLiteral("service:installOrConfigure")));
                 QVERIFY2(held >= 0 && held < proof, qPrintable(r.world.log.all().join(QLatin1Char('\n'))));
                 QVERIFY(proof < r.world.log.indexOf(QStringLiteral("lockio:commitAs engine.lock")));
             }};
    // An insecure lock file, through the same fresh-install path: acquiring the lock reports an
    // insecure FILE, which is a mismatched pair; replaced under the engine-free proof, not refused.
    t << Row{"NotInstalled x Windows x insecure lock file", Op::RunInstallN,
             [](Rig& r) {
                 r.service.installation.state = InstallState::NotInstalled;
                 r.service.engineRunning = r.service.engineHoldsLock = false;
                 r.service.pid = 0;
                 r.engineLock.windows();
                 r.engineLock.plantInsecureLock(LockIdentity::format(QByteArrayLiteral("5555555555555555aaaaaaaaaaaaaaaa")));
             },
             Outcome::Ok, Persisted::Committed, configB(), "running", [](Rig& r, const Result&) {
                 QVERIFY(!r.engineLock.lockFileInsecure());
                 const auto lock = LockIdentity::parse(r.engineLock.lockContent());
                 QVERIFY(lock);
                 QCOMPARE(LockIdentity::parse(r.engineLock.recordContent()), lock);
                 QVERIFY(r.world.log.indexOf(QStringLiteral("lockio:commitAs engine.lock")) >
                         r.world.log.indexOf(QStringLiteral("service:query"),
                                             r.world.log.indexOf(QStringLiteral("service:installOrConfigure"))));
             }};
    // R8's calls (remove the equal pending file, then sync the directory) are judged against the
    // Commit phase's deadline before R9: a late return stops there, so no token is published, starts stay
    // blocked and the bridge is not started; the commit itself stays recorded.
    auto lateR8 = [](bool syncDir) {
        return [syncDir](Rig& r) {
            auto state = std::make_shared<int>(0);   // 0 waiting, 1 armed, 2 done
            r.io.beforeHook = [&r, syncDir, state](const QString& op, const QString& name) {
                const bool removal = op == QLatin1String("remove") && name == QLatin1String("config.pending.json");
                const bool arm = syncDir ? removal : op == QLatin1String("commitAs") && name == QLatin1String("config.json");
                const bool late = syncDir ? op == QLatin1String("syncDir") : removal;
                if (*state == 0 && arm) {
                    *state = 1;
                } else if (*state == 1 && late) {
                    *state = 2;
                    r.world.clock.advance(std::chrono::seconds(31));   // the call outlives its 30 s phase
                }
            };
        };
    };
    auto r8Late = [](Rig& r, const Result& res) {
        QCOMPARE(res.step, phaseName(Phase::Commit));
        QVERIFY2(res.message.contains(QStringLiteral("The new settings are saved")), qPrintable(res.message));
        QVERIFY(!r.has(kToken));
        QVERIFY(r.service.startsBlocked);
        QVERIFY(!r.world.log.contains(QStringLiteral("firewall:sweep")));
        QVERIFY(!r.world.log.contains(QStringLiteral("service:allowStarts")));
        QVERIFY(!r.service.engineRunning);
    };
    t << Row{"R8 pending removal returns late", Op::RunN, lateR8(false),
             Outcome::StateUnknown, Persisted::Committed, configB(), kVerified, r8Late};
    t << Row{"R8 directory sync returns late", Op::RunN, lateR8(true),
             Outcome::StateUnknown, Persisted::Committed, configB(), kVerified, r8Late};
    return t;
}

Result runRow(Rig& r, const Row& row)
{
    Applier a = applier(r);
    switch (row.op) {
    case Op::RunN: return a.run(request(configB()), false);
    case Op::RunInstallN: return a.run(request(configB()), true);
    case Op::Start: return a.start();
    case Op::Edit: return a.edit([](BridgeConfig& c) { c.ports[0].tcpPort = 4005; });
    case Op::Repair: return a.repair();
    case Op::SaveAndRemoveN: return a.saveAndRemove(request(configB()));
    case Op::DeactivateN: return a.deactivate(request(configB()));
    }
    return {};
}

void addRows()
{
    QTest::addColumn<int>("row");
    const QList<Row> t = rows();
    for (int i = 0; i < t.size(); ++i) QTest::newRow(t[i].name) << i;
}

}  // namespace

class tst_Applier : public QObject {
    Q_OBJECT
private slots:
    // The happy paths
    void saveAndApplyRunsEveryStepInOrderUnderTheLock();
    void orderUnderLock();
    void firstInstallWritesEmptyConfig();
    void applyPromotesPending();
    void startResyncsFirewall();
    void stopRevokesStopsAndKeepsTheStartType();
    // INV-39, INV-76, INV-106: the fault matrix
    void faultMatrix_data();
    void faultMatrix();
    void outcomeByStep_data();
    void outcomeByStep();
    void persistedByStep_data();
    void persistedByStep();
    // INV-24
    void lockHeldAcrossEveryStep_data();
    void lockHeldAcrossEveryStep();
    void saveAndRemoveUsesTrustedHandles();
    void discardPendingUsesTrustedHandles();
    // INV-125, INV-126 (WS-16): uninstall
    void uninstallRemovesServiceProgramLocksAndEntry();
    void uninstallKeepsSettingsByDefault();
    void uninstallRemovesKnownSettingsWhenAsked();
    void uninstallKeepsUnknownEntries();
    void uninstallUntrustedSettingsUntouched();
    void uninstallSettingsOnlyAfterServiceAndProgramGone();
    void uninstallFailureBeforeSettingsTouchesNoSettings();
    void uninstallListsEverythingKept();
    void uninstallLockDispositionFailureKeepsProgramAndEntry();
    void uninstallRetryFromNotInstalledFinishes();
    void uninstallProgramKeptKeepsLockFiles();
    void uninstallRunningImageScheduledCountsAsRemoved();
    void uninstallLinuxRemovesProgramWithTheUnit();
    void uninstallLateAfterProgramGoneStillRemovesEntry();
    void uninstallThrowingProgramRemovalKeepsLockFiles();
    void uninstallLateAfterRecordRemovesEmptyProgramFolder();
    // INV-127
    void discardPendingIfMatches();
    void discardPendingIfReplaced();
    void discardPendingIfMissing();
    // INV-25, INV-73
    void firewallSyncedFromTempReadBack();
    void commitIsRenameOfSyncedTemp();
    void failureBeforeRenameLeavesOldConfig();
    // INV-21, INV-63, INV-74
    void runRefusesUntrusted();
    void repairNoticeOnlyAfterRename();
    void repairRenamesAside();
    void configJsonDirectoryUntrusted();
    void pendingDirectoryUntrusted();
    void editRefusedByTheEditorWritesNothing();
    // INV-47
    void stopWaitsForRunningApply();
    // INV-76, INV-109
    void ackRequiresDigestAndPid_data();
    void ackRequiresDigestAndPid();
    void ackWithConfigErrorIsCommittedNotRunning();
    void ackRequiresToken();
    void staleStatusAfterPidReuseRejected();
    // A service this program did not install
    void foreignRefusedEverywhere();
    // INV-91, INV-99
    void saveAndRemoveWritesPendingNotCommitted();
    void deactivateWritesPendingNotCommitted();
    // The window's Save with nothing installed: save only, under apply.lock.
    void saveOnlyWritesPendingAndNothingElse();
    void saveOnlyRefusesAnInstallationFoundUnderTheLock_data();
    void saveOnlyRefusesAnInstallationFoundUnderTheLock();
    // The window's install route: the installation it asked about, under apply.lock.
    void installRefusesAChangedInstallationUnderTheLock_data();
    void installRefusesAChangedInstallationUnderTheLock();
    void rulesRemovedBeforeServiceRemoved();
    void syncedButUncommittedCannotRun();
    // INV-92
    void deadlineForbidsR3();
    void lockHeldThroughSyncAndRename();
    void releasedOnlyImmediatelyBeforeStart();
    void blockStartsBeforeStop();
    void tokenDeletedBeforeAnyChange();
    // INV-98
    void incompleteInstallResumes();
    // INV-102
    void pendingConflictRefused();
    void pendingFromTheSameSourceIsNotAConflict();
    void supersedePendingCommitsAndDeletes();
    // INV-108
    void crashBeforeRenameKeepsOldFiles();
    void crashAfterRenameBeforeSyncNotReportedStaged();
    void crashAfterDurableStagingShowsN();
    void crashAfterCommitNeverRevertsToStalePending();
    void equalPendingDroppedOnReload();
    void unchangedPendingRetryConfirmsDurability();
    void unchangedPendingRunNotStagedBeforeR4();
    void p5KeepsTheEnginesStatusTemps();
    void untickPendingNotDurableWarns();
    void untickNeverSaysOffAfterAFailedStep();
    void discardWarnsWhenTheDeletionIsNotDurable();
    void startConfirmsCommittedDurability();
    void lockPublicationReportedEvenWhenTheRecordFails();
    // INV-113
    void revocationOrder();
    void revocationSurvivesResurrection();
    void revocationFlushFailureIsUncommitted();
    void tokenWrittenOnlyAfterCommit();
    void repairUntrustedFolderTouchesNothingInside();
    // INV-116 (prologue side; the pair-state rows are in tst_LockIdentity)
    void unsupportedFileSystemRefusedBeforeAnyMutation();
    // WS-5 round 2: proof before irreversibility, and no dead ends
    void forceRecreateProvesTheStopBeforeMovingTheFolder_data();
    void forceRecreateProvesTheStopBeforeMovingTheFolder();
    void forceRecreateRefusalAfterTheBarrierIsUncommitted();
    void lateCallStopsBeforeTheNextIrreversibleStep_data();
    void lateCallStopsBeforeTheNextIrreversibleStep();
    void advisedRecoverySucceeds_data();
    void advisedRecoverySucceeds();
    // WS-5 round 3: the three findings, by name (the generated rows are tst_IrreversibleGate's)
    void folderRepairWaitsForTheEngineFreeProof_data();
    void folderRepairWaitsForTheEngineFreeProof();
    void identityReadAfterTheBarrierDeadline_data();
    void identityReadAfterTheBarrierDeadline();
    void recoveryLockWaitsAreJudgedAgainstTheirPhase_data();
    void recoveryLockWaitsAreJudgedAgainstTheirPhase();
    // WS-5 round 4: the review's firewall finding, by name (the mismatched-lock one extends
    // folderRepairWaitsForTheEngineFreeProof)
    void firewallConnectedBeforeEveryFirewallStep_data();
    void firewallConnectedBeforeEveryFirewallStep();
    void cleanupWithoutALockNeverCallsTheStopVerified();
    // WS-5 round 6: the review's two findings, by name (the generated rows are tst_IrreversibleGate's)
    void earlyBarrierCallReturnsLate_data();
    void earlyBarrierCallReturnsLate();
    void lockRefusedNeverClaimsAStop_data();
    void lockRefusedNeverClaimsAStop();
};

// ---------------------------------------------------------------------------------------------------
void tst_Applier::saveAndApplyRunsEveryStepInOrderUnderTheLock()
{
    Rig r;
    r.installedRunning(configA());
    const Result res = applier(r).run(request(configB()), false);
    QCOMPARE(res.outcome, Outcome::Ok);
    QCOMPARE(res.persisted, Persisted::Committed);
    QCOMPARE(res.persistedSha256, digest(configB()));
    QCOMPARE(r.file(kConfig), configB());
    QVERIFY(!r.has(kPending));
    QVERIFY(r.service.engineRunning);
    QVERIFY(!r.service.startsBlocked);
    QCOMPARE(r.service.status->configSha256.toLatin1(), digest(configB()));
    QVERIFY(r.world.violations.isEmpty());
    QVERIFY(r.world.files.tempNames().isEmpty());
}

// INV-25 (R1-R10). Kills: any reordering of the run (e.g. enable before the firewall).
void tst_Applier::orderUnderLock()
{
    Rig r;
    r.installedRunning(configA());
    QCOMPARE(applier(r).run(request(configB()), false).outcome, Outcome::Ok);
    const QStringList order = {
        QStringLiteral("applyLock:acquired"),        QStringLiteral("service:detect"),
        QStringLiteral("folder:inspect"),            QStringLiteral("io:openExisting start.token"),   // B0
        QStringLiteral("service:inhibitStarts"),     QStringLiteral("service:requestStop"),           // B1, B2
        QStringLiteral("engineLock:acquired"),       QStringLiteral("io:commitAs config.pending.json"),   // B3, R4
        QStringLiteral("io:readBack config.json."),  QStringLiteral("firewall:connect"),              // R5, R6
        QStringLiteral("firewall:sync"),             QStringLiteral("io:commitAs config.json"),       // R6, R7
        QStringLiteral("io:remove config.pending.json"), QStringLiteral("service:reconcileOutstandingJobs"),   // R8
        QStringLiteral("firewall:sweep"),            QStringLiteral("io:commitAs start.token"),       // R9
        QStringLiteral("engineLock:released"),       QStringLiteral("service:allowStarts"),
        QStringLiteral("service:start"),             QStringLiteral("service:query"),                 // R10
        QStringLiteral("applyLock:released"),
    };
    int i = 0;
    for (const QString& e : order) {
        const int next = r.world.log.indexOf(e, i);
        QVERIFY2(next >= 0, qPrintable(QStringLiteral("%1 missing or out of order in\n%2").arg(e, events(r))));
        i = next + 1;
    }
}

// INV-49. Kills: a first install without an empty committed config.
void tst_Applier::firstInstallWritesEmptyConfig()
{
    Rig r;
    r.folder.state = FakeFolder::DirState::Missing;
    const Result res = applier(r).run(std::nullopt, true);
    QVERIFY2(res.outcome == Outcome::Ok, qPrintable(res.message));
    QVERIFY(r.has(kConfig));
    const auto c = fromJson(r.file(kConfig), nullptr);
    QVERIFY(c);
    QCOMPARE(*c, BridgeConfig{});
    QVERIFY(r.has(kToken));
    QCOMPARE(r.service.installation.state, InstallState::Current);
    QCOMPARE(LockIdentity::classify(LockFileRead{LockFileRead::Status::Ok, r.engineLock.lockContent(), {}},
                                    LockFileRead{LockFileRead::Status::Ok, r.engineLock.recordContent(), {}}),
             LockIdentity::PairState::Matching);
    // The token is written only after the first successful commit (INV-113).
    QVERIFY(at(r, QStringLiteral("io:commitAs config.json")) < at(r, QStringLiteral("io:commitAs start.token")));
}

// INV-43. Kills: --apply ignoring the pending file, or committing without deleting it.
void tst_Applier::applyPromotesPending()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(kPending, configB(), QStringLiteral("root"));
    const Result res = applier(r).run(std::nullopt, false);
    QCOMPARE(res.outcome, Outcome::Ok);
    QCOMPARE(res.persisted, Persisted::Committed);
    QCOMPARE(res.persistedSha256, digest(configB()));
    QCOMPARE(r.file(kConfig), configB());
    QVERIFY(!r.has(kPending));
}

// INV-29. Kills: a Start that skips the firewall sync (or syncs from anything but the committed bytes).
void tst_Applier::startResyncsFirewall()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(kPending, configB(), QStringLiteral("root"));   // never what Start runs
    const Result res = applier(r).start();
    QCOMPARE(res.outcome, Outcome::Ok);
    // Its own words: nothing new was saved (WS-9, station run); Save & apply keeps "Saved and applied".
    QVERIFY2(res.message.startsWith(QLatin1String("Started: the bridge is running the saved settings.")), qPrintable(res.message));
    QVERIFY(r.firewall.syncedFrom);
    QCOMPARE(*r.firewall.syncedFrom, *fromJson(configA(), nullptr));
    QVERIFY(at(r, QStringLiteral("firewall:sync")) < at(r, QStringLiteral("service:start")));
    QVERIFY(at(r, QStringLiteral("service:inhibitStarts")) < at(r, QStringLiteral("firewall:sync")));
    QCOMPARE(r.file(kConfig), configA());
    QCOMPARE(r.file(kPending), configB());
}

void tst_Applier::stopRevokesStopsAndKeepsTheStartType()
{
    Rig r;
    r.installedRunning(configA());
    const Result res = applier(r).stop();
    QCOMPARE(res.outcome, Outcome::Ok);
    QVERIFY(res.postcondition.contains(QString::fromLatin1(kVerified)));
    QVERIFY(!r.service.engineRunning);
    QVERIFY(!r.service.startsBlocked);   // parity: the start type is kept ...
    QVERIFY(!r.has(kToken));             // ... and the token is gone, so nothing restarts it
    QVERIFY(!r.world.log.contains(QStringLiteral("service:inhibitStarts")));
    QVERIFY(!r.world.log.contains(QStringLiteral("firewall:")));
    r.service.launchEngine();            // a recovery restart finds no token
    QVERIFY(r.world.log.contains(QStringLiteral("engine:exit 6")));
}

// ---------------------------------------------------------------------------------------------------
void tst_Applier::faultMatrix_data()
{
    addRows();
}

void tst_Applier::faultMatrix()
{
    QFETCH(int, row);
    const Row t = rows().at(row);
    Rig r;
    r.installedRunning(configA());
    t.setup(r);
    const Result res = runRow(r, t);
    QVERIFY2(res.outcome == t.outcome, qPrintable(QStringLiteral("outcome %1: %2\n%3").arg(enumName(res.outcome), res.message, events(r))));
    QCOMPARE(enumName(res.persisted), enumName(t.persisted));
    if (!t.committedAfter.isNull()) QCOMPARE(r.file(kConfig), t.committedAfter);
    if (*t.postcondition)
        QVERIFY2(res.postcondition.contains(QString::fromLatin1(t.postcondition)),
                 qPrintable(QStringLiteral("postcondition: %1\n%2").arg(res.postcondition, events(r))));
    if (t.extra) t.extra(r, res);
    QVERIFY2(r.world.violations.isEmpty(), qPrintable(r.world.violations.join(QLatin1Char(','))));
    QVERIFY(!r.world.applyLock.held());
    QVERIFY(!r.engineLock.appliersHold());
}

// INV-76: the outcome is decided by the step (and the ACK), every row.
void tst_Applier::outcomeByStep_data()
{
    addRows();
}

void tst_Applier::outcomeByStep()
{
    QFETCH(int, row);
    const Row t = rows().at(row);
    Rig r;
    r.installedRunning(configA());
    t.setup(r);
    const Result res = runRow(r, t);
    QCOMPARE(enumName(res.outcome), enumName(t.outcome));
    // NothingChanged never claims a change; everything else names its step or its confirmation.
    // (Repair reports its folder and its service separately: "The data folder was not repaired ... Service: ...")
    if (res.outcome == Outcome::NothingChanged)
        QVERIFY(res.message.contains(QStringLiteral("were not changed")) || res.message.contains(QStringLiteral("not repaired")));
    if (res.outcome == Outcome::Uncommitted) QVERIFY(!res.step.isEmpty() && res.message.contains(QStringLiteral("Service:")));
    if (res.outcome == Outcome::CommittedNotRunning) QVERIFY(res.message.contains(QStringLiteral("did not start")));
}

// INV-106. Kills: reporting `persisted` or its digest for anything but what is durably on disk
// (e.g. Committed for a pre-commit StateUnknown).
void tst_Applier::persistedByStep_data()
{
    addRows();
}

void tst_Applier::persistedByStep()
{
    QFETCH(int, row);
    const Row t = rows().at(row);
    Rig r;
    r.installedRunning(configA());
    t.setup(r);
    const Result res = runRow(r, t);
    QCOMPARE(enumName(res.persisted), enumName(t.persisted));
    if (res.persisted == Persisted::None) {
        QVERIFY(res.persistedSha256.isEmpty());
        return;
    }
    // The digest is that of the submitted bytes, and those bytes really are on disk as claimed.
    const QByteArray submitted = t.op == Op::Edit ? r.file(kPending) : (t.op == Op::Start ? configA() : configB());
    QCOMPARE(res.persistedSha256, digest(submitted));
    if (res.persisted == Persisted::Staged) QCOMPARE(digest(r.file(kPending)), res.persistedSha256);
    if (res.persisted == Persisted::Committed) QCOMPARE(digest(r.file(kConfig)), res.persistedSha256);
}

// ---------------------------------------------------------------------------------------------------
// INV-24. Kills: any seam call (service, store, trust, firewall, engine lock) outside apply.lock, for
// all eight operations.
void tst_Applier::lockHeldAcrossEveryStep_data()
{
    QTest::addColumn<int>("op");
    const char* names[] = {"Run", "Start", "Stop", "SaveAndRemove", "Deactivate", "Edit", "DiscardPending", "Repair"};
    for (int i = 0; i < 8; ++i) QTest::newRow(names[i]) << i;
}

void tst_Applier::lockHeldAcrossEveryStep()
{
    QFETCH(int, op);
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(kPending, configB(), QStringLiteral("root"));
    Applier a = applier(r);
    Result res;
    switch (op) {
    case 0: res = a.run(std::nullopt, false); break;
    case 1: res = a.start(); break;
    case 2: res = a.stop(); break;
    case 3: res = a.saveAndRemove(request(configC(), Source::Pending, configB())); break;
    case 4: res = a.deactivate(request(configC(), Source::Pending, configB())); break;
    case 5: res = a.edit([](BridgeConfig& c) { c.ports[0].baudRate = 9600; }); break;
    case 6: res = a.discardPending(); break;
    case 7: res = a.repair(); break;
    }
    QVERIFY2(res.outcome != Outcome::NothingChanged, qPrintable(res.message));
    QVERIFY2(r.world.violations.isEmpty(), qPrintable(r.world.violations.join(QLatin1Char('\n'))));
    const QStringList all = r.world.log.all();
    QCOMPARE(all.first(), QStringLiteral("applyLock:acquired"));
    QCOMPARE(r.world.log.count(QStringLiteral("applyLock:acquired")), 1);
    // Nothing after the release but the engine's own activity.
    const int released = r.world.log.indexOf(QStringLiteral("applyLock:released"));
    for (int i = released + 1; i < all.size(); ++i) QVERIFY2(all[i].startsWith(QStringLiteral("engine:")), qPrintable(all[i]));
}

// INV-24. Kills: SaveAndRemove writing outside the trusted folder's handle (or in an untrusted folder).
void tst_Applier::saveAndRemoveUsesTrustedHandles()
{
    {
        Rig r;
        r.installedRunning(configA());
        QCOMPARE(applier(r).saveAndRemove(request(configB())).outcome, Outcome::SavedNotApplied);
        QVERIFY(!r.io.dirIds.isEmpty());
        for (const FileId& id : r.io.dirIds) QCOMPARE(id, r.folder.handle().id());
    }
    {
        Rig r;
        r.installedRunning(configA());
        r.folder.owner = QStringLiteral("attacker");
        const Result res = applier(r).saveAndRemove(request(configB()));
        QCOMPARE(res.outcome, Outcome::NothingChanged);
        QVERIFY(r.io.dirIds.isEmpty());
        QVERIFY(!r.has(kPending));
        QVERIFY(r.service.engineRunning);
    }
}

// INV-24. Kills: DiscardPending deleting without the trusted folder (no Mutating inspection).
void tst_Applier::discardPendingUsesTrustedHandles()
{
    {
        Rig r;
        r.installedRunning(configA());
        r.world.files.plantFile(kPending, configB(), QStringLiteral("root"));
        QCOMPARE(applier(r).discardPending().outcome, Outcome::Ok);
        QVERIFY(!r.has(kPending));
        QVERIFY(at(r, QStringLiteral("folder:inspect")) < at(r, QStringLiteral("io:remove config.pending.json")));
        for (const FileId& id : r.io.dirIds) QCOMPARE(id, r.folder.handle().id());
    }
    {
        Rig r;
        r.installedRunning(configA());
        r.world.files.plantFile(kPending, configB(), QStringLiteral("root"));
        r.world.files.plantFile(QStringLiteral("planted.txt"), "x");   // untrusted child
        QCOMPARE(applier(r).discardPending().outcome, Outcome::NothingChanged);
        QCOMPARE(r.file(kPending), configB());
        QVERIFY(r.io.dirIds.isEmpty());
    }
}

// INV-127: the digest the caller showed is the file's: deleted by handle in the trusted folder, durably.
void tst_Applier::discardPendingIfMatches()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(kPending, configB(), QStringLiteral("root"));
    const Result res = applier(r).discardPendingIf(QString::fromLatin1(digest(configB())));
    QCOMPARE(res.outcome, Outcome::Ok);
    QVERIFY(!r.has(kPending));
    QVERIFY(at(r, QStringLiteral("folder:inspect")) < at(r, QStringLiteral("io:remove config.pending.json")));
    QVERIFY(at(r, QStringLiteral("io:remove config.pending.json")) < at(r, QStringLiteral("io:syncDir")));
    QCOMPARE(r.file(kConfig), configA());
}

// INV-127: the file was replaced since the caller read it: nothing is deleted, and the result says why. Kills
// inv127-discard-ignores-digest.
void tst_Applier::discardPendingIfReplaced()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(kPending, configC(), QStringLiteral("root"));
    const Result res = applier(r).discardPendingIf(QString::fromLatin1(digest(configB())));
    QCOMPARE(res.outcome, Outcome::NothingChanged);
    QCOMPARE(r.file(kPending), configC());
    QVERIFY(!r.world.log.contains(QStringLiteral("io:remove config.pending.json")));
    QVERIFY2(res.message.contains(QStringLiteral("were replaced since this window read them; nothing was discarded")),
             qPrintable(res.message));
    QVERIFY(res.message.contains(QStringLiteral("Reload to see them")));
}

// INV-127: no pending file: NothingChanged with its own wording, nothing removed.
void tst_Applier::discardPendingIfMissing()
{
    Rig r;
    r.installedRunning(configA());
    const Result res = applier(r).discardPendingIf(QString::fromLatin1(digest(configB())));
    QCOMPARE(res.outcome, Outcome::NothingChanged);
    QVERIFY2(res.message.startsWith(QStringLiteral("There are no saved changes to discard.")), qPrintable(res.message));
    QVERIFY(!r.world.log.contains(QStringLiteral("io:remove")));
}

// ---------------------------------------------------------------------------------------------------
// INV-25. Kills: syncing the firewall from the in-memory N instead of the temp's read-back bytes.
void tst_Applier::firewallSyncedFromTempReadBack()
{
    Rig r;
    r.installedRunning(configA());
    r.io.tamperReadBack = [](const QByteArray&) { return configC(); };   // what the file "really" holds
    applier(r).run(request(configB()), false);
    QVERIFY(r.firewall.syncedFrom);
    QCOMPARE(r.firewall.syncedFrom->ports.value(0).comPort, QStringLiteral("COM5"));
}

// INV-73. Kills: writing config.json directly (or renaming any temp but the one that was read back
// and synced), or renaming before the sync.
void tst_Applier::commitIsRenameOfSyncedTemp()
{
    Rig r;
    r.installedRunning(configA());
    QCOMPARE(applier(r).run(request(configB()), false).outcome, Outcome::Ok);
    const SpyFileIo::TempRecord* committed = nullptr;
    int commits = 0;
    for (const auto& t : r.io.temps) {
        if (t.committedAs == kConfig) {
            committed = &t;
            ++commits;
        }
    }
    QCOMPARE(commits, 1);
    QVERIFY(committed->readBack);
    QCOMPARE(*r.firewall.syncedFrom, *fromJson(committed->readBytes, nullptr));
    QVERIFY(at(r, QStringLiteral("firewall:sync")) < at(r, QStringLiteral("io:commitAs config.json")));
    QCOMPARE(r.world.log.count(QStringLiteral("io:commitAs config.json")), 1);
}

void tst_Applier::failureBeforeRenameLeavesOldConfig()
{
    for (const char* m : {"connect", "sync"}) {
        Rig r;
        r.installedRunning(configA());
        r.firewall.fail[QString::fromLatin1(m)] = QStringLiteral("failed");
        QCOMPARE(applier(r).run(request(configB()), false).outcome, Outcome::Uncommitted);
        QCOMPARE(r.file(kConfig), configA());
        QVERIFY(r.world.files.tempNames().isEmpty());
    }
}

// ---------------------------------------------------------------------------------------------------
// INV-21. Kills: Run recreating (renaming aside) an untrusted folder.
void tst_Applier::runRefusesUntrusted()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(QStringLiteral("planted.txt"), "x");
    const Result res = applier(r).run(request(configB()), true);
    QCOMPARE(res.outcome, Outcome::NothingChanged);
    QVERIFY(res.message.contains(QStringLiteral("Repair")));
    QCOMPARE(r.folder.renames, 0);
    QCOMPARE(r.folder.creates, 0);
    QCOMPARE(r.file(kConfig), configA());
    // --force-recreate (repairFirst) is the explicit way, under the same lock.
    const Result forced = applier(r).run(request(configB()), true, true);
    QVERIFY2(forced.outcome == Outcome::Ok, qPrintable(forced.message));
    QCOMPARE(r.folder.renames, 1);
}

// INV-21. Kills: the "moved aside" notice before the rename succeeded.
void tst_Applier::repairNoticeOnlyAfterRename()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(QStringLiteral("planted.txt"), "x");
    r.folder.failRenameAside = QStringLiteral("sharing violation");
    const Result failed = applier(r).repair();
    QVERIFY(!failed.fileChanges.join(QLatin1Char(' ')).contains(QStringLiteral("moved")));
    QVERIFY(!failed.message.contains(QStringLiteral("moved the untrusted folder")));
    r.folder.failRenameAside.reset();
    const Result ok = applier(r).repair();
    QCOMPARE(ok.outcome, Outcome::Ok);
    QVERIFY(ok.fileChanges.join(QLatin1Char(' ')).contains(r.folder.asidePath));
}

// INV-74 (pure). Kills: Repair deleting anything of the untrusted folder.
void tst_Applier::repairRenamesAside()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(QStringLiteral("planted.txt"), "x");
    const Result res = applier(r).repair();
    QCOMPARE(res.outcome, Outcome::Ok);
    QCOMPARE(r.folder.aside.value(kConfig), configA());
    QVERIFY(r.folder.aside.contains(QStringLiteral("planted.txt")));
    QVERIFY(r.folder.aside.contains(kToken));
    QVERIFY(!r.world.log.contains(QStringLiteral("io:remove")));
    QVERIFY(!r.world.log.contains(QStringLiteral("io:removeByHandle")));
    QVERIFY(r.service.startsBlocked);   // left disabled
    QVERIFY(res.message.contains(QStringLiteral("Folder:")) && res.message.contains(QStringLiteral("Service:")));
}

// INV-63. Kills: accepting a directory at config.json / config.pending.json.
void tst_Applier::configJsonDirectoryUntrusted()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.eraseEntry(kConfig);
    r.world.files.plantDirectory(kConfig);
    const Result res = applier(r).run(request(configB()), false);
    QCOMPARE(res.outcome, Outcome::NothingChanged);
    QVERIFY2(res.message.contains(QStringLiteral("config.json is not a regular file")), qPrintable(res.message));
    QVERIFY(!r.world.log.contains(QStringLiteral("service:inhibitStarts")));
}

void tst_Applier::pendingDirectoryUntrusted()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantDirectory(kPending);
    const Result res = applier(r).edit([](BridgeConfig&) {});
    QCOMPARE(res.outcome, Outcome::NothingChanged);
    QVERIFY(res.message.contains(QStringLiteral("config.pending.json is not a regular file")));
}

// WS-9: an edit the editor refuses (ConfigEditor throws: no such port, a duplicate name) is NothingChanged with
// the editor's reason; config.pending.json is not written and the service is not touched. Kills: letting the
// exception escape the operation worker, or writing the unchanged config as pending.
void tst_Applier::editRefusedByTheEditorWritesNothing()
{
    Rig r;
    r.installedRunning(configA());
    r.world.log.clear();
    const Result res = applier(r).edit([](BridgeConfig&) { throw std::runtime_error("No port named 'X'. See --show-config"); });
    QCOMPARE(res.outcome, Outcome::NothingChanged);
    QCOMPARE(res.persisted, Persisted::None);
    QVERIFY2(res.message.contains(QStringLiteral("No port named 'X'")), qPrintable(res.message));
    QVERIFY(!r.has(kPending));
    for (const char* call : {"service:inhibitStarts", "service:requestStop", "service:start", "io:createTemp"})
        QVERIFY2(!r.world.log.contains(QString::fromLatin1(call)), call);
    QVERIFY(!r.world.applyLock.held());
}

// ---------------------------------------------------------------------------------------------------
// INV-47. Kills: Stop not taking apply.lock (it would run while an apply is mid-firewall).
void tst_Applier::stopWaitsForRunningApply()
{
    Rig r;
    r.installedRunning(configA());
    std::promise<void> inSync, release;
    std::shared_future<void> released = release.get_future().share();
    r.firewall.onCall = [&](const QString& m) {
        if (m == QLatin1String("sync")) {
            inSync.set_value();
            released.wait();
        }
    };
    std::thread applyThread([&] { applier(r).run(request(configB()), false); });
    inSync.get_future().wait();
    const int stopsBefore = r.world.log.count(QStringLiteral("service:requestStop"));
    std::thread stopThread([&] { applier(r).stop(); });
    for (int i = 0; i < 2000 && r.world.applyLock.waiting.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const int waiting = r.world.applyLock.waiting.load();
    const int stopsDuring = r.world.log.count(QStringLiteral("service:requestStop"));
    release.set_value();
    applyThread.join();
    stopThread.join();
    QCOMPARE(waiting, 1);                 // Stop was blocked on the lock ...
    QCOMPARE(stopsDuring, stopsBefore);   // ... and had done nothing
    // Stop ran entirely after the apply released the lock.
    const int firstRelease = at(r, QStringLiteral("applyLock:released"));
    QVERIFY(r.world.log.indexOf(QStringLiteral("service:requestStop"), firstRelease) > firstRelease);
}

// ---------------------------------------------------------------------------------------------------
// INV-76. Kills: treating SCM "Running" (or any status) as success without the matching pid and digest.
void tst_Applier::ackRequiresDigestAndPid_data()
{
    QTest::addColumn<int>("mode");
    QTest::newRow("wrong pid") << int(AckMode::WrongPid);
    QTest::newRow("wrong digest") << int(AckMode::WrongDigest);
    QTest::newRow("no status") << int(AckMode::Never);
}

void tst_Applier::ackRequiresDigestAndPid()
{
    QFETCH(int, mode);
    Rig r;
    r.installedRunning(configA());
    r.service.ack = AckMode(mode);
    const Result res = applier(r).run(request(configB()), false);
    QCOMPARE(res.outcome, Outcome::StateUnknown);
    QCOMPARE(res.persisted, Persisted::Committed);
    QVERIFY(r.service.engineRunning);   // SCM says running; that is not an ACK
}

void tst_Applier::ackWithConfigErrorIsCommittedNotRunning()
{
    Rig r;
    r.installedRunning(configA());
    r.service.ack = AckMode::ConfigError;
    const Result res = applier(r).run(request(configB()), false);
    QCOMPARE(res.outcome, Outcome::CommittedNotRunning);
    QVERIFY(res.message.contains(QStringLiteral("nope")));
    QVERIFY(res.postcondition.contains(QString::fromLatin1(kVerified)));
}

// INV-109. Kills: accepting a status without the token written in R9.
void tst_Applier::ackRequiresToken()
{
    Rig r;
    r.installedRunning(configA());
    r.service.ack = AckMode::StaleToken;
    r.service.previousToken = QByteArrayLiteral("ffffffffffffffffffffffffffffffff");
    QCOMPARE(applier(r).run(request(configB()), false).outcome, Outcome::StateUnknown);
}

// INV-109. Kills: ignoring startToken when the pid was reused and the digest happens to match.
void tst_Applier::staleStatusAfterPidReuseRejected()
{
    Rig r;
    r.installedRunning(configB());   // the old engine already ran B: same digest
    r.service.ack = AckMode::StaleToken;
    const Result res = applier(r).run(request(configB(), Source::Committed, configB()), false);
    QCOMPARE(res.outcome, Outcome::StateUnknown);
    QCOMPARE(r.service.status->configSha256.toLatin1(), digest(configB()));
    QCOMPARE(r.service.status->pid, r.service.pid);
}

// ---------------------------------------------------------------------------------------------------
// Kills: any barrier, file or firewall step before refusing a service this program did not install, in any operation.
void tst_Applier::foreignRefusedEverywhere()
{
    for (int op = 0; op < 8; ++op) {
        Rig r;
        r.installedRunning(configA());
        r.service.installation.state = InstallState::Foreign;
        r.service.installation.detail = QStringLiteral(R"(the service runs "C:\Tools\Other.exe" --service)");
        Applier a = applier(r);
        Result res;
        switch (op) {
        case 0: res = a.run(request(configB()), true); break;
        case 1: res = a.start(); break;
        case 2: res = a.stop(); break;
        case 3: res = a.saveAndRemove(request(configB())); break;
        case 4: res = a.deactivate(request(configB())); break;
        case 5: res = a.edit([](BridgeConfig&) {}); break;
        case 6: res = a.discardPending(); break;
        case 7: res = a.repair(); break;
        }
        QCOMPARE(res.outcome, Outcome::NothingChanged);
        QCOMPARE(res.persisted, Persisted::None);
        QVERIFY2(res.message.contains(QStringLiteral("not one this program installed")), qPrintable(res.message));
        QVERIFY2(res.message.contains(QStringLiteral("Other.exe")), qPrintable(res.message));
        QCOMPARE(r.world.log.all(), (QStringList{QStringLiteral("applyLock:acquired"), QStringLiteral("service:detect"),
                                                 QStringLiteral("applyLock:released")}));
        QCOMPARE(r.file(kConfig), configA());
    }
}

// ---------------------------------------------------------------------------------------------------
// INV-91. Kills: SaveAndRemove committing N.
void tst_Applier::saveAndRemoveWritesPendingNotCommitted()
{
    Rig r;
    r.installedRunning(configA());
    const Result res = applier(r).saveAndRemove(request(configB()));
    QCOMPARE(res.outcome, Outcome::SavedNotApplied);
    QCOMPARE(res.persisted, Persisted::Staged);
    QCOMPARE(res.persistedSha256, digest(configB()));
    QCOMPARE(r.file(kConfig), configA());
    QCOMPARE(r.file(kPending), configB());
    QVERIFY(!r.has(kToken));
    QCOMPARE(r.service.installation.state, InstallState::NotInstalled);
    // The pending file is written FIRST, before any service change.
    QVERIFY(at(r, QStringLiteral("io:commitAs config.pending.json")) < at(r, QStringLiteral("service:inhibitStarts")));
}

// INV-91. Kills: Deactivate committing N (unsynced).
void tst_Applier::deactivateWritesPendingNotCommitted()
{
    Rig r;
    r.installedRunning(configA());
    const Result res = applier(r).deactivate(request(configB()));
    QCOMPARE(res.outcome, Outcome::SavedNotApplied);
    QCOMPARE(r.file(kConfig), configA());
    QCOMPARE(r.file(kPending), configB());
    QVERIFY(r.service.startsBlocked);
    QVERIFY(!r.service.engineRunning);
    QVERIFY(!r.has(kToken));
    QVERIFY(!r.world.log.contains(QStringLiteral("firewall:")));
    QCOMPARE(r.service.installation.state, InstallState::Current);   // the unit stays (masked)
}

// The window's Save with nothing installed (v1.1 fix after WS-17): the pending file only. No barrier, no service call and
// no firewall call (save-and-remove would still connect to the firewall); a program kept by an uninstall is still
// "nothing installed".
void tst_Applier::saveOnlyWritesPendingAndNothingElse()
{
    for (const bool programKept : {false, true}) {
        Rig r;
        r.service.programPresent = programKept;
        const Result res = applier(r).saveOnly(request(configB()));
        QVERIFY2(res.outcome == Outcome::SavedNotApplied, qPrintable(res.message));
        QCOMPARE(res.persisted, Persisted::Staged);
        QCOMPARE(r.file(kPending), configB());
        QVERIFY(!r.has(kConfig));
        QCOMPARE(r.service.installation.state, InstallState::NotInstalled);
        for (const QString& e : r.world.log.all())
            QVERIFY2((!e.startsWith(QLatin1String("service:")) || e.startsWith(QLatin1String("service:detect"))) &&
                         !e.startsWith(QLatin1String("firewall:")),
                     qPrintable(e));
    }
}

// The window chose Save on its read of the installation, before its questions (the replacement
// question can stay open for minutes); a service installed in between (another window, the CLI) is found by the
// transaction's own detect under apply.lock. Save only must not become Deactivate on it, which would write the pending
// file and then stop the new service and block its starts without asking: refused, nothing written, no barrier, no
// service or firewall call. Kills: the precondition dropped (Save only is Deactivate again).
void tst_Applier::saveOnlyRefusesAnInstallationFoundUnderTheLock_data()
{
    QTest::addColumn<bool>("incomplete");
    QTest::newRow("installed and running") << false;
    QTest::newRow("an unfinished installation") << true;
}

void tst_Applier::saveOnlyRefusesAnInstallationFoundUnderTheLock()
{
    QFETCH(bool, incomplete);
    Rig r;
    // The window read "nothing installed" here; meanwhile, elsewhere:
    r.installedRunning(configA());
    if (incomplete) {
        r.service.installation.state = InstallState::IncompleteInstall;
        r.service.installation.detail = QStringLiteral("a lock file is missing");
    }
    r.world.log.clear();
    const Result res = applier(r).saveOnly(request(configB()));
    QVERIFY2(res.outcome == Outcome::NothingChanged, qPrintable(res.message));
    QCOMPARE(res.persisted, Persisted::None);
    QVERIFY2(res.message.startsWith(QStringLiteral("Nothing was saved: the installation changed")), qPrintable(res.message));
    QVERIFY2(res.message.contains(incomplete ? QStringLiteral("partly installed: a lock file is missing")
                                             : QStringLiteral("the bridge service is now installed")),
             qPrintable(res.message));
    QVERIFY(!r.has(kPending));
    QCOMPARE(r.file(kConfig), configA());
    QVERIFY(r.service.engineRunning);
    QVERIFY(!r.service.startsBlocked);
    QVERIFY(r.has(kToken));
    QCOMPARE(r.service.installation.state, incomplete ? InstallState::IncompleteInstall : InstallState::Current);
    for (const QString& e : r.world.log.all())
        QVERIFY2((!e.startsWith(QLatin1String("service:")) || e.startsWith(QLatin1String("service:detect"))) &&
                     !e.startsWith(QLatin1String("firewall:")) && !e.startsWith(QLatin1String("io:")),
                 qPrintable(e));
}

// The window decides its install route (and asks its questions) on its own read; another build
// installed while the install question is open must not be installed over without the build question (D67). The
// request carries the installation the window read; under apply.lock a different one is refused: nothing written,
// nothing replaced, no service or firewall call. Two controls: a matching basis installs, and no basis (the CLI's
// --install) still installs over another build as before. Kills: the precondition dropped.
namespace {
void makeOtherBuild(Rig& r, const QString& sha)
{
    r.installedRunning(configA());
    r.service.installation.state = InstallState::IncompleteInstall;
    r.service.installation.otherBuild = true;
    r.service.installation.installedSha256 = sha;
    r.service.installation.detail = QStringLiteral("the installed program is a different build");
}
const QString kShaX = QStringLiteral("117eb29a0b1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f708192a3b4c5d6e7f8091a");
const QString kShaY = QStringLiteral("2222b29a0b1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f708192a3b4c5d6e7f8091b");
}  // namespace

void tst_Applier::installRefusesAChangedInstallationUnderTheLock_data()
{
    QTest::addColumn<QString>("basis");   // what the window read: none (no basis), nothing, otherX
    QTest::addColumn<QString>("found");   // what the transaction finds: nothing, current, otherX, otherY
    QTest::addColumn<bool>("refused");
    QTest::newRow("read nothing, found another build") << "nothing" << "otherX" << true;
    QTest::newRow("read nothing, found installed") << "nothing" << "current" << true;
    QTest::newRow("read another build, found a different one") << "otherX" << "otherY" << true;
    QTest::newRow("read another build, found installed") << "otherX" << "current" << true;
    QTest::newRow("control: read nothing, found nothing") << "nothing" << "nothing" << false;
    QTest::newRow("control: read another build, found it") << "otherX" << "otherX" << false;
    QTest::newRow("control: no basis (CLI --install) over another build") << "none" << "otherX" << false;
}

void tst_Applier::installRefusesAChangedInstallationUnderTheLock()
{
    QFETCH(QString, basis);
    QFETCH(QString, found);
    QFETCH(bool, refused);
    Rig r;
    if (found == QLatin1String("current")) r.installedRunning(configA());
    else if (found == QLatin1String("otherX")) makeOtherBuild(r, kShaX);
    else if (found == QLatin1String("otherY")) makeOtherBuild(r, kShaY);
    SaveRequest req = request(configB());
    if (basis == QLatin1String("nothing"))
        req.expectInstallation = SaveRequest::InstallationBasis{true, InstallState::NotInstalled, false, QString()};
    else if (basis == QLatin1String("otherX"))
        req.expectInstallation = SaveRequest::InstallationBasis{true, InstallState::IncompleteInstall, true, kShaX};
    const InstallState stateBefore = r.service.installation.state;
    const QString shaBefore = r.service.installation.installedSha256;
    const QByteArray configBefore = r.has(kConfig) ? r.file(kConfig) : QByteArray();
    r.world.log.clear();
    const Result res = applier(r).run(req, /*install=*/true);
    if (!refused) {
        QVERIFY2(res.outcome == Outcome::Ok, qPrintable(res.message));
        QCOMPARE(r.file(kConfig), configB());
        QCOMPARE(r.service.installation.state, InstallState::Current);
        QVERIFY(!r.service.installation.otherBuild);
        return;
    }
    QVERIFY2(res.outcome == Outcome::NothingChanged, qPrintable(res.message));
    QCOMPARE(res.persisted, Persisted::None);
    QVERIFY2(res.message.startsWith(QStringLiteral("Nothing was saved: the installation changed")), qPrintable(res.message));
    QVERIFY(!r.has(kPending));
    QCOMPARE(r.has(kConfig) ? r.file(kConfig) : QByteArray(), configBefore);
    QCOMPARE(r.service.installation.state, stateBefore);
    QCOMPARE(r.service.installation.installedSha256, shaBefore);
    if (found != QLatin1String("nothing")) {
        QVERIFY(r.service.engineRunning);
        QVERIFY(!r.service.startsBlocked);
    }
    for (const QString& e : r.world.log.all())
        QVERIFY2((!e.startsWith(QLatin1String("service:")) || e.startsWith(QLatin1String("service:detect"))) &&
                     !e.startsWith(QLatin1String("firewall:")) && !e.startsWith(QLatin1String("io:")),
                 qPrintable(e));
}

// INV-91. Kills: removing the service before its firewall rules.
void tst_Applier::rulesRemovedBeforeServiceRemoved()
{
    Rig r;
    r.installedRunning(configA());
    applier(r).saveAndRemove(std::nullopt);
    QVERIFY(at(r, QStringLiteral("firewall:removeAll")) < at(r, QStringLiteral("service:uninstall")));
    QVERIFY(r.firewall.rules.isEmpty());
}

// INV-99 (pure half; the native activation_matrix is WS-13's): sync succeeded, rename failed — the new
// rules exist with the old config; nothing can run until the next apply re-syncs.
void tst_Applier::syncedButUncommittedCannotRun()
{
    Rig r;
    r.installedRunning(configA());
    r.firewall.onCall = [&r](const QString& m) {
        if (m == QLatin1String("sync")) r.world.files.failNext(FakeFileIo::Op::Rename);
    };
    QCOMPARE(applier(r).run(request(configB()), false).outcome, Outcome::Uncommitted);
    QCOMPARE(r.file(kConfig), configA());
    QCOMPARE(r.file(kPending), configB());
    QVERIFY(r.service.startsBlocked);
    QVERIFY(!r.has(kToken));
    r.service.startsBlocked = false;   // even if something re-enabled it, the engine needs the token
    r.service.launchEngine();
    QVERIFY(r.world.log.contains(QStringLiteral("engine:exit 6")));
    QVERIFY(!r.service.engineRunning);
}

// ---------------------------------------------------------------------------------------------------
// INV-92. Kills: continuing to R3 (install, staging, firewall, rename) after the barrier deadline.
void tst_Applier::deadlineForbidsR3()
{
    for (bool install : {false, true}) {
        Rig r;
        r.installedRunning(configA());
        r.service.neverReleases = true;
        const Result res = applier(r).run(request(configB()), install);
        QCOMPARE(res.outcome, Outcome::StateUnknown);
        QCOMPARE(res.persisted, Persisted::None);
        QVERIFY(res.message.contains(QStringLiteral("30 s")));
        for (const char* e : {"service:installOrConfigure", "io:createTemp", "firewall:", "io:commitAs", "lockio:"})
            QVERIFY2(!r.world.log.contains(QString::fromLatin1(e)), e);
        QCOMPARE(r.file(kConfig), configA());
    }
}

// INV-92. Kills: releasing engine.lock before the firewall sync or the rename.
void tst_Applier::lockHeldThroughSyncAndRename()
{
    Rig r;
    r.installedRunning(configA());
    bool heldAtSync = false;
    r.firewall.onCall = [&](const QString& m) {
        if (m == QLatin1String("sync")) heldAtSync = r.engineLock.appliersHold();
    };
    QCOMPARE(applier(r).run(request(configB()), false).outcome, Outcome::Ok);
    QVERIFY(heldAtSync);
    const int acquired = at(r, QStringLiteral("engineLock:acquired"));
    const int released = at(r, QStringLiteral("engineLock:released"));
    QVERIFY(acquired < at(r, QStringLiteral("firewall:sync")));
    QVERIFY(at(r, QStringLiteral("io:commitAs config.json")) < released);
    QVERIFY(at(r, QStringLiteral("firewall:sweep")) < released);
}

// INV-92. Kills: releasing engine.lock anywhere but immediately before enable/start.
void tst_Applier::releasedOnlyImmediatelyBeforeStart()
{
    Rig r;
    r.installedRunning(configA());
    QCOMPARE(applier(r).run(request(configB()), false).outcome, Outcome::Ok);
    const int released = at(r, QStringLiteral("engineLock:released"));
    QVERIFY(at(r, QStringLiteral("io:committed start.token")) < released);
    QCOMPARE(r.world.log.indexOf(QStringLiteral("service:"), released), at(r, QStringLiteral("service:allowStarts")));
    QCOMPARE(r.world.log.count(QStringLiteral("engineLock:released")), 1);
}

// INV-92. Kills: requesting the stop before blocking starts.
void tst_Applier::blockStartsBeforeStop()
{
    Rig r;
    r.installedRunning(configA());
    applier(r).run(request(configB()), false);
    QVERIFY(at(r, QStringLiteral("service:inhibitStarts")) < at(r, QStringLiteral("service:requestStop")));
    Rig s;
    s.installedRunning(configA());
    applier(s).saveAndRemove(request(configB()));
    QVERIFY(at(s, QStringLiteral("service:inhibitStarts")) < at(s, QStringLiteral("service:requestStop")));
}

// INV-92/113. Kills: revoking the token after B1 (or not at all), or after any firewall/config change.
void tst_Applier::tokenDeletedBeforeAnyChange()
{
    for (int op = 0; op < 3; ++op) {
        Rig r;
        r.installedRunning(configA());
        Applier a = applier(r);
        if (op == 0) a.run(request(configB()), false);
        if (op == 1) a.start();
        if (op == 2) a.deactivate(request(configB()));
        const int revoked = at(r, QStringLiteral("io:flushExisting start.token"));
        QVERIFY(revoked >= 0);
        QVERIFY(revoked < at(r, QStringLiteral("service:inhibitStarts")));
        const int firstFirewall = r.world.log.indexOf(QStringLiteral("firewall:"));
        if (firstFirewall >= 0) QVERIFY(revoked < firstFirewall);
        const int firstTemp = r.world.log.indexOf(QStringLiteral("io:createTemp"));
        if (firstTemp >= 0 && op != 2) QVERIFY(revoked < firstTemp);
    }
}

// ---------------------------------------------------------------------------------------------------
// INV-98. Kills: classifying IncompleteInstall as Foreign (or letting Apply/Start use it).
void tst_Applier::incompleteInstallResumes()
{
    Rig r;
    r.world.files.plantFile(kConfig, configA(), QStringLiteral("root"));
    r.service.installation.state = InstallState::IncompleteInstall;   // e.g. killed after the mask
    QCOMPARE(applier(r).run(request(configB()), false).outcome, Outcome::NothingChanged);
    QVERIFY(applier(r).run(std::nullopt, false).message.contains(QStringLiteral("--install")));
    QCOMPARE(applier(r).start().outcome, Outcome::NothingChanged);
    const Result res = applier(r).run(request(configB()), true);
    QVERIFY2(res.outcome == Outcome::Ok, qPrintable(res.message + QLatin1Char('\n') + events(r)));
    QCOMPARE(r.service.installation.state, InstallState::Current);
    QCOMPARE(r.file(kConfig), configB());
    QCOMPARE(LockIdentity::classify(LockFileRead{LockFileRead::Status::Ok, r.engineLock.lockContent(), {}},
                                    LockFileRead{LockFileRead::Status::Ok, r.engineLock.recordContent(), {}}),
             LockIdentity::PairState::Matching);
}

// ---------------------------------------------------------------------------------------------------
// INV-102. Kills: ignoring the request's source digest.
void tst_Applier::pendingConflictRefused()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(kPending, configC(), QStringLiteral("root"));   // a CLI edit after the GUI loaded A
    const Result res = applier(r).run(request(configB(), Source::Committed, configA()), false);
    QCOMPARE(res.outcome, Outcome::NothingChanged);
    QVERIFY(res.message.contains(QStringLiteral("replace them explicitly")));
    QCOMPARE(r.file(kPending), configC());
    QCOMPARE(r.file(kConfig), configA());
    QVERIFY(r.service.engineRunning);
    // Untick has the same precedence rule.
    QCOMPARE(applier(r).saveAndRemove(request(configB(), Source::Committed, configA())).outcome, Outcome::NothingChanged);
}

void tst_Applier::pendingFromTheSameSourceIsNotAConflict()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(kPending, configC(), QStringLiteral("root"));
    QCOMPARE(applier(r).run(request(configB(), Source::Pending, configC()), false).outcome, Outcome::Ok);
    QCOMPARE(r.file(kConfig), configB());
}

// INV-102. Kills: keeping the pending file after a commit (delete pending only when promoted).
void tst_Applier::supersedePendingCommitsAndDeletes()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(kPending, configC(), QStringLiteral("root"));
    const Result res = applier(r).run(request(configB(), Source::Committed, configA(), true), false);
    QCOMPARE(res.outcome, Outcome::Ok);
    QCOMPARE(r.file(kConfig), configB());
    QVERIFY(!r.has(kPending));
}

// ---------------------------------------------------------------------------------------------------
// INV-108: each crash fixture is followed by a reload and a re-apply.
void tst_Applier::crashBeforeRenameKeepsOldFiles()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(kPending, configC(), QStringLiteral("root"));
    r.io.crashBefore(QStringLiteral("commitAs"), kPending);
    bool crashed = false;
    try {
        applier(r).run(request(configB(), Source::Committed, configA(), true), false);
    } catch (const SimulatedCrash&) {
        crashed = true;
    }
    QVERIFY(crashed);
    powerLoss(r);
    const Reloaded seen = reload(r);
    QCOMPARE(seen.committed, configA());
    QCOMPARE(seen.pending.value_or(QByteArray()), configC());   // the old pending P is intact
    QVERIFY(!r.has(kToken));                                     // B0 happened: nothing can run
    const Result again = applier(r).run(request(configB(), Source::Committed, configA(), true), false);
    QCOMPARE(again.outcome, Outcome::Ok);
    QCOMPARE(r.file(kConfig), configB());
}

// INV-108. Kills: reporting Staged right after R4's rename, before its directory sync.
void tst_Applier::crashAfterRenameBeforeSyncNotReportedStaged()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(kPending, configC(), QStringLiteral("root"));
    bool armed = false;
    r.service.onCall = [&r, &armed](const QString& m) {
        if (m == QLatin1String("inhibitStarts") && !armed) {
            armed = true;
            r.world.files.failNext(FakeFileIo::Op::SyncDir);   // B1 is followed by R4's commit: its sync fails
        }
    };
    const Result res = applier(r).run(request(configB(), Source::Committed, configA(), true), false);
    QCOMPARE(res.outcome, Outcome::Uncommitted);
    QCOMPARE(res.persisted, Persisted::None);   // N is visible, but not durably: never "Staged"
    QVERIFY(res.persistedSha256.isEmpty());
    QCOMPARE(r.file(kPending), configB());       // visible before the crash ...
    powerLoss(r);
    const Reloaded seen = reload(r);
    QCOMPARE(seen.committed, configA());
    QCOMPARE(seen.pending.value_or(QByteArray()), configC());   // ... and P again after power loss
    r.service.onCall = {};
    const Result again = applier(r).run(request(configB(), Source::Committed, configA(), true), false);
    QVERIFY2(again.outcome == Outcome::Ok, qPrintable(again.message + QLatin1Char('\n') + events(r)));
}

void tst_Applier::crashAfterDurableStagingShowsN()
{
    Rig r;
    r.installedRunning(configA());
    r.io.crashBefore(QStringLiteral("commitAs"), kConfig);
    try {
        applier(r).run(request(configB()), false);
        QFAIL("no crash");
    } catch (const SimulatedCrash&) {
    }
    powerLoss(r);
    const Reloaded seen = reload(r);
    QCOMPARE(seen.committed, configA());
    QCOMPARE(seen.pending.value_or(QByteArray()), configB());   // shown as "Saved, not applied"
    const Result again = applier(r).run(std::nullopt, false);  // re-apply promotes it
    QCOMPARE(again.outcome, Outcome::Ok);
    QCOMPARE(r.file(kConfig), configB());
    QVERIFY(!r.has(kPending));
}

// INV-108. Kills: skipping R4 (a stale pending P would be resurrected over a committed N).
void tst_Applier::crashAfterCommitNeverRevertsToStalePending()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(kPending, configC(), QStringLiteral("root"));
    r.io.crashBefore(QStringLiteral("remove"), kPending);   // R8
    try {
        applier(r).run(request(configB(), Source::Committed, configA(), true), false);
        QFAIL("no crash");
    } catch (const SimulatedCrash&) {
    }
    powerLoss(r);
    const Reloaded seen = reload(r);
    QCOMPARE(seen.committed, configB());
    QVERIFY2(!seen.pending || *seen.pending == seen.committed, "a stale pending would revert the committed N");
    QCOMPARE(applier(r).start().outcome, Outcome::Ok);
    QCOMPARE(r.file(kConfig), configB());
}

// INV-108. Kills: no drop of a pending equal to config.json (P5).
void tst_Applier::equalPendingDroppedOnReload()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(kPending, configA(), QStringLiteral("root"));
    QCOMPARE(applier(r).edit([](BridgeConfig&) {}).outcome, Outcome::SavedNotApplied);
    // Edit started from the committed config, because the equal pending was dropped first.
    QVERIFY(at(r, QStringLiteral("io:remove config.pending.json")) < at(r, QStringLiteral("io:createTemp")));
    Rig s;
    s.installedRunning(configA());
    s.world.files.plantFile(kPending, configA(), QStringLiteral("root"));
    QCOMPARE(applier(s).discardPending().outcome, Outcome::NothingChanged);   // nothing left to discard
}

// Kills: reporting Staged for a pending file that merely holds the submitted bytes. A
// Deactivate(N) whose pending write was renamed but not durable (directory sync failed), retried with the
// same N and no crash in between: the visible bytes match, but only a successful directory sync makes them
// Staged. With the sync failing again the retry must report None; after a power loss N is indeed gone.
// With the sync working, Staged is reported and N survives a power loss.
void tst_Applier::unchangedPendingRetryConfirmsDurability()
{
    for (const bool syncWorks : {false, true}) {
        Rig r;
        r.installedRunning(configA());
        r.world.files.eraseEntry(kToken);   // the review's trigger: no token, so B0 syncs nothing
        r.world.files.syncDir(FakeDir(), nullptr);
        r.world.files.failNext(FakeFileIo::Op::SyncDir);
        const Result first = applier(r).deactivate(request(configB()));
        QCOMPARE(first.persisted, Persisted::None);
        QCOMPARE(r.file(kPending), configB());   // visible, not durable
        if (!syncWorks) r.world.files.failAlways(FakeFileIo::Op::SyncDir);
        const Result retry = applier(r).deactivate(request(configB()));
        QCOMPARE(retry.outcome, Outcome::SavedNotApplied);
        r.world.files.clearFailures();
        powerLoss(r);
        const Reloaded seen = reload(r);
        if (syncWorks) {
            QCOMPARE(retry.persisted, Persisted::Staged);
            QCOMPARE(retry.persistedSha256, digest(configB()));
            QCOMPARE(seen.pending.value_or(QByteArray()), configB());
        } else {
            QCOMPARE(retry.persisted, Persisted::None);   // the editor keeps its dirty copy ...
            QVERIFY(retry.persistedSha256.isEmpty());
            QVERIFY(!seen.pending);                        // ... which is the only copy left
        }
    }
}

// The Run shortcut. Kills: Run reporting Staged in its prologue because config.pending.json
// already holds N. A Run whose R4 rename was not durable, retried with the same N while the barrier fails:
// nothing established N's durability, so the result is None (and a power loss indeed loses N).
void tst_Applier::unchangedPendingRunNotStagedBeforeR4()
{
    Rig r;
    r.installedRunning(configA());
    bool armed = false;
    r.service.onCall = [&r, &armed](const QString& m) {
        if (m == QLatin1String("inhibitStarts") && !armed) {
            armed = true;
            r.world.files.failNext(FakeFileIo::Op::SyncDir);   // R4's directory sync
        }
    };
    const Result first = applier(r).run(request(configB()), false);
    QCOMPARE(first.outcome, Outcome::Uncommitted);
    QCOMPARE(first.persisted, Persisted::None);
    QCOMPARE(r.file(kPending), configB());
    r.service.onCall = {};
    r.service.fail[QStringLiteral("inhibitStarts")] = QStringLiteral("access denied");
    const Result retry = applier(r).run(request(configB()), false);
    QCOMPARE(retry.outcome, Outcome::Uncommitted);
    QCOMPARE(retry.persisted, Persisted::None);
    QVERIFY(retry.persistedSha256.isEmpty());
    powerLoss(r);
    QVERIFY(!reload(r).pending);
}

// First path. Kills: an untick whose pending write is visible but not durable reporting "your
// edits are saved" without the warning edit() gives (persisted stays None).
void tst_Applier::untickPendingNotDurableWarns()
{
    for (const bool remove : {false, true}) {
        Rig r;
        r.installedRunning(configA());
        r.world.files.failNext(FakeFileIo::Op::SyncDir);   // the pending write's directory sync (before B0)
        Applier a = applier(r);
        const Result res = remove ? a.saveAndRemove(request(configB())) : a.deactivate(request(configB()));
        QCOMPARE(res.outcome, Outcome::SavedNotApplied);
        QCOMPARE(res.persisted, Persisted::None);
        QVERIFY2(res.message.contains(QStringLiteral("Warning: they may not survive a power loss")), qPrintable(res.message));
        QVERIFY2(res.message.contains(QStringLiteral("still open")), qPrintable(res.message));
    }
}

// Second path, and the audit row "untick without N, removal fails". Kills: "the bridge is off"
// after a barrier or removal step failed, and Outcome::Ok for an untick without N whose removal failed.
void tst_Applier::untickNeverSaysOffAfterAFailedStep()
{
    const QString off = QStringLiteral("the bridge is off");
    {   // barrier: the engine never releases its lock
        Rig r;
        r.installedRunning(configA());
        r.service.neverReleases = true;
        const Result res = applier(r).deactivate(request(configB()));
        QCOMPARE(res.outcome, Outcome::SavedNotApplied);
        QCOMPARE(res.persisted, Persisted::Staged);
        QVERIFY2(!res.message.contains(off, Qt::CaseInsensitive), qPrintable(res.message));
        QVERIFY(res.message.contains(QString::fromLatin1(kUnproven)));
    }
    {   // removal: uninstall fails
        Rig r;
        r.installedRunning(configA());
        r.service.fail[QStringLiteral("uninstall")] = QStringLiteral("marked for deletion");
        const Result res = applier(r).saveAndRemove(request(configB()));
        QCOMPARE(res.outcome, Outcome::SavedNotApplied);
        QVERIFY2(!res.message.contains(off, Qt::CaseInsensitive), qPrintable(res.message));
        QVERIFY(res.postcondition.contains(QStringLiteral("service not removed")));
    }
    {   // removal: the rules cannot be removed. The service stays (rules first, INV-91), disabled and verified
        Rig r;
        r.installedRunning(configA());
        r.firewall.fail[QStringLiteral("removeAll")] = QStringLiteral("RPC unavailable");
        const Result res = applier(r).saveAndRemove(request(configB()));
        QVERIFY2(!res.message.contains(off, Qt::CaseInsensitive), qPrintable(res.message));
        QVERIFY(res.postcondition.contains(QStringLiteral("firewall rules not removed")));
        QVERIFY2(res.postcondition.contains(QStringLiteral("the service not removed")), qPrintable(res.postcondition));
        QVERIFY2(res.postcondition.contains(QString::fromLatin1(kVerified)), qPrintable(res.postcondition));
        QVERIFY(!r.world.log.contains(QStringLiteral("service:uninstall")));
        QCOMPARE(r.service.installation.state, InstallState::Current);
    }
    {   // removal: the firewall cannot be connected. Nothing is removed, and the result says so
        Rig r;
        r.installedRunning(configA());
        r.firewall.fail[QStringLiteral("connect")] = QStringLiteral("RPC_E_DISCONNECTED");
        const QStringList rules = r.firewall.rules;
        const Result res = applier(r).saveAndRemove(std::nullopt);
        QCOMPARE(enumName(res.outcome), enumName(Outcome::Uncommitted));
        QVERIFY2(res.message.contains(QStringLiteral("could not connect to the firewall")), qPrintable(res.message));
        QVERIFY2(res.postcondition.contains(QStringLiteral("firewall rules not removed and the service not removed")),
                 qPrintable(res.postcondition));
        QVERIFY2(res.postcondition.contains(QString::fromLatin1(kVerified)), qPrintable(res.postcondition));
        QVERIFY(!r.world.log.contains(QStringLiteral("firewall:removeAll")));
        QVERIFY(!r.world.log.contains(QStringLiteral("service:uninstall")));
        QCOMPARE(r.firewall.rules, rules);
        QCOMPARE(r.service.installation.state, InstallState::Current);
        QVERIFY(r.service.startsBlocked);
        // Retried once the firewall answers, the removal completes.
        r.firewall.fail.clear();
        const Result again = applier(r).saveAndRemove(std::nullopt);
        QVERIFY2(again.outcome == Outcome::Ok, qPrintable(again.message));
        QVERIFY(r.firewall.rules.isEmpty());
        QCOMPARE(r.service.installation.state, InstallState::NotInstalled);
    }
    for (const char* failing : {"connect", "removeAll"}) {   // no service installed, rules left behind
        Rig r;
        r.installedRunning(configA());
        r.service.installation.state = InstallState::NotInstalled;
        r.service.engineRunning = r.service.engineHoldsLock = false;
        r.service.pid = 0;
        r.firewall.fail[QString::fromLatin1(failing)] = QStringLiteral("RPC unavailable");
        const Result res = applier(r).saveAndRemove(std::nullopt);
        QCOMPARE(enumName(res.outcome), enumName(Outcome::Uncommitted));
        QVERIFY2(res.postcondition.startsWith(QStringLiteral("firewall rules not removed")), qPrintable(res.postcondition));
        QVERIFY2(res.postcondition.endsWith(QStringLiteral("; not installed")), qPrintable(res.postcondition));
        QVERIFY2(!res.postcondition.contains(QStringLiteral("service not removed")), qPrintable(res.postcondition));
        QVERIFY2(!res.message.contains(QStringLiteral("service not removed")), qPrintable(res.message));
    }
    {   // without N: a failed removal is not Ok
        Rig r;
        r.installedRunning(configA());
        r.service.fail[QStringLiteral("uninstall")] = QStringLiteral("marked for deletion");
        const Result res = applier(r).saveAndRemove(std::nullopt);
        QCOMPARE(res.outcome, Outcome::Uncommitted);
        QCOMPARE(res.step, QStringLiteral("remove the service"));
        QVERIFY2(!res.message.contains(off, Qt::CaseInsensitive), qPrintable(res.message));
        QVERIFY(res.postcondition.contains(QString::fromLatin1(kVerified)));
    }
    {   // success still says it
        Rig r;
        r.installedRunning(configA());
        const Result res = applier(r).deactivate(request(configB()));
        QCOMPARE(res.outcome, Outcome::SavedNotApplied);
        QVERIFY2(res.message.contains(off), qPrintable(res.message));
    }
}

// Audit: DiscardPending reported "discarded" although a failed directory sync lets a power loss bring
// config.pending.json back.
void tst_Applier::discardWarnsWhenTheDeletionIsNotDurable()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(kPending, configB(), QStringLiteral("root"));
    r.world.files.failNext(FakeFileIo::Op::SyncDir);
    const Result res = applier(r).discardPending();
    QCOMPARE(res.outcome, Outcome::Ok);
    QVERIFY2(res.message.contains(QStringLiteral("may come back after a power loss")), qPrintable(res.message));
    powerLoss(r);
    QCOMPARE(reload(r).pending.value_or(QByteArray()), configB());   // the warning was true
}

// Audit: Start reports persisted = Committed for config.json; an earlier R7 rename
// whose directory sync failed is not durable, so Start establishes the sync first, or does not claim it.
void tst_Applier::startConfirmsCommittedDurability()
{
    Rig r;
    r.installedRunning(configA());
    r.service.onCall = [&r](const QString& m) {
        if (m == QLatin1String("requestStop")) r.world.files.failNext(FakeFileIo::Op::SyncDir);   // after B0's own sync
    };
    const Result res = applier(r).start();
    QCOMPARE(res.outcome, Outcome::Uncommitted);
    QCOMPARE(res.step, QStringLiteral("make the saved settings durable"));
    QCOMPARE(res.persisted, Persisted::None);
    QVERIFY(!r.world.log.contains(QStringLiteral("firewall:sync")));
    QVERIFY(res.postcondition.contains(QString::fromLatin1(kVerified)));
}

// Audit: a recovery whose record publication fails after engine.lock was replaced reports the replaced
// lock among the file changes (it used to be recorded only after both publications).
void tst_Applier::lockPublicationReportedEvenWhenTheRecordFails()
{
    Rig r;
    r.installedRunning(configA());
    r.service.engineRunning = r.service.engineHoldsLock = false;
    r.service.pid = 0;
    r.engineLock.plantLock(LockIdentity::format(QByteArrayLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")));
    static_cast<SpyFileIo&>(r.engineLock.io()).beforeHook = [&r](const QString& op, const QString& name) {
        if (op == QLatin1String("commitAs") && name == QLatin1String("engine.lock.id"))
            r.world.lockFiles.failNext(FakeFileIo::Op::Rename);
    };
    const Result res = applier(r).run(std::nullopt, true);
    QCOMPARE(res.outcome, Outcome::Uncommitted);
    QVERIFY2(res.fileChanges.join(QLatin1Char('|')).contains(QStringLiteral("engine.lock published")),
             qPrintable(res.fileChanges.join(QLatin1Char('|'))));
    QVERIFY(res.message.contains(QStringLiteral("engine.lock published")));
}

// Kills: P5 removing every *.asbtmp. The running engine writes status.json through its own
// temp (Runtime policy, owner "process"); P5 runs before the barrier, while that write may be in flight,
// so only the temps of the files the Applier writes (config.json, config.pending.json, start.token) go.
void tst_Applier::p5KeepsTheEnginesStatusTemps()
{
    const QString hex = QStringLiteral("0123456789abcdef0123456789abcdef");
    const QString statusTemp = QStringLiteral("status.json.%1.asbtmp").arg(hex);
    const QStringList ours = {QStringLiteral("config.json.%1.asbtmp").arg(hex),
                              QStringLiteral("config.pending.json.%1.asbtmp").arg(hex),
                              QStringLiteral("start.token.%1.asbtmp").arg(hex)};
    for (int op = 0; op < 2; ++op) {
        Rig r;
        r.installedRunning(configA());
        // The engine is mid-write: its temp exists, the rename over status.json has not happened yet.
        r.world.files.plantFile(statusTemp, QByteArrayLiteral("{\"pid\":"), QStringLiteral("process"));
        for (const QString& n : ours) r.world.files.plantFile(n, QByteArrayLiteral("{"), QStringLiteral("root"));
        bool engineRunningAtP5 = false;
        r.io.beforeHook = [&](const QString& o, const QString& name) {
            if (o == QLatin1String("remove") && ours.contains(name)) engineRunningAtP5 = r.service.engineRunning;
        };
        const Result res = op == 0 ? applier(r).edit([](BridgeConfig& c) { c.ports[0].tcpPort = 4005; })
                                   : applier(r).run(request(configB()), false);
        QVERIFY2(res.outcome == (op == 0 ? Outcome::SavedNotApplied : Outcome::Ok), qPrintable(res.message));
        QVERIFY(engineRunningAtP5);
        QVERIFY2(r.has(statusTemp), "P5 deleted the engine's in-flight status temp");
        QCOMPARE(r.file(statusTemp), QByteArrayLiteral("{\"pid\":"));
        for (const QString& n : ours) QVERIFY2(!r.has(n), qPrintable(n));
    }
}

// ---------------------------------------------------------------------------------------------------
// INV-113 (N9-4): truncate -> write -> file flush -> remove -> directory sync, all before any B1 call.
void tst_Applier::revocationOrder()
{
    Rig r;
    r.installedRunning(configA());
    applier(r).run(request(configB()), false);
    int i = 0;
    for (const char* e : {"io:openExisting start.token", "io:truncate start.token", "io:writeAll start.token",
                          "io:flushExisting start.token", "io:removeByHandle start.token", "io:syncDir"}) {
        const int next = r.world.log.indexOf(QString::fromLatin1(e), i);
        QVERIFY2(next >= 0, e);
        i = next + 1;
    }
    QVERIFY(i <= at(r, QStringLiteral("service:inhibitStarts")));
}

// INV-113. Kills: omitting the file flush (after a power loss the old, valid token would come back).
void tst_Applier::revocationSurvivesResurrection()
{
    Rig r;
    r.installedRunning(configA());
    // B0's deletion is never made durable: no directory sync succeeds in this operation (Start's own
    // durability confirmation of config.json then fails too, so it stops Uncommitted after B0).
    r.world.files.failAlways(FakeFileIo::Op::SyncDir);
    QCOMPARE(applier(r).start().outcome, Outcome::Uncommitted);
    r.world.files.clearFailures();
    powerLoss(r);
    QVERIFY(r.has(kToken));   // resurrected
    QString why;
    QVERIFY(!r.token->read(&why));
    r.service.startsBlocked = false;
    r.service.launchEngine();
    QVERIFY(r.world.log.contains(QStringLiteral("engine:exit 6")));
}

// INV-113. Kills: reporting NothingChanged on a revocation flush failure.
void tst_Applier::revocationFlushFailureIsUncommitted()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.failNext(FakeFileIo::Op::FlushExisting);
    const Result res = applier(r).start();
    QCOMPARE(res.outcome, Outcome::Uncommitted);
    QVERIFY(res.message.contains(QStringLiteral("may already be invalidated")));
    QVERIFY(!r.world.log.contains(QStringLiteral("service:inhibitStarts")));
}

// INV-113. Kills: writing the token before the commit rename.
void tst_Applier::tokenWrittenOnlyAfterCommit()
{
    Rig r;
    r.installedRunning(configA());
    applier(r).run(request(configB()), false);
    QVERIFY(at(r, QStringLiteral("io:committed config.json")) < at(r, QStringLiteral("io:commitAs start.token")));
    Rig s;
    s.installedRunning(configA());
    s.firewall.fail[QStringLiteral("sync")] = QStringLiteral("x");
    applier(s).run(request(configB()), false);
    QVERIFY(!s.world.log.contains(QStringLiteral("io:commitAs start.token")));
}

// INV-113 (pure half). Kills: B0 touching a token inside an untrusted folder.
void tst_Applier::repairUntrustedFolderTouchesNothingInside()
{
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(QStringLiteral("planted.txt"), "x");
    QCOMPARE(applier(r).repair().outcome, Outcome::Ok);
    QVERIFY(!r.world.log.contains(QStringLiteral("io:openExisting start.token")));
    QVERIFY(!r.world.log.contains(QStringLiteral("folder:read")));
    QVERIFY(r.folder.aside.contains(kToken));
}

// INV-116. Kills: checking the file system only after a mutation.
void tst_Applier::unsupportedFileSystemRefusedBeforeAnyMutation()
{
    Rig r;
    r.installedRunning(configA());
    r.engineLock.unsupportedFs = QStringLiteral("overlayfs");
    const Result res = applier(r).run(request(configB()), true);
    QCOMPARE(res.outcome, Outcome::NothingChanged);
    QVERIFY(res.message.contains(QStringLiteral("overlayfs")));
    QVERIFY(!r.world.log.contains(QStringLiteral("io:")));
    QVERIFY(!r.world.log.contains(QStringLiteral("service:inhibitStarts")));
}

// ---------------------------------------------------------------------------------------------------
// --force-recreate is Repair first under the same lock: the untrusted live folder is moved
// aside only after Repair's barrier proved the engine stopped. Kills: the folder repair before the barrier.
void tst_Applier::forceRecreateProvesTheStopBeforeMovingTheFolder_data()
{
    QTest::addColumn<int>("fault");   // 0 none, 1 engine never releases, 2 block starts fails, 3 stop fails
    QTest::addColumn<bool>("install");
    for (bool install : {false, true}) {
        const char* op = install ? "--install" : "--apply";
        QTest::addRow("%s, barrier succeeds", op) << 0 << install;
        QTest::addRow("%s, engine never releases", op) << 1 << install;
        QTest::addRow("%s, block starts fails", op) << 2 << install;
        QTest::addRow("%s, stop request fails", op) << 3 << install;
    }
}

void tst_Applier::forceRecreateProvesTheStopBeforeMovingTheFolder()
{
    QFETCH(int, fault);
    QFETCH(bool, install);
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(QStringLiteral("planted.txt"), "x");   // untrusted: Repair would move it aside
    if (fault == 1) r.service.neverReleases = true;
    if (fault == 2) r.service.fail[QStringLiteral("inhibitStarts")] = QStringLiteral("denied");
    if (fault == 3) r.service.fail[QStringLiteral("requestStop")] = QStringLiteral("denied");
    const Result res = applier(r).run(request(configB()), install, true);
    if (fault == 0) {
        QVERIFY2(res.outcome == Outcome::Ok, qPrintable(res.message));
        QCOMPARE(r.folder.renames, 1);
        // The barrier proved the stop (engine.lock held) before the live folder moved.
        QVERIFY(at(r, QStringLiteral("engineLock:acquired")) < at(r, QStringLiteral("folder:renameAside")));
        QVERIFY(at(r, QStringLiteral("service:inhibitStarts")) < at(r, QStringLiteral("folder:renameAside")));
        return;
    }
    QVERIFY2(res.outcome != Outcome::Ok && res.outcome != Outcome::NothingChanged, qPrintable(res.message));
    QCOMPARE(r.folder.renames, 0);
    QCOMPARE(r.folder.creates, 0);
    QVERIFY(!r.world.log.contains(QStringLiteral("folder:renameAside")));
    QCOMPARE(r.file(kConfig), configA());
    QVERIFY(r.has(QStringLiteral("planted.txt")));
    QCOMPARE(res.persisted, Persisted::None);
}

// After --force-recreate's barrier, a refusal (here an unreadable pending file found in the
// repaired folder) is Uncommitted with failClosed's proof, not "not changed": the barrier changed the service.
void tst_Applier::forceRecreateRefusalAfterTheBarrierIsUncommitted()
{
    for (bool forced : {false, true}) {
        Rig r;
        r.installedRunning(configA());
        r.world.files.plantFile(kPending, QByteArrayLiteral("{\"version\":1,\"ports\":["), QStringLiteral("root"));
        const Result res = applier(r).run(std::nullopt, false, forced);
        if (!forced) {
            QCOMPARE(res.outcome, Outcome::NothingChanged);   // before any barrier: nothing changed
            QVERIFY(r.service.engineRunning);
            continue;
        }
        QVERIFY2(res.outcome == Outcome::Uncommitted, qPrintable(res.message));
        QVERIFY2(res.postcondition.contains(QString::fromLatin1(kVerified)), qPrintable(res.postcondition));
        QVERIFY(!res.message.contains(QStringLiteral("not changed")));
        QVERIFY(r.service.startsBlocked);
        QCOMPARE(r.file(kConfig), configA());
    }
}

// WS-5 round 2, audit (b): a native call that returns after its phase deadline stops the operation before
// the next irreversible step. Each row makes one call late and names the step that must not follow.
void tst_Applier::lateCallStopsBeforeTheNextIrreversibleStep_data()
{
    QTest::addColumn<int>("row");
    const char* names[] = {"installation query (Run)",      "folder normalisation (Run)",  "installation query (Stop)",
                           "folder normalisation (Edit)",   "folder normalisation (Discard)", "Start: config.json read",
                           "SaveAndRemove: pending write",  "Repair: folder repair",       "--force-recreate: folder repair",
                           "--force-recreate: the last folder call", "Repair: the last folder call, nothing after it",
                           "Deactivate, not installed: pending write, nothing after it"};
    for (int i = 0; i < 12; ++i) QTest::newRow(names[i]) << i;
}

void tst_Applier::lateCallStopsBeforeTheNextIrreversibleStep()
{
    QFETCH(int, row);
    Rig r;
    r.installedRunning(configA());
    auto lateOnce = [&r](const QString& method, std::chrono::seconds by) {
        auto done = std::make_shared<bool>(false);
        return [&r, method, by, done](const QString& m) {
            if (*done || m != method) return;
            *done = true;
            r.world.clock.advance(by);
        };
    };
    Applier a = applier(r);
    Result res;
    QString mustNotRun;
    switch (row) {
    case 0:
        r.service.onCall = lateOnce(QStringLiteral("detect"), std::chrono::seconds(16));
        res = a.run(request(configB()), false);
        mustNotRun = QStringLiteral("folder:normalise");
        break;
    case 1:
        r.folder.onCall = lateOnce(QStringLiteral("normalise"), std::chrono::seconds(16));
        res = a.run(request(configB()), false);
        mustNotRun = QStringLiteral("io:openExisting start.token");   // B0
        break;
    case 2:
        r.service.onCall = lateOnce(QStringLiteral("detect"), std::chrono::seconds(16));
        res = a.stop();
        mustNotRun = QStringLiteral("io:openExisting start.token");
        break;
    case 3:
        r.folder.onCall = lateOnce(QStringLiteral("normalise"), std::chrono::seconds(16));
        res = a.edit([](BridgeConfig& c) { c.ports[0].tcpPort = 4005; });
        mustNotRun = QStringLiteral("io:commitAs config.pending.json");
        break;
    case 4:
        r.world.files.plantFile(kPending, configB(), QStringLiteral("root"));
        r.folder.onCall = lateOnce(QStringLiteral("normalise"), std::chrono::seconds(16));
        res = a.discardPending();
        mustNotRun = QStringLiteral("io:remove config.pending.json");
        break;
    case 5: {
        auto done = std::make_shared<bool>(false);
        r.service.onCall = [&r, done](const QString& m) {   // the barrier's phase runs out while reading B
            if (m == QLatin1String("requestStop") && !*done) {
                *done = true;
                r.world.clock.advance(std::chrono::seconds(29));
            }
        };
        r.folder.onRead = [&r](const QString& name) {
            if (name == QLatin1String("config.json")) r.world.clock.advance(std::chrono::seconds(2));
        };
        res = a.start();
        mustNotRun = QStringLiteral("firewall:connect");
        break;
    }
    case 6:
        r.io.beforeHook = [&r](const QString& op, const QString& name) {
            if (op == QLatin1String("commitAs") && name == QLatin1String("config.pending.json"))
                r.world.clock.advance(std::chrono::seconds(31));
        };
        res = a.saveAndRemove(request(configB()));
        mustNotRun = QStringLiteral("firewall:removeAll");   // the removal behind the barrier
        break;
    case 7:   // a mismatched pair, so the recovery behind the folder repair would publish
        r.service.engineRunning = r.service.engineHoldsLock = false;
        r.service.pid = 0;
        r.engineLock.plantLock(LockIdentity::format(QByteArray(32, 'b')));
        r.world.files.plantFile(QStringLiteral("planted.txt"), "x");
        r.folder.onCall = lateOnce(QStringLiteral("renameAside"), std::chrono::seconds(31));
        res = a.repair();
        mustNotRun = QStringLiteral("lockio:commitAs");
        break;
    case 8:
        r.world.files.plantFile(QStringLiteral("planted.txt"), "x");
        r.folder.onCall = lateOnce(QStringLiteral("renameAside"), std::chrono::seconds(31));
        res = a.run(request(configB()), false, true);
        mustNotRun = QStringLiteral("io:commitAs config.pending.json");   // R4
        break;
    case 9:   // round 3: the folder phase's last call; only the overrun latched at R4's phase change can see it
        r.world.files.plantFile(QStringLiteral("planted.txt"), "x");
        r.folder.onCall = lateOnce(QStringLiteral("normalise"), std::chrono::seconds(31));
        res = a.run(request(configB()), false, true);
        mustNotRun = QStringLiteral("io:commitAs config.pending.json");   // R4
        break;
    case 10:   // a matching pair: no recovery follows the folder repair, so only Repair's own check can see it
        r.world.files.plantFile(QStringLiteral("planted.txt"), "x");
        r.folder.onCall = lateOnce(QStringLiteral("normalise"), std::chrono::seconds(31));
        res = a.repair();
        mustNotRun = QStringLiteral("lockio:commitAs");
        break;
    case 11:   // no barrier and no removal follow: only the untick's own check can see the late write
        r.service.installation.state = InstallState::NotInstalled;
        r.io.beforeHook = [&r](const QString& op, const QString& name) {
            if (op == QLatin1String("commitAs") && name == QLatin1String("config.pending.json"))
                r.world.clock.advance(std::chrono::seconds(31));
        };
        res = a.deactivate(request(configB()));
        mustNotRun = QStringLiteral("service:inhibitStarts");
        break;
    }
    QVERIFY2(res.message.contains(QStringLiteral("returned after its deadline")), qPrintable(res.message));
    QVERIFY2(res.outcome != Outcome::Ok, qPrintable(res.message));
    // The step after the late call never ran (failClosed may run: it is the cleanup D57 asks for).
    QVERIFY2(r.world.log.indexOf(mustNotRun) < 0, qPrintable(mustNotRun + QLatin1Char('\n') + events(r)));
    if (row < 7 || row == 11) QCOMPARE(r.file(kConfig), configA());
    else QCOMPARE(r.folder.aside.value(kConfig), configA());
}

// ---------------------------------------------------------------------------------------------------
// WS-5 round 2, audit (a): no dead ends. For each state an operation can stop in, the operation its
// message advises succeeds from that state, and the original intent then succeeds too.
namespace {
using Op2 = std::function<Result(Rig&, Applier&)>;
struct Advised {
    const char* name;
    std::function<void(Rig&)> setup;
    Op2 refused;                  // the operation that stops in the state
    Outcome refusedOutcome;
    const char* advice;           // a substring of its message naming the recovery
    std::function<void(Rig&)> between;   // what changes meanwhile (a released engine, a manual fix); may be empty
    Op2 recovery;                 // the advised operation
    Op2 again;                    // the original intent
};

void stopTheEngine(Rig& r)
{
    r.service.engineRunning = r.service.engineHoldsLock = false;
    r.service.pid = 0;
}
const QByteArray kRecordedId = QByteArrayLiteral("5555555555555555aaaaaaaaaaaaaaaa");   // installedRunning's id

QList<Advised> advisedRows()
{
    const Op2 start = [](Rig&, Applier& a) { return a.start(); };
    const Op2 apply = [](Rig&, Applier& a) { return a.run(request(configB()), false); };
    const Op2 install = [](Rig&, Applier& a) { return a.run(std::nullopt, true); };
    const Op2 repair = [](Rig&, Applier& a) { return a.repair(); };
    QList<Advised> t;
    t << Advised{"mismatched pair (Linux) -> --install",
                 [](Rig& r) { stopTheEngine(r); r.engineLock.plantLock(LockIdentity::format(QByteArray(32, 'b'))); },
                 start, Outcome::NothingChanged, "run --install or Repair", {}, install, start};
    t << Advised{"mismatched pair (Windows) -> Repair",
                 [](Rig& r) {
                     stopTheEngine(r);
                     r.engineLock.windows();
                     r.engineLock.plantLock(LockIdentity::format(QByteArray(32, 'b')));
                 },
                 start, Outcome::StateUnknown, "Run --install or Repair", {}, repair, start};
    t << Advised{"record only (Linux) -> Repair",
                 [](Rig& r) { stopTheEngine(r); r.world.lockFiles.eraseEntry(QStringLiteral("engine.lock")); },
                 start, Outcome::NothingChanged, "run --install or Repair", {}, repair, start};
    t << Advised{"partial (Windows) -> --install",
                 [](Rig& r) {
                     stopTheEngine(r);
                     r.engineLock.windows();
                     r.world.lockFiles.eraseEntry(QStringLiteral("engine.lock.id"));
                 },
                 start, Outcome::NothingChanged, "run --install or Repair", {}, install, start};
    t << Advised{"insecure lock file (Linux) -> Repair",
                 [](Rig& r) { stopTheEngine(r); r.engineLock.plantInsecureLock(LockIdentity::format(kRecordedId)); },
                 start, Outcome::NothingChanged, "run --install or Repair", {}, repair, start};
    t << Advised{"insecure lock file (Windows) -> --install",
                 [](Rig& r) {
                     stopTheEngine(r);
                     r.engineLock.windows();
                     r.engineLock.plantInsecureLock(LockIdentity::format(kRecordedId));
                 },
                 start, Outcome::StateUnknown, "Run --install or Repair", {}, install, start};
    t << Advised{"insecure lock directory -> manual", [](Rig& r) { r.engineLock.insecure = true; }, start,
                 Outcome::StateUnknown, "An administrator must restore", [](Rig& r) { r.engineLock.insecure = false; },
                 start, apply};
    t << Advised{"unsupported lock file system -> manual",
                 [](Rig& r) { r.engineLock.unsupportedFs = QStringLiteral("nfs"); }, start, Outcome::NothingChanged,
                 "unsupported file system", [](Rig& r) { r.engineLock.unsupportedFs.reset(); }, start, apply};
    t << Advised{"untrusted data folder -> Repair", [](Rig& r) { r.world.files.plantFile(QStringLiteral("planted.txt"), "x"); },
                 apply, Outcome::NothingChanged, "Run Repair", {}, repair, apply};
    t << Advised{"folder recreated by Repair -> Save & apply",
                 [](Rig& r) { r.world.files.plantFile(QStringLiteral("planted.txt"), "x"); }, repair, Outcome::Ok,
                 "run Save & apply", {}, apply, start};
    t << Advised{"no saved settings -> Save & apply",
                 [](Rig& r) {
                     r.world.files.plantFile(QStringLiteral("planted.txt"), "x");
                     QCOMPARE(applier(r).repair().outcome, Outcome::Ok);
                 },
                 start, Outcome::Uncommitted, "use Save & apply", {}, apply, start};
    t << Advised{"Repair with an insecure lock file, folder step failed -> Repair again",
                 [](Rig& r) {
                     stopTheEngine(r);
                     r.engineLock.plantInsecureLock(LockIdentity::format(kRecordedId));
                     r.world.files.plantFile(QStringLiteral("planted.txt"), "x");
                     r.folder.failRenameAside = QStringLiteral("sharing violation");
                 },
                 repair, Outcome::NothingChanged, "run Repair again", [](Rig& r) { r.folder.failRenameAside.reset(); },
                 repair, apply};
    t << Advised{"start token possibly invalidated -> Start",
                 [](Rig& r) { r.world.files.failNext(FakeFileIo::Op::FlushExisting); }, apply, Outcome::Uncommitted,
                 "until a Start or Save & apply succeeds", {}, start, apply};
    t << Advised{"start token damaged, engine exits 6 -> Start",
                 [](Rig& r) {
                     stopTheEngine(r);
                     r.world.files.plantFile(kToken, QByteArrayLiteral("{\"token\":"), QStringLiteral("root"));
                 },
                 [](Rig& r, Applier&) {
                     r.service.launchEngine();
                     Result res;
                     res.outcome = Outcome::StateUnknown;
                     // The host prints exit 6 (WS-7, WS-8): "not activated: an apply did not finish; run Start
                     // or Save & apply" (master §4.6).
                     if (r.world.log.contains(QStringLiteral("engine:exit 6")))
                         res.message = QStringLiteral("exit 6: run Start or Save & apply");
                     return res;
                 },
                 Outcome::StateUnknown, "run Start or Save & apply", {}, start, apply};
    t << Advised{"incomplete install -> --install",
                 [](Rig& r) { r.service.installation.state = InstallState::IncompleteInstall; }, apply,
                 Outcome::NothingChanged, "finish it with --install", {}, install, apply};
    t << Advised{"pending conflict -> replace explicitly",
                 [](Rig& r) { r.world.files.plantFile(kPending, configC(), QStringLiteral("root")); }, apply,
                 Outcome::NothingChanged, "replace them explicitly", {},
                 [](Rig&, Applier& a) { return a.run(request(configB(), Source::Committed, {}, true), false); }, start};
    t << Advised{"Stop -> Start", {}, [](Rig&, Applier& a) { return a.stop(); }, Outcome::Ok, "until Start or Save & apply",
                 {}, start, apply};
    t << Advised{"Edit -> --apply", {}, [](Rig&, Applier& a) { return a.edit([](BridgeConfig& c) { c.ports[0].tcpPort = 4005; }); },
                 Outcome::SavedNotApplied, "run --apply", {}, [](Rig&, Applier& a) { return a.run(std::nullopt, false); },
                 start};
    t << Advised{"engine outlives the barrier -> retry", [](Rig& r) { r.service.neverReleases = true; }, apply,
                 Outcome::StateUnknown, "Could not prove the bridge stopped",
                 [](Rig& r) {
                     r.service.neverReleases = false;
                     r.service.engineHoldsLock = false;   // the old process exits
                 },
                 apply, start};
    t << Advised{"late native call -> retry",
                 [](Rig& r) { r.firewall.callDuration = std::chrono::seconds(61); }, apply, Outcome::StateUnknown,
                 "returned after its deadline", [](Rig& r) { r.firewall.callDuration = {}; }, apply, start};
    t << Advised{"engine-free proof fails -> retry once the process is gone",
                 [](Rig& r) {
                     r.world.lockFiles.eraseEntry(QStringLiteral("engine.lock"));   // record only: needs a recovery
                     r.service.engineHoldsLock = false;
                     r.service.neverReleases = true;   // the process outlives its stop request
                 },
                 install, Outcome::StateUnknown, "Could not prove that no engine runs",
                 [](Rig& r) {
                     r.service.neverReleases = false;   // the old process exits
                     r.service.pid = 0;
                 },
                 install, start};
    t << Advised{"bridge did not confirm (ACK timeout) -> Start", [](Rig& r) { r.service.ack = AckMode::Never; }, apply,
                 Outcome::StateUnknown, "did not confirm", [](Rig& r) { r.service.ack = AckMode::Correct; }, start, apply};
    t << Advised{"committed, not running (enable failed) -> Start",
                 [](Rig& r) { r.service.fail[QStringLiteral("allowStarts")] = QStringLiteral("denied"); }, apply,
                 Outcome::CommittedNotRunning, "the bridge did not start", [](Rig& r) { r.service.fail.clear(); }, start,
                 apply};
    t << Advised{"saved, not applied (firewall failed) -> --apply",
                 [](Rig& r) { r.firewall.fail[QStringLiteral("sync")] = QStringLiteral("WBEM_E_FAILED"); }, apply,
                 Outcome::Uncommitted, "Your edits are saved but not applied", [](Rig& r) { r.firewall.fail.clear(); },
                 [](Rig&, Applier& a) { return a.run(std::nullopt, false); }, start};
    t << Advised{"another operation holds apply.lock -> retry", [](Rig& r) { r.world.applyLock.alwaysTimeout = true; },
                 apply, Outcome::NothingChanged, "another operation is still running",
                 [](Rig& r) { r.world.applyLock.alwaysTimeout = false; }, apply, start};
    return t;
}
}  // namespace

void tst_Applier::advisedRecoverySucceeds_data()
{
    QTest::addColumn<int>("row");
    const QList<Advised> t = advisedRows();
    for (int i = 0; i < t.size(); ++i) QTest::newRow(t[i].name) << i;
}

void tst_Applier::advisedRecoverySucceeds()
{
    QFETCH(int, row);
    const Advised t = advisedRows().at(row);
    Rig r;
    r.installedRunning(configA());
    if (t.setup) t.setup(r);
    {
        Applier a = applier(r);
        const Result res = t.refused(r, a);
        QVERIFY2(res.outcome == t.refusedOutcome, qPrintable(enumName(res.outcome) + QLatin1Char(' ') + res.message));
        QVERIFY2(res.message.contains(QString::fromLatin1(t.advice)), qPrintable(res.message));
    }
    if (t.between) t.between(r);
    {
        Applier a = applier(r);
        const Result res = t.recovery(r, a);
        QVERIFY2(res.outcome == Outcome::Ok,
                 qPrintable(QStringLiteral("recovery: %1 %2\n%3").arg(enumName(res.outcome), res.message, events(r))));
    }
    {
        Applier a = applier(r);
        const Result res = t.again(r, a);
        QVERIFY2(res.outcome == Outcome::Ok,
                 qPrintable(QStringLiteral("again: %1 %2\n%3").arg(enumName(res.outcome), res.message, events(r))));
    }
    QVERIFY2(r.world.violations.isEmpty(), qPrintable(r.world.violations.join(QLatin1Char(','))));
}

// ---------------------------------------------------------------------------------------------------
// WS-5 round 3: Repair and --force-recreate change the data folder only once the engine is proven
// stopped. With engine.lock missing (an old engine may hold its unlinked inode) or an insecure lock file, no
// hold can prove it: the engine-free proof comes first, and while a process survives its stop request
// neither the rename aside nor the create happens. Once it is gone, the same operation succeeds.
// Round 4 adds the third way: B3 ACQUIRES an engine.lock, but it is a replacement whose id is
// not the recorded one, while the old engine survives on the unlinked inode it holds. A tolerated mismatched
// hold proves nothing, so the engine-free proof decides, and the cleanup never calls that stop verified.
void tst_Applier::folderRepairWaitsForTheEngineFreeProof_data()
{
    QTest::addColumn<bool>("forceRecreate");
    QTest::addColumn<int>("lock");   // 0 missing, 1 an insecure file, 2 acquired but not the recorded one
    QTest::newRow("Repair, engine.lock missing") << false << 0;
    QTest::newRow("Repair, engine.lock an insecure file") << false << 1;
    QTest::newRow("Repair, engine.lock acquired but mismatched") << false << 2;
    QTest::newRow("--install --force-recreate, engine.lock missing") << true << 0;
    QTest::newRow("--install --force-recreate, engine.lock an insecure file") << true << 1;
    QTest::newRow("--install --force-recreate, engine.lock acquired but mismatched") << true << 2;
}

void tst_Applier::folderRepairWaitsForTheEngineFreeProof()
{
    QFETCH(bool, forceRecreate);
    QFETCH(int, lock);
    Rig r;
    r.installedRunning(configA());
    r.world.files.plantFile(QStringLiteral("planted.txt"), "x");   // untrusted: Repair would move it aside
    if (lock == 1) {
        r.engineLock.plantInsecureLock(r.engineLock.lockContent());
    } else if (lock == 0) {
        r.world.lockFiles.eraseEntry(QStringLiteral("engine.lock"));   // the old engine keeps the unlinked inode
    } else {   // a replacement engine.lock, free to acquire; the old engine keeps the unlinked inode
        r.engineLock.plantLock(LockIdentity::format(QByteArray(32, 'c')));
        r.service.engineHoldsLock = false;
    }
    r.service.neverReleases = true;   // ... and outlives its stop request
    auto runIt = [&] { return forceRecreate ? applier(r).run(request(configB()), true, true) : applier(r).repair(); };
    const Result res = runIt();
    QCOMPARE(enumName(res.outcome), enumName(Outcome::StateUnknown));
    QVERIFY2(res.message.contains(QStringLiteral("Could not prove that no engine runs")), qPrintable(res.message));
    // The cleanup reports what it proved: never a verified stop while the old engine survives.
    QVERIFY2(!res.postcondition.contains(QString::fromLatin1(kVerified)), qPrintable(res.postcondition));
    QVERIFY2(!res.message.contains(QStringLiteral("verified")), qPrintable(res.message));
    QVERIFY2(!r.world.log.contains(QStringLiteral("folder:renameAside")), qPrintable(events(r)));
    QVERIFY(!r.world.log.contains(QStringLiteral("folder:create")));
    QVERIFY(!r.world.log.contains(QStringLiteral("service:installOrConfigure")));
    QVERIFY(!r.world.log.contains(QStringLiteral("lockio:")));
    QCOMPARE(r.file(kConfig), configA());   // the live folder is where it was, whole
    QVERIFY(r.has(QStringLiteral("planted.txt")));
    QVERIFY(r.folder.aside.isEmpty());
    QVERIFY(r.service.startsBlocked);
    QVERIFY(res.fileChanges.isEmpty());
    QVERIFY(!r.engineLock.appliersHold());

    // The old process exits: the same operation now moves the folder aside and recovers the lock pair.
    r.service.neverReleases = false;
    r.service.engineHoldsLock = false;
    r.service.pid = 0;
    const Result again = runIt();
    QVERIFY2(again.outcome == Outcome::Ok, qPrintable(again.message));
    QCOMPARE(r.folder.aside.value(kConfig), configA());
    QVERIFY(r.world.log.indexOf(QStringLiteral("service:query")) < r.world.log.indexOf(QStringLiteral("folder:renameAside")));
}

// WS-5 round 3: B3 acquires engine.lock in time, but the identity read through the hold returns
// after the barrier's deadline. That is no proof: the barrier ends as B4 (Deadline), keeping its effects
// named, and nothing from R3 on runs: no staging, no firewall, no commit, no restart.
void tst_Applier::identityReadAfterTheBarrierDeadline_data()
{
    QTest::addColumn<QString>("op");
    QTest::addColumn<QString>("which");
    for (const char* op : {"Run", "Start", "Stop", "--install"})
        for (const char* which : {"held", "record"})
            QTest::newRow(qPrintable(QStringLiteral("%1, %2 read late").arg(QLatin1String(op), QLatin1String(which))))
                << QString::fromLatin1(op) << QString::fromLatin1(which);
}

void tst_Applier::identityReadAfterTheBarrierDeadline()
{
    QFETCH(QString, op);
    QFETCH(QString, which);
    Rig r;
    r.installedRunning(configA());
    // B3's reads, made while the Applier holds engine.lock (the prologue's record read is not B3's).
    auto done = std::make_shared<bool>(false);
    r.engineLock.onRead = [&r, done, which](const QString& w) {
        if (*done || !r.engineLock.appliersHold() || w != which) return;
        *done = true;
        r.world.clock.advance(std::chrono::seconds(31));
    };
    Applier a = applier(r);
    const Result res = op == QLatin1String("Run")     ? a.run(request(configB()), false)
                       : op == QLatin1String("Start") ? a.start()
                       : op == QLatin1String("Stop")  ? a.stop()
                                                      : a.run(request(configB()), true);
    QCOMPARE(enumName(res.outcome), enumName(Outcome::StateUnknown));
    QCOMPARE(res.step, QStringLiteral("prove the bridge stopped"));
    QVERIFY2(res.message.contains(QStringLiteral("Could not prove the bridge stopped within 30 s")), qPrintable(res.message));
    QCOMPARE(enumName(res.persisted), enumName(Persisted::None));
    for (const char* never : {"service:installOrConfigure", "io:commitAs config.pending.json", "firewall:", "io:commitAs config.json",
                              "service:allowStarts", "service:start"})
        QVERIFY2(!r.world.log.contains(QString::fromLatin1(never)), qPrintable(QString::fromLatin1(never) + QLatin1Char('\n') + events(r)));
    QCOMPARE(r.file(kConfig), configA());
    QVERIFY(!r.has(kPending));
    QVERIFY(!r.service.engineRunning);
    QVERIFY(!r.engineLock.appliersHold());
    if (op != QLatin1String("Stop")) QVERIFY(r.service.startsBlocked);   // failClosed (Stop keeps the start type)
    // The late hold is no proof, but the cleanup's own B3 is: it acquires again, reads a matching id and says
    // so (WS-5 round 6: the B4 end runs the cleanup like any late call).
    QVERIFY2(res.postcondition.contains(QString::fromLatin1(kVerified)), qPrintable(res.postcondition));
}

// WS-5 round 3: the engine-lock waits of lock recovery are bounded by the phase that runs them, and an
// acquisition that returns late is judged before the next step. (a) the re-acquire after publishing returns
// late: nothing from R4 on. (b) a fresh install over a leftover lock an orphan holds: the wait ends with the
// Install phase, not 30 s after it began, so the pair is never published past the deadline.
void tst_Applier::recoveryLockWaitsAreJudgedAgainstTheirPhase_data()
{
    QTest::addColumn<int>("row");
    QTest::newRow("re-acquire after publication returns late") << 0;
    QTest::newRow("judging a leftover pair: the wait ends with the Install phase") << 1;
    QTest::newRow("Repair: the identity re-check after publication returns late") << 2;
}

void tst_Applier::recoveryLockWaitsAreJudgedAgainstTheirPhase()
{
    QFETCH(int, row);
    Rig r;
    if (row == 0) {
        r.installedRunning(configA());
        r.engineLock.plantRecord(LockIdentity::format(QByteArray(32, 'b')));   // mismatched: install recovers it
        auto acquires = std::make_shared<int>(0);
        r.world.log.onAdd = [&r, acquires](const QString& e) {
            if (e == QLatin1String("engineLock:acquireUntil") && ++*acquires == 2)   // B3, then the re-acquire
                r.world.clock.advance(std::chrono::seconds(61));
        };
        const Result res = applier(r).run(request(configB()), true);
        r.world.log.onAdd = {};
        QCOMPARE(enumName(res.outcome), enumName(Outcome::StateUnknown));
        QVERIFY2(res.message.contains(QStringLiteral("A call while installing the service returned after its deadline")),
                 qPrintable(res.message));
        QVERIFY(r.world.log.contains(QStringLiteral("lockio:commitAs engine.lock.id")));   // published before it
        // Judged at once: nothing is read through the late hold. (failClosed then releases that unvalidated hold
        // and its own B3 acquires and judges the lock afresh, round 4: the only identity read after it.)
        const int reacquire = r.world.log.indexOf(QStringLiteral("engineLock:acquireUntil"),
                                                  r.world.log.indexOf(QStringLiteral("lockio:commitAs engine.lock.id")));
        QVERIFY(reacquire > 0);
        const int read = r.world.log.indexOf(QStringLiteral("engineLock:readHeld"), reacquire);
        const int cleanupAcquire = r.world.log.indexOf(QStringLiteral("engineLock:acquireUntil"), reacquire + 1);
        QVERIFY2(read < 0 || (cleanupAcquire > reacquire && read > cleanupAcquire), qPrintable(events(r)));
        QVERIFY2(!r.world.log.contains(QStringLiteral("io:commitAs config.pending.json")), qPrintable(events(r)));
        QVERIFY(!r.world.log.contains(QStringLiteral("firewall:sync")));
        QVERIFY(res.fileChanges.join(QLatin1Char(' ')).contains(QStringLiteral("engine.lock published")));
    } else if (row == 1) {
        r.world.files.plantFile(kConfig, configA(), QStringLiteral("root"));
        r.engineLock.windows();
        r.engineLock.plantLock(LockIdentity::format(QByteArray(32, 'c')));   // a leftover lock, no record
        r.service.installation.state = InstallState::NotInstalled;
        r.service.engineHoldsLock = true;      // an orphan holds it, and lets go after 15 s: inside a fresh 30 s
        r.service.stopRequested = true;        // budget, but after the Install phase's 60 s
        r.service.releaseAfterPolls = 150;
        r.service.onCall = [&r](const QString& m) {
            if (m == QLatin1String("installOrConfigure")) r.world.clock.advance(std::chrono::seconds(50));
        };
        const Result res = applier(r).run(request(configB()), true);
        QVERIFY2(res.outcome == Outcome::StateUnknown, qPrintable(res.message));
        // The wait ended with its phase: the lock was not acquired late, it was not acquired.
        QVERIFY2(res.message.contains(QStringLiteral("Could not acquire the engine lock")), qPrintable(res.message));
        QVERIFY2(!r.world.log.contains(QStringLiteral("lockio:commitAs")), qPrintable(events(r)));
        QVERIFY(!r.world.log.contains(QStringLiteral("io:commitAs config.pending.json")));
        QVERIFY(!r.engineLock.appliersHold());
    } else {
        // Repair ends with the recovery: its last identity read is judged, or Repair would report Ok late.
        r.installedRunning(configA());
        r.engineLock.plantRecord(LockIdentity::format(QByteArray(32, 'b')));
        auto published = std::make_shared<bool>(false);
        r.world.log.onAdd = [&r, published](const QString& e) {
            if (e.startsWith(QLatin1String("lockio:committed engine.lock.id"))) *published = true;
            if (*published && e == QLatin1String("engineLock:readHeld")) {
                *published = false;
                r.world.clock.advance(std::chrono::seconds(61));
            }
        };
        const Result res = applier(r).repair();
        r.world.log.onAdd = {};
        QCOMPARE(enumName(res.outcome), enumName(Outcome::StateUnknown));
        QVERIFY2(res.message.contains(QStringLiteral("returned after its deadline")), qPrintable(res.message));
        QVERIFY(r.service.startsBlocked);
    }
    QVERIFY2(r.world.violations.isEmpty(), qPrintable(r.world.violations.join(QLatin1Char(','))));
}

// WS-5 round 4: every firewall call of every operation comes after that operation's own connect
// (Windows: COM setup and ConnectServer), on a host where nothing connected before. FakeFirewall fails a call
// made without the connection, as the native adapter would.
void tst_Applier::firewallConnectedBeforeEveryFirewallStep_data()
{
    QTest::addColumn<int>("op");   // 0 Run, 1 Start, 2 SaveAndRemove, 3 SaveAndRemove without N
    QTest::newRow("Run") << 0;
    QTest::newRow("Start") << 1;
    QTest::newRow("SaveAndRemove") << 2;
    QTest::newRow("SaveAndRemove without N") << 3;
}

void tst_Applier::firewallConnectedBeforeEveryFirewallStep()
{
    QFETCH(int, op);
    Rig r;
    r.installedRunning(configA());
    QVERIFY(!r.firewall.connected);
    Result res;
    switch (op) {
    case 0: res = applier(r).run(request(configB()), false); break;
    case 1: res = applier(r).start(); break;
    case 2: res = applier(r).saveAndRemove(request(configB())); break;
    default: res = applier(r).saveAndRemove(std::nullopt); break;
    }
    QVERIFY2(res.outcome == Outcome::Ok || res.outcome == Outcome::SavedNotApplied,
             qPrintable(res.message + QLatin1Char('\n') + events(r)));
    const QStringList all = r.world.log.all();
    const int connect = at(r, QStringLiteral("firewall:connect"));
    QVERIFY(connect >= 0);
    QCOMPARE(int(all.count(QStringLiteral("firewall:connect"))), 1);
    for (int i = 0; i < connect; ++i)
        QVERIFY2(!all[i].startsWith(QStringLiteral("firewall:")),
                 qPrintable(all[i] + QStringLiteral(" before firewall:connect\n") + events(r)));
    if (op >= 2) {
        QVERIFY(r.firewall.rules.isEmpty());
        QCOMPARE(r.service.installation.state, InstallState::NotInstalled);
    }
}

// WS-5 round 4: a cleanup that finds no engine.lock has no proof that a running engine stopped. An engine-free
// proof taken earlier in the operation is not that proof: here --install proves it over a missing lock, then
// the failed install starts an engine that outlives the cleanup's stop request.
void tst_Applier::cleanupWithoutALockNeverCallsTheStopVerified()
{
    Rig r;
    r.installedRunning(configA());
    r.world.lockFiles.eraseEntry(QStringLiteral("engine.lock"));   // the engine stops; the proof succeeds
    r.service.partialInstallRunning = true;                          // R3 starts an engine, then throws
    r.service.onCall = [&r](const QString& m) {
        if (m == QLatin1String("installOrConfigure")) r.service.neverReleases = true;   // ... that survives its stop
    };
    const Result res = applier(r).run(request(configB()), true);
    QVERIFY(r.world.log.contains(QStringLiteral("service:installOrConfigure")));
    QVERIFY(r.service.stopRequested && r.service.neverReleases);   // an engine process survives
    QVERIFY2(!res.postcondition.contains(QStringLiteral("verified")), qPrintable(res.postcondition));
    QVERIFY2(res.postcondition.contains(QStringLiteral("not proven stopped")), qPrintable(res.postcondition));
    QVERIFY2(!res.message.contains(QStringLiteral("verified")), qPrintable(res.message));
}

// WS-5 round 6: an early barrier call (B0 revoke, B1 block starts, B2 request stop) returns after the
// barrier's 30 s deadline, possibly before the stop was requested. The barrier ends at B4, and the operation
// stops through the one handler of every late call: the cleanup requests the stop and reports what it proved.
// Here the cleanup's stop succeeds, so it is verified; Stop's cleanup keeps the start type. Nothing from R3 on
// runs.
void tst_Applier::earlyBarrierCallReturnsLate_data()
{
    QTest::addColumn<QString>("op");
    QTest::addColumn<QString>("call");
    for (const char* op : {"Run", "Start", "Stop", "Deactivate", "SaveAndRemove", "Repair", "--install"})
        for (const char* call : {"io:openExisting start.token", "service:inhibitStarts", "service:requestStop"}) {
            if (QLatin1String(op) == QLatin1String("Stop") && QLatin1String(call) == QLatin1String("service:inhibitStarts"))
                continue;   // Stop does not block starts
            QTest::newRow(qPrintable(QStringLiteral("%1, %2 late").arg(QLatin1String(op), QLatin1String(call))))
                << QString::fromLatin1(op) << QString::fromLatin1(call);
        }
}

void tst_Applier::earlyBarrierCallReturnsLate()
{
    QFETCH(QString, op);
    QFETCH(QString, call);
    Rig r;
    r.installedRunning(configA());
    auto late = std::make_shared<bool>(false);
    r.world.log.onAdd = [&r, call, late](const QString& e) {
        if (*late || !e.startsWith(call)) return;
        *late = true;   // the first such call returns after the barrier's deadline; the cleanup's are on time
        r.world.clock.advance(std::chrono::seconds(31));
    };
    Applier a = applier(r);
    const Result res = op == QLatin1String("Run")             ? a.run(request(configB()), false)
                       : op == QLatin1String("Start")         ? a.start()
                       : op == QLatin1String("Stop")          ? a.stop()
                       : op == QLatin1String("Deactivate")    ? a.deactivate(std::nullopt)
                       : op == QLatin1String("SaveAndRemove") ? a.saveAndRemove(std::nullopt)
                       : op == QLatin1String("Repair")        ? a.repair()
                                                              : a.run(request(configB()), true);
    r.world.log.onAdd = {};
    const QString context = QStringLiteral("%1: %2\npostcondition: %3\n%4")
                                .arg(enumName(res.outcome), res.message, res.postcondition, events(r));
    QVERIFY2(*late, qPrintable(context));
    QCOMPARE(enumName(res.outcome), enumName(Outcome::StateUnknown));
    QCOMPARE(res.step, QStringLiteral("prove the bridge stopped"));
    QVERIFY2(res.message.contains(QStringLiteral("Could not prove the bridge stopped within 30 s (a call returned after its "
                                                 "deadline)")),
             qPrintable(context));
    // The cleanup requested the stop and proved it ...
    QVERIFY2(r.service.stopRequested, qPrintable(context));
    QVERIFY(!r.service.engineRunning && !r.service.engineHoldsLock);
    if (op == QLatin1String("Stop")) {
        QCOMPARE(res.postcondition, QStringLiteral("stopped (verified: engine lock acquired); start type unchanged"));
        QVERIFY(!r.service.startsBlocked);
    } else {
        QCOMPARE(res.postcondition, QStringLiteral("stopped and disabled (verified: engine lock acquired)"));
        QVERIFY(r.service.startsBlocked);
    }
    // ... and nothing after the barrier ran.
    QCOMPARE(enumName(res.persisted), enumName(Persisted::None));
    QCOMPARE(r.file(kConfig), configA());
    QVERIFY(!r.has(kPending));
    for (const char* never : {"service:installOrConfigure", "firewall:", "io:commitAs config", "folder:renameAside",
                              "service:allowStarts", "service:start", "service:uninstall", "lockio:"})
        QVERIFY2(!r.world.log.contains(QString::fromLatin1(never)), qPrintable(QString::fromLatin1(never) + QLatin1Char('\n') + context));
    QVERIFY(!r.engineLock.appliersHold());
    QVERIFY(!r.world.applyLock.held());
}

// WS-5 round 6: engine.lock is replaced between the prologue (which found the pair matching) and B3,
// while the old engine survives its stop request on the unlinked inode it holds. B3 acquires the new file and
// rejects its identity. The stop was requested but nothing proves it: neither the postcondition nor the message,
// nor the untick's service line, may claim it. The barrier's effects and the advice are kept.
void tst_Applier::lockRefusedNeverClaimsAStop_data()
{
    QTest::addColumn<QString>("op");
    for (const char* op : {"Run", "Start", "Stop", "Deactivate", "Deactivate with N", "SaveAndRemove", "SaveAndRemove with N"})
        QTest::newRow(op) << QString::fromLatin1(op);
}

void tst_Applier::lockRefusedNeverClaimsAStop()
{
    QFETCH(QString, op);
    Rig r;
    r.installedRunning(configA());
    auto replaced = std::make_shared<bool>(false);
    r.service.onCall = [&r, replaced](const QString& m) {
        if (*replaced || m != QLatin1String("requestStop")) return;
        *replaced = true;
        r.engineLock.plantLock(LockIdentity::format(QByteArray(32, 'c')));
        r.service.engineHoldsLock = false;   // the old engine holds the unlinked inode, not this file
        r.service.neverReleases = true;
    };
    Applier a = applier(r);
    const bool withN = op.endsWith(QLatin1String("with N"));
    const std::optional<SaveRequest> n = withN ? std::optional<SaveRequest>(request(configB())) : std::nullopt;
    const Result res = op == QLatin1String("Run")           ? a.run(request(configB()), false)
                       : op == QLatin1String("Start")       ? a.start()
                       : op == QLatin1String("Stop")        ? a.stop()
                       : op.startsWith(QLatin1String("Deactivate")) ? a.deactivate(n)
                                                                    : a.saveAndRemove(n);
    const QString context = QStringLiteral("%1: %2\npostcondition: %3\n%4")
                                .arg(enumName(res.outcome), res.message, res.postcondition, events(r));
    QVERIFY2(*replaced && r.service.stopRequested && r.service.neverReleases, qPrintable(context));   // it survives
    QVERIFY2(!claimsStop(res.postcondition), qPrintable(context));
    QVERIFY2(!claimsStop(res.message), qPrintable(context));
    QVERIFY2(res.postcondition.contains(QString::fromLatin1(kUnproven)), qPrintable(context));
    QVERIFY2(res.postcondition.contains(QStringLiteral("stop requested")), qPrintable(context));
    if (withN) {
        QCOMPARE(enumName(res.outcome), enumName(Outcome::SavedNotApplied));
        QVERIFY2(res.message.contains(QStringLiteral("Service: could not prove the bridge stopped")), qPrintable(context));
    } else {
        QCOMPARE(enumName(res.outcome), enumName(Outcome::StateUnknown));
        QVERIFY2(res.message.contains(QStringLiteral("Run --install or Repair")), qPrintable(context));
    }
    for (const char* never : {"firewall:", "service:uninstall", "service:allowStarts", "service:start", "io:commitAs config.json"})
        QVERIFY2(!r.world.log.contains(QString::fromLatin1(never)), qPrintable(QString::fromLatin1(never) + QLatin1Char('\n') + context));
}

// ---------------------------------------------------------------------------------------------------
// WS-16: uninstall (INV-125, INV-126, D69)
namespace {

// A station as uninstall finds it: running, its program installed, the Apps & features entry written.
void station(Rig& r, const QByteArray& committed = configA())
{
    r.installedRunning(committed);
    r.service.entry = {true, r.service.installedExePath, r.service.version};
}

QString changes(const Result& res)
{
    return res.fileChanges.join(QLatin1Char('\n'));
}

}  // namespace

// The whole Windows uninstall: rules, service, the lock files marked and gone with their holds, the program, the
// record, the entry, then the empty folder. The settings stay.
void tst_Applier::uninstallRemovesServiceProgramLocksAndEntry()
{
    Rig r;
    station(r);
    const Result res = applier(r).uninstall({});
    QVERIFY2(res.outcome == Outcome::Ok, qPrintable(res.message + QLatin1Char('\n') + events(r)));
    QCOMPARE(r.service.installation.state, InstallState::NotInstalled);
    QVERIFY(r.firewall.rules.isEmpty());
    QVERIFY(!r.service.programPresent);
    QVERIFY(!r.service.entry.present);
    QVERIFY(!r.world.lockFiles.exists(QStringLiteral("engine.lock")));
    QVERIFY(!r.world.lockFiles.exists(QStringLiteral("engine.lock.id")));
    QVERIFY(!r.world.applyLock.present);
    QVERIFY2(!r.service.folderPresent, qPrintable(changes(res)));
    QVERIFY2(res.fileChanges.contains(QStringLiteral(R"(removed C:\Program Files\AetherSerialBridge)")), qPrintable(changes(res)));
    QVERIFY(r.has(kConfig));
    // The lock files are marked before the program goes (D69), and the entry is the last thing removed under the lock.
    QVERIFY(at(r, QStringLiteral("service:uninstall")) < at(r, QStringLiteral("applyLock:markForDeletion")));
    QVERIFY(at(r, QStringLiteral("lockio:markForDeletion engine.lock")) < at(r, QStringLiteral("service:removeProgram")));
    QVERIFY(at(r, QStringLiteral("service:removeProgram")) < at(r, QStringLiteral("lockio:remove engine.lock.id")));
    QVERIFY(at(r, QStringLiteral("lockio:remove engine.lock.id")) < at(r, QStringLiteral("service:removeAppsEntry")));
    QVERIFY(at(r, QStringLiteral("service:removeAppsEntry")) < at(r, QStringLiteral("applyLock:deleted")));
    QVERIFY(at(r, QStringLiteral("applyLock:deleted")) < at(r, QStringLiteral("service:removeProgramFolder")));
    QVERIFY2(r.world.violations.isEmpty(), qPrintable(r.world.violations.join(QLatin1Char(','))));
    QVERIFY(!r.world.applyLock.held());
}

// INV-125: without removeSettings nothing in the data folder is deleted. Kills inv125-settings-removed-by-default.
void tst_Applier::uninstallKeepsSettingsByDefault()
{
    Rig r;
    station(r);
    r.world.files.plantFile(kPending, configB(), QStringLiteral("root"));
    const Result res = applier(r).uninstall({});
    QCOMPARE(res.outcome, Outcome::Ok);
    QCOMPARE(r.file(kConfig), configA());
    QCOMPARE(r.file(kPending), configB());
    QVERIFY(!r.world.log.contains(QStringLiteral("io:remove config")));
    QVERIFY(!r.world.log.contains(QStringLiteral("folder:removeIfEmpty")));
    QVERIFY(!res.message.contains(QStringLiteral("The settings were removed")));
}

// INV-125: every known file that exists is deleted through the folder's handle, then the empty folder.
void tst_Applier::uninstallRemovesKnownSettingsWhenAsked()
{
    Rig r;
    station(r);
    r.world.files.plantFile(kPending, configB(), QStringLiteral("root"));
    r.world.files.plantFile(QStringLiteral("status.json"), "{}", QStringLiteral("root"));
    r.world.files.plantFile(QStringLiteral("bridge.log"), "x", QStringLiteral("root"));
    r.world.files.plantFile(QStringLiteral("bridge.log.1"), "x", QStringLiteral("root"));
    const Result res = applier(r).uninstall({/*removeSettings=*/true});
    QVERIFY2(res.outcome == Outcome::Ok, qPrintable(res.message));
    QVERIFY2(r.world.files.names().isEmpty(), qPrintable(r.world.files.names().join(QLatin1Char(','))));
    QCOMPARE(r.folder.state, FakeFolder::DirState::Missing);
    QVERIFY2(res.message.contains(QStringLiteral("The settings were removed.")), qPrintable(res.message));
    for (const FileId& id : r.io.dirIds) QCOMPARE(id, r.folder.handle().id());
    QVERIFY(res.fileChanges.contains(QStringLiteral("removed config.json")));
    QVERIFY(res.fileChanges.contains(QStringLiteral("removed the data folder")));
}

// INV-125: anything that is not a known file stays, and so does the folder. Kills inv125-remove-whole-folder.
void tst_Applier::uninstallKeepsUnknownEntries()
{
    Rig r;
    station(r);
    r.world.files.plantFile(QStringLiteral("notes.txt"), "mine", QStringLiteral("root"));
    const Result res = applier(r).uninstall({true});
    QCOMPARE(res.outcome, Outcome::Ok);
    QVERIFY(!r.has(kConfig));
    QCOMPARE(r.file(QStringLiteral("notes.txt")), QByteArray("mine"));
    QCOMPARE(r.folder.state, FakeFolder::DirState::Directory);
    QVERIFY2(res.fileChanges.contains(QStringLiteral("kept notes.txt (not a file this program writes)")), qPrintable(changes(res)));
    QVERIFY2(res.message.contains(QStringLiteral("kept because it holds other files: notes.txt")), qPrintable(res.message));
    QVERIFY(!r.world.log.contains(QStringLiteral("folder:removeIfEmpty")));
}

// INV-125: an untrusted folder is touched not at all, with or without removeSettings (no FileIo call in it), and the
// result says so. Kills inv125-untrusted-folder-cleared.
void tst_Applier::uninstallUntrustedSettingsUntouched()
{
    Rig r;
    station(r);
    r.world.files.plantFile(QStringLiteral("planted.txt"), "x");   // untrusted child
    const Result res = applier(r).uninstall({true});
    QVERIFY2(res.outcome == Outcome::Ok, qPrintable(res.message));
    QVERIFY2(r.io.dirIds.isEmpty(), qPrintable(events(r)));
    QCOMPARE(r.file(kConfig), configA());
    QVERIFY(r.has(QStringLiteral("planted.txt")));
    QVERIFY(r.has(kToken));
    QVERIFY2(res.message.contains(QStringLiteral("not trusted")) && res.message.contains(QStringLiteral("nothing in it was touched")),
             qPrintable(res.message));
    QVERIFY(!r.service.programPresent);   // the rest of the uninstall still happened
}

// INV-125: the settings go only after the service and the program are gone. Kills inv125-settings-before-service.
void tst_Applier::uninstallSettingsOnlyAfterServiceAndProgramGone()
{
    Rig r;
    station(r);
    const Result res = applier(r).uninstall({true});
    QCOMPARE(res.outcome, Outcome::Ok);
    const int settings = at(r, QStringLiteral("io:remove config.json"));
    QVERIFY(settings > 0);
    QVERIFY(at(r, QStringLiteral("firewall:removeAll")) < settings);
    QVERIFY(at(r, QStringLiteral("service:uninstall")) < settings);
    QVERIFY(at(r, QStringLiteral("service:removeProgram")) < settings);
    QVERIFY(settings < at(r, QStringLiteral("service:removeAppsEntry")));
}

// INV-125: a failure before the settings step (rules, service, lock disposition, program) touches no setting.
void tst_Applier::uninstallFailureBeforeSettingsTouchesNoSettings()
{
    const QList<std::pair<const char*, std::function<void(Rig&)>>> faults{
        {"the rules", [](Rig& r) { r.firewall.fail[QStringLiteral("removeAll")] = QStringLiteral("RPC failure"); }},
        {"the service", [](Rig& r) { r.service.fail[QStringLiteral("uninstall")] = QStringLiteral("access denied"); }},
        {"the lock disposition", [](Rig& r) { r.world.applyLock.failMark = QStringLiteral("not supported"); }},
        {"the program", [](Rig& r) { r.service.fail[QStringLiteral("removeProgram!")] = QStringLiteral("in use"); }},
    };
    for (const auto& [name, fault] : faults) {
        Rig r;
        station(r);
        fault(r);
        const Result res = applier(r).uninstall({true});
        QVERIFY2(res.outcome != Outcome::Ok, name);
        QCOMPARE(r.file(kConfig), configA());
        QVERIFY2(!r.world.log.contains(QStringLiteral("io:remove config")), name);
        QVERIFY2(r.service.entry.present, name);   // and the entry stays with the program
        QVERIFY2(r.service.entryNamesExistingProgram(), name);
    }
}

// INV-125: the result lists every removal and everything kept: an unknown file, a folder Repair moved aside, a folder
// that still holds something.
void tst_Applier::uninstallListsEverythingKept()
{
    Rig r;
    station(r);
    r.world.files.plantFile(QStringLiteral("notes.txt"), "mine", QStringLiteral("root"));
    r.folder.asides << QStringLiteral(R"(C:\ProgramData\AetherSerialBridge.untrusted-20261001T000000Z-1a2b3c4d)");
    r.service.folderOthers << QStringLiteral("readme.txt");
    const Result res = applier(r).uninstall({true});
    QCOMPARE(res.outcome, Outcome::Ok);
    const QString all = changes(res);
    QVERIFY2(all.contains(QStringLiteral("removed config.json")), qPrintable(all));
    QVERIFY2(all.contains(QStringLiteral("kept notes.txt")), qPrintable(all));
    QVERIFY2(all.contains(QStringLiteral("AetherSerialBridge.untrusted-20261001T000000Z-1a2b3c4d (moved aside by Repair; never removed)")), qPrintable(all));
    QVERIFY2(all.contains(QStringLiteral(R"(kept C:\Program Files\AetherSerialBridge (it still holds readme.txt))")), qPrintable(all));
    QVERIFY2(all.contains(QStringLiteral(R"(removed C:\Program Files\AetherSerialBridge\aether-serial-bridge.exe)")), qPrintable(all));
    QVERIFY2(all.contains(QStringLiteral("removed the Apps & features entry")), qPrintable(all));
}

// INV-126, D69: neither lock can be marked: stop there. The service and the rules are gone; the program, the lock
// files and the entry are kept, the settings are not touched even with removeSettings, and the result says how to
// finish. Kills inv126-program-removed-before-lock-cleanup.
void tst_Applier::uninstallLockDispositionFailureKeepsProgramAndEntry()
{
    for (const bool engineFails : {false, true}) {
        Rig r;
        station(r);
        if (engineFails) r.engineLock.failMark = QStringLiteral("not supported");
        else r.world.applyLock.failMark = QStringLiteral("not supported");
        const Result res = applier(r).uninstall({true});
        QCOMPARE(res.outcome, Outcome::Uncommitted);
        QCOMPARE(r.service.installation.state, InstallState::NotInstalled);
        QVERIFY(r.firewall.rules.isEmpty());
        QVERIFY(r.service.programPresent);
        QVERIFY(r.service.entry.present);
        QVERIFY(r.world.lockFiles.exists(QStringLiteral("engine.lock")));
        QVERIFY(r.world.lockFiles.exists(QStringLiteral("engine.lock.id")));
        QVERIFY(r.world.applyLock.present);   // never one of them marked: the apply mark is cleared when engine's fails
        QCOMPARE(r.file(kConfig), configA());
        QVERIFY(!r.world.log.contains(QStringLiteral("service:removeProgram")));
        QVERIFY(!r.world.log.contains(QStringLiteral("service:removeAppsEntry")));
        QVERIFY2(res.message.startsWith(QStringLiteral("The service and its firewall rules were removed. The lock files in "
                                                       R"(C:\Program Files\AetherSerialBridge could not be marked for removal )"
                                                       "(not supported), so the program and its Apps & features entry were "
                                                       "kept: uninstall again to finish.")),
                 qPrintable(res.message));
    }
}

// INV-126: the entry's retry (the service already gone, so detect says NotInstalled) finishes the job: the lock files
// are taken and marked, the program, the record, the entry and the folder go.
void tst_Applier::uninstallRetryFromNotInstalledFinishes()
{
    Rig r;
    station(r);
    r.world.applyLock.failMark = QStringLiteral("not supported");
    QCOMPARE(applier(r).uninstall({}).outcome, Outcome::Uncommitted);
    QCOMPARE(r.service.installation.state, InstallState::NotInstalled);
    r.world.applyLock.failMark.reset();
    r.world.log.clear();
    const Result res = applier(r).uninstall({});
    QVERIFY2(res.outcome == Outcome::Ok, qPrintable(res.message + QLatin1Char('\n') + events(r)));
    QVERIFY(!r.service.programPresent);
    QVERIFY(!r.service.entry.present);
    QVERIFY(!r.world.lockFiles.exists(QStringLiteral("engine.lock")));
    QVERIFY(!r.world.lockFiles.exists(QStringLiteral("engine.lock.id")));
    QVERIFY(!r.service.folderPresent);
    QVERIFY(!r.world.log.contains(QStringLiteral("service:uninstall")));   // no service to remove
    QVERIFY2(r.world.violations.isEmpty(), qPrintable(r.world.violations.join(QLatin1Char(','))));
}

// INV-126, D69: the program is kept (in use): the dispositions are cleared again before any hold closes, so its lock
// files stay with it, and so do engine.lock.id and the entry. Kills inv126-disposition-kept-on-kept-program and
// inv126-entry-removed-first.
void tst_Applier::uninstallProgramKeptKeepsLockFiles()
{
    Rig r;
    station(r);
    r.service.fail[QStringLiteral("removeProgram!")] = QStringLiteral("in use");
    const Result res = applier(r).uninstall({true});
    QCOMPARE(res.outcome, Outcome::Uncommitted);
    QVERIFY(r.service.programPresent);
    QVERIFY(r.service.entry.present);
    QVERIFY(r.world.lockFiles.exists(QStringLiteral("engine.lock")));
    QVERIFY(r.world.lockFiles.exists(QStringLiteral("engine.lock.id")));
    QVERIFY(r.world.applyLock.present);
    QVERIFY(r.service.folderPresent);
    QVERIFY(at(r, QStringLiteral("service:removeProgram")) < at(r, QStringLiteral("applyLock:clearDeletion")));
    QVERIFY(at(r, QStringLiteral("lockio:clearDeletion engine.lock")) >= 0);
    QVERIFY2(res.message.contains(QStringLiteral("was kept (in use), with its lock files and its Apps & features entry")),
             qPrintable(res.message));
    QCOMPARE(r.file(kConfig), configA());
}

// The running image (Apps & features, or --uninstall typed in the program folder) is renamed aside and scheduled: it
// counts as removed, so the settings and the entry go, and the result says when it disappears.
void tst_Applier::uninstallRunningImageScheduledCountsAsRemoved()
{
    Rig r;
    station(r);
    r.service.programIsOwnImage = true;
    const Result res = applier(r).uninstall({true});
    QVERIFY2(res.outcome == Outcome::Ok, qPrintable(res.message));
    QVERIFY(r.service.programScheduled);
    QVERIFY(!r.service.entry.present);
    QVERIFY(!r.has(kConfig));
    QVERIFY2(res.message.contains(QStringLiteral("The program is removed when Windows restarts.")), qPrintable(res.message));
}

// Linux: the program goes with the unit (uninstall()); there is no lock disposition, no program step, no entry.
void tst_Applier::uninstallLinuxRemovesProgramWithTheUnit()
{
    Rig r;
    station(r);
    r.service.separateProgram = false;
    r.service.entry = {};
    r.folder.known = {QStringLiteral("config.json"), QStringLiteral("config.pending.json"), QStringLiteral("start.token")};
    r.world.files.plantFile(QStringLiteral("status.json"), "{}", QStringLiteral("root"));   // not a Linux settings file
    const Result res = applier(r).uninstall({true});
    QVERIFY2(res.outcome == Outcome::Ok, qPrintable(res.message));
    QVERIFY(!r.service.programPresent);
    QVERIFY(!r.world.log.contains(QStringLiteral("applyLock:markForDeletion")));
    QVERIFY(!r.world.log.contains(QStringLiteral("service:removeProgram")));
    QVERIFY(!r.world.log.contains(QStringLiteral("service:removeAppsEntry")));
    QVERIFY(!r.has(kConfig));
    QVERIFY(r.has(QStringLiteral("status.json")));
    QVERIFY2(res.message.startsWith(QStringLiteral("Uninstalled: the service and the program were removed.")), qPrintable(res.message));
}

// INV-126: a call that returns late after the program is gone stops the uninstall, but the entry still goes (cleanup
// step), so it never names a removed program.
void tst_Applier::uninstallLateAfterProgramGoneStillRemovesEntry()
{
    Rig r;
    station(r);
    r.world.log.onAdd = [&r](const QString& e) {
        if (e == QLatin1String("service:removeProgram")) r.world.clock.advance(std::chrono::seconds(400));
    };
    const Result res = applier(r).uninstall({true});
    r.world.log.onAdd = {};
    QVERIFY(res.outcome != Outcome::Ok);
    QVERIFY(!r.service.programPresent);
    QVERIFY2(!r.service.entry.present, qPrintable(res.message + QLatin1Char('\n') + events(r)));
    QVERIFY(r.has(kConfig));   // the settings step did not run
    QVERIFY2(res.message.contains(QStringLiteral("its Apps & features entry was removed")), qPrintable(res.message));
    // The program folder is tried once the holds are released; engine.lock.id (its step did not run) keeps it,
    // and the result names it.
    QVERIFY(r.world.log.contains(QStringLiteral("service:removeProgramFolder")));
    QVERIFY(r.service.folderPresent);
    QVERIFY2(res.fileChanges.join(QLatin1Char('|')).contains(QStringLiteral("it still holds engine.lock.id")),
             qPrintable(res.fileChanges.join(QLatin1Char('|'))));
}

// A seam that throws after the lock disposition (removeProgram's own deadline check, outside t.call) stops
// the uninstall the same way a late call does: the marks are cleared before any hold closes, so the kept program keeps
// its lock files and its entry (D69).
void tst_Applier::uninstallThrowingProgramRemovalKeepsLockFiles()
{
    Rig r;
    station(r);
    r.service.fail[QStringLiteral("removeProgram")] = QStringLiteral("could not reach the file system");
    const Result res = applier(r).uninstall({true});
    QVERIFY2(res.outcome != Outcome::Ok, qPrintable(res.message));
    QVERIFY2(res.message.contains(QStringLiteral("could not reach the file system"), Qt::CaseInsensitive), qPrintable(res.message));
    QVERIFY(r.service.programPresent);
    QVERIFY(r.service.entry.present);
    QVERIFY(r.world.lockFiles.exists(QStringLiteral("engine.lock")));
    QVERIFY(r.world.lockFiles.exists(QStringLiteral("engine.lock.id")));
    QVERIFY(r.world.applyLock.present);
    QVERIFY(r.service.folderPresent);
    QVERIFY(at(r, QStringLiteral("service:removeProgram")) < at(r, QStringLiteral("applyLock:clearDeletion")));
    QVERIFY(at(r, QStringLiteral("lockio:clearDeletion engine.lock")) >= 0);
    QVERIFY(!r.world.log.contains(QStringLiteral("service:removeAppsEntry")));
    QCOMPARE(r.file(kConfig), configA());
}

// A late call after the program and its lock record are gone (here the settings step) still removes the entry
// and, once the holds are released, the now empty program folder.
void tst_Applier::uninstallLateAfterRecordRemovesEmptyProgramFolder()
{
    Rig r;
    station(r);
    r.world.log.onAdd = [&r](const QString& e) {
        if (e == QLatin1String("folder:removeIfEmpty")) r.world.clock.advance(std::chrono::seconds(400));
    };
    const Result res = applier(r).uninstall({true});
    r.world.log.onAdd = {};
    QVERIFY(res.outcome != Outcome::Ok);
    QVERIFY(!r.service.programPresent);
    QVERIFY2(!r.service.entry.present, qPrintable(res.message + QLatin1Char('\n') + events(r)));
    QVERIFY2(!r.service.folderPresent, qPrintable(res.fileChanges.join(QLatin1Char('|')) + QLatin1Char('\n') + events(r)));
    QVERIFY2(res.fileChanges.contains(QStringLiteral("removed %1").arg(r.service.programFolder())),
             qPrintable(res.fileChanges.join(QLatin1Char('|'))));
}

QTEST_GUILESS_MAIN(tst_Applier)
#include "tst_Applier.moc"
