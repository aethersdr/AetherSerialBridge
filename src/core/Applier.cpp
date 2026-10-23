#include "core/Applier.h"

#include "core/AtomicJson.h"
#include "core/Barrier.h"
#include "core/ConfigFiles.h"
#include "core/ConfigJson.h"
#include "core/EngineClaim.h"
#include "core/GatedSeams.h"
#include "core/IrreversibleGate.h"
#include "core/LockIdentity.h"
#include "core/TrustedConfig.h"

#include <algorithm>
#include <exception>

namespace asb {
namespace {

using std::chrono::seconds;

// A native call returned after its phase deadline (D57): the operation stops here, failClosed runs.
struct Late {
    Phase phase;
};

QString what(const std::exception& e)
{
    return QString::fromLocal8Bit(e.what());
}

// P5's sweep: a temp of a file the Applier itself writes in the data folder (config.json,
// config.pending.json, start.token: "<base>.<32 hex>.asbtmp"). Any other temp there is not ours: the
// running engine writes status.json through its own temps (Runtime policy) and may be mid-write while
// P5 runs, before the barrier.
bool applierTemp(const QString& name)
{
    if (!isTempName(name)) return false;
    const qsizetype tail = 1 + 32 + qsizetype(qstrlen(kTempSuffix));   // ".<hex>.asbtmp"
    for (const char* base : {ConfigFiles::kConfig, ConfigFiles::kPending, ConfigFiles::kStartToken}) {
        const QString prefix = QString::fromLatin1(base) + QLatin1Char('.');
        if (name.startsWith(prefix) && name.size() == prefix.size() - 1 + tail) return true;
    }
    return false;
}

// Advice for the engine-lock states that neither --install nor Repair can change (they refuse them too).
constexpr char kLockDirectoryAdvice[] =
    "An administrator must restore the engine lock directory's owner and permissions to what the installer "
    "sets (--install and Repair do not change that directory), then retry.";
constexpr char kLockFileSystemAdvice[] =
    "The engine lock directory must be on a supported local file system; --install and Repair cannot move it.";

QString lockFailure(LockResult why)
{
    switch (why) {
    case LockResult::Busy: return QStringLiteral("engine.lock is still held");
    case LockResult::Missing: return QStringLiteral("engine.lock does not exist");
    case LockResult::InsecureFile: return QStringLiteral("engine.lock has the wrong owner or permissions");
    case LockResult::Insecure:
        return QStringLiteral("the engine lock directory is not secured. %1").arg(QString::fromLatin1(kLockDirectoryAdvice));
    case LockResult::Unsupported:
        return QStringLiteral("the lock directory is on an unsupported file system. %1")
            .arg(QString::fromLatin1(kLockFileSystemAdvice));
    case LockResult::Acquired: break;
    }
    return QStringLiteral("unknown");
}

// One operation's state. Members are destroyed in reverse order: engine.lock is released before
// apply.lock (master §4.6).
//
// Every irreversible step goes through `gate` (IrreversibleGate.h): it is admitted only when the step's
// declared proofs hold and its phase deadline has not passed, at the moment of the call, and it is made
// through `seams`, which needs the token the admission mints. The Txn holds no Platform of its own.
class Txn {
public:
    Txn(Platform& platform, IClock& c, PhaseClock* pc)
        : seams(platform), clock(c), phases(pc), gate(c, [this] { return proofs(); })
    {
        gate.onAdmitted = [this](Step s) {
            if (s == Step::InstallService) installAttempted = true;   // a service exists from here on
            if (s == Step::AllowStarts) engineFree = false;            // starts are no longer blocked
            publishEvidence();
        };
    }
    ~Txn()
    {
        engineHold.reset();
        if (phases) phases->clear();
    }

    GatedSeams seams;
    IClock& clock;
    PhaseClock* phases;
    IrreversibleGate gate;
    std::unique_ptr<LockHold> applyHold;
    std::unique_ptr<LockHold> engineHold;

    Installation inst;
    bool installed = false;
    bool installedNow = false;           // installOrConfigure ran in this operation (starts blocked)
    OpenedFolder opened;                 // the data folder as this operation opened it (P4 / Repair)
    bool trusted = false;                // the folder inspection said Trust: B0 may touch the token
    BarrierEffects fx;
    LockIdentity::PairState pair = LockIdentity::PairState::Matching;
    // How `pair` was decided. Authoritative once B3 (or recovery) judged it through the lock itself: read
    // through a held handle, or acquireUntil saw the file missing or insecure. Otherwise it comes from the
    // prologue's unheld read, which on Windows cannot see an existing engine.lock's bytes (Unreadable).
    bool pairAuthoritative = false;
    bool lockUnreadableUnheld = false;   // the prologue's peekLock said Unreadable
    bool lockFileInsecure = false;       // engine.lock failed its owner/mode check in a trusted directory
    bool barrierRan = false;             // B0 started: from here a refusal is a change, not NothingChanged
    bool detected = false;               // P2 answered: `installed` is the installation's state
    bool keepStartType = false;          // Stop: its cleanup requests the stop without blocking starts
    QStringList fileChanges;
    bool hasSubmission = false;          // a revision was submitted (request or promoted pending)
    QByteArray submittedSha;
    Persisted persisted = Persisted::None;
    QByteArray persistedSha;
    bool pendingWritten = false;         // stagePending replaced config.pending.json (durable or not)
    bool committed = false;              // R7 renamed and synced: config.json durably holds B
    bool commitVisible = false;          // R7's rename happened (durable or not)
    Phase phase = Phase::None;
    Deadline phaseDeadline{};
    // The proofs only this Txn can establish (IrreversibleGate::Proofs).
    std::optional<Phase> overrun;        // a phase that was left after its deadline had passed
    bool engineFree = false;             // the engine-free proof holds (proveEngineFree)
    bool lockMatching = false;           // engineHold's identity matched the record, judged on time
    bool installAttempted = false;       // InstallService was admitted: "not installed" no longer proves anything
    bool firewallConnected = false;      // IFirewall::connect returned inside its phase deadline (round 4)
    bool programGone = false;            // uninstall: the program was removed, scheduled, or not there (WS-16)
    QStringList notes;                   // said after the result's message (an entry that was not written)

    Proofs proofs() const
    {
        Proofs pr;
        pr.phase = phase;
        pr.phaseDeadline = phaseDeadline;
        pr.overrun = overrun;
        pr.trustedFolder = trusted;
        pr.engineLockHeld = engineHold != nullptr;
        pr.engineFree = engineFree;
        pr.notInstalled = detected && !installed && !installAttempted;
        pr.lockMatching = lockMatching;
        pr.committed = committed;
        pr.firewallConnected = firewallConnected;
        pr.programGone = programGone;
        return pr;
    }

    // ---- time ------------------------------------------------------------------------------------
    // A phase whose deadline judges native calls. The apply-lock wait has its own timeout, the ACK loop
    // ends at its deadline by design, and the cleanup is best effort.
    static bool judged(Phase ph) { return ph != Phase::None && ph != Phase::Lock && ph != Phase::Ack && ph != Phase::Cleanup; }
    // Entering a phase never forgives the one it leaves: an overrun is latched, and the gate refuses every
    // later step but failClosed's.
    void enter(Phase ph, seconds budget)
    {
        if (!overrun && judged(phase) && clock.now() > phaseDeadline) overrun = phase;
        phase = ph;
        phaseDeadline = clock.now() + budget;
        if (phases) phases->enter(ph, phaseDeadline);
        publishEvidence();
    }
    void checkLate()
    {
        if (clock.now() > phaseDeadline) throw Late{phase};
    }
    // Runs one native call. false (with *err) when it threw; throws Late when it returned too late.
    template <class F>
    bool call(F&& f, QString* err)
    {
        try {
            f();
        } catch (const std::exception& e) {
            *err = what(e);
            checkLate();
            return false;
        }
        checkLate();
        return true;
    }

    // The firewall connection every firewall step needs (Interfaces.h: connect() does the COM setup and
    // ConnectServer on Windows). It is a native call of the current phase: false (with *err) when it threw,
    // Late when it returned after the deadline. Connected once per operation.
    bool connectFirewall(QString* err)
    {
        if (firewallConnected) return true;
        if (!call([&] { seams.connectFirewall(phaseDeadline); }, err)) return false;
        firewallConnected = true;
        return true;
    }

    // ---- what a result says about the engine -----------------------------
    // Every stop wording of every operation comes from engineClaim() with this evidence: the gate's own
    // derivation of EngineStopped (Proofs::stopEvidence), read at the moment the text is made. A hold whose
    // identity was rejected, a stop request, blocked starts: none of them is evidence, so none says "stopped".
    StopEvidence evidence() const { return proofs().stopEvidence(); }
    QString claim(EngineClaim c, const QString& detail = {}) const { return engineClaim(evidence(), c, detail); }
    // The interim notification words the same evidence. Published whenever it may change; that is always
    // between native calls, and an interim is only ever delivered while the worker is inside one.
    void publishEvidence()
    {
        if (phases) phases->setEvidence(evidence());
    }

    // ---- results ---------------------------------------------------------------------------------
    void setPersisted(Persisted ps, const QByteArray& sha)
    {
        persisted = ps;
        persistedSha = sha;
    }
    Result make(Outcome o, const QString& step, const QString& message, const QString& post = {}) const
    {
        Result r;
        r.outcome = o;
        r.persisted = persisted;
        if (persisted != Persisted::None) r.persistedSha256 = persistedSha;
        r.step = step;
        r.message = notes.isEmpty() ? message : message + QLatin1Char(' ') + notes.join(QLatin1Char(' '));
        r.postcondition = post;
        r.fileChanges = fileChanges;
        return r;
    }
    QString folderNote() const
    {
        return fileChanges.isEmpty() ? QString()
                                     : QStringLiteral(" Folder changes: %1.").arg(fileChanges.join(QStringLiteral("; ")));
    }
    Result nothing(const QString& reason, const QString& post = {}) const
    {
        return make(Outcome::NothingChanged, QString(),
                    QStringLiteral("The settings and the service were not changed: %1.").arg(reason) + folderNote(), post);
    }
    QString editsNote() const
    {
        if (!hasSubmission) return {};
        if (persisted == Persisted::Staged) return QStringLiteral(" Your edits are saved but not applied.");
        // None: not written, or written without the durability evidence: either way the editor's copy is
        // the one to keep.
        if (persisted == Persisted::None) return QStringLiteral(" Your edits are not durably saved; they are still open.");
        return {};
    }
    Result uncommitted(const QString& step, const QString& msg, const QString& post) const
    {
        return make(Outcome::Uncommitted, step,
                    QStringLiteral("Could not %1: %2.").arg(step, msg) + editsNote() +
                        QStringLiteral(" Service: %1.").arg(post) + folderNote(),
                    post);
    }
    // A refusal. NothingChanged while nothing has changed; once the barrier has run its effects are changes,
    // so the refusal is Uncommitted with failClosed's proof (the --force-recreate path refuses after
    // Repair's barrier).
    Result refuse(const QString& step, QString reason)
    {
        if (!barrierRan) return nothing(reason);
        while (reason.endsWith(QLatin1Char('.'))) reason.chop(1);
        return uncommitted(step, reason, failClosed());
    }
    Result committedNotRunning(const QString& step, const QString& msg, const QString& post) const
    {
        return make(Outcome::CommittedNotRunning, step,
                    QStringLiteral("The new settings are saved and the firewall matches them, but the bridge did not "
                                   "start: could not %1: %2. Service: %3.")
                        .arg(step, msg, post) + folderNote(),
                    post);
    }
    // A native call returned after its phase deadline (D57): the operation stopped there and failClosed
    // ran. The message says what the call may still have done, from what was recorded before lateness was
    // judged: a late call may have succeeded.
    Result stateUnknownLate(Phase ph, const QString& post) const
    {
        return stateUnknownStopped(phaseName(ph),
                                   QStringLiteral("a call while %1 returned after its deadline, so the operation stopped "
                                                  "there")
                                       .arg(phaseName(ph)),
                                   post);
    }
    // The operation stopped before a step (`why`, lower case, no full stop) after something had changed.
    Result stateUnknownStopped(const QString& step, const QString& why, const QString& post) const
    {
        QString sentence = why;
        if (!sentence.isEmpty()) sentence[0] = sentence[0].toUpper();
        if (committed)
            return make(Outcome::StateUnknown, step,
                        QStringLiteral("The new settings are saved and the firewall matches them, but %1. Service: %2.")
                                .arg(why, post) + folderNote(),
                        post);
        const QString config = commitVisible
                                   ? QStringLiteral("config.json already holds the new settings, but they may not "
                                                    "survive a power loss")
                                   : QStringLiteral("config.json was not replaced");
        return make(Outcome::StateUnknown, step,
                    QStringLiteral("%1; %2.").arg(sentence, config) + editsNote() + QStringLiteral(" Service: %1.").arg(post) +
                        folderNote(),
                    post);
    }
    // The one handler for an operation that stopped: a call returned late (checkLate), the barrier ended at
    // its deadline (B4), or the gate refused a step. Before anything but the folder's own P4/P5 changes,
    // nothing changed: NothingChanged, no failClosed (it would stop and disable a healthy bridge for
    // nothing). After that the cleanup always runs (Stop's keeps the start type), and the result is
    // StateUnknown with what the cleanup proved (D57; WS-5 round 6).
    Result stopped(const QString& step, const QString& why)
    {
        if (!gate.changedBeyondPrologue()) return nothing(why);
        const QString post = failClosed();
        return stateUnknownStopped(step, why, post);
    }
    Result stopped(const Late& late)
    {
        return stopped(phaseName(late.phase),
                       QStringLiteral("a call while %1 returned after its deadline, so the operation stopped there")
                           .arg(phaseName(late.phase)));
    }
    Result stopped(const GateRefusal& g)
    {
        if (g.kind == GateRefusal::Kind::Late) return stopped(Late{g.phase});
        return stopped(stepName(g.step), g.describe());
    }

    // ---- staging (R4, and the pending write of SaveAndRemove/Deactivate) --------------------------
    // Puts `n` durably into config.pending.json and records persisted = Staged when that is established.
    // When the file already holds exactly `n`, its durability is confirmed the way a fresh write's is:
    // atomicWrite flushed its data before the rename, so the directory sync completes the evidence. If
    // that sync fails, `n` is written afresh, and that write reports its own durability. false = not
    // durably staged; *notDurable then says why (also when the bytes are visible but not durable).
    bool stagePending(Step step, const QByteArray& n, const ConfigFiles::Loaded& pending, QString* notDurable)
    {
        QString err;
        bool durable = pending.present && pending.error.isEmpty() && pending.bytes == n && seams.syncFolder(&err);
        if (!durable) {
            const WriteResult w = seams.writePendingFile(gate.admit(step), n);
            durable = w.replaced && w.durable;
            pendingWritten = w.replaced;
            err = w.error;
        }
        if (durable) setPersisted(Persisted::Staged, submittedSha);   // before any lateness is judged
        else if (notDurable) *notDurable = err;
        return durable;
    }

    // ---- prologue (P1-P3, the read-only lock checks, P4-P5) --------------------------------------
    std::optional<Result> lockAndDetect()
    {
        enter(Phase::Lock, Applier::kApplyLockTimeout);
        applyHold = seams.acquireApplyLock(Applier::kApplyLockTimeout);
        if (!applyHold) {
            const QString refused = seams.applyLockRefusalReason();
            if (!refused.isEmpty()) return nothing(QStringLiteral("the apply lock cannot be used: %1").arg(refused));
            return nothing(QStringLiteral("another operation is still running (the apply lock was not free within 120 s)"));
        }
        enter(Phase::Prologue, Applier::kStart);
        try {
            inst = seams.detect(phaseDeadline);
        } catch (const std::exception& e) {
            return nothing(QStringLiteral("could not query the installation: %1").arg(what(e)));
        }
        // P3: a service this program did not install refuses every operation.
        if (inst.state == InstallState::Foreign)
            return nothing(QStringLiteral("the service is not one this program installed (%1); remove it first")
                               .arg(inst.detail.isEmpty() ? QStringLiteral("unrecognised") : inst.detail));
        installed = inst.state == InstallState::Current || inst.state == InstallState::IncompleteInstall;
        detected = true;
        publishEvidence();
        // A late query is judged by the gate at the first step that changes anything (P4, B0 or the pending
        // write), which refuses it NothingChanged (D57).
        return std::nullopt;
    }

    // INV-116, read-only: supported file system; for normal operations a pair that is not matching is
    // refused here when it can be read without the lock (Linux). Install/Repair go on to recovery.
    std::optional<Result> lockPairPrologue(bool recovering)
    {
        QString why;
        if (!seams.lockFilesystemSupported(&why)) return nothing(QStringLiteral("unsupported file system: %1").arg(why));
        const LockFileRead lock = seams.peekLock();
        lockUnreadableUnheld = lock.status == LockFileRead::Status::Unreadable;
        pair = LockIdentity::classify(lock, seams.readRecord());
        if (recovering || pair == LockIdentity::PairState::Matching || pair == LockIdentity::PairState::Unknown)
            return std::nullopt;
        return nothing(QStringLiteral("lock record mismatch: %1; run --install or Repair").arg(LockIdentity::describe(pair)));
    }

    std::optional<Result> openFolder(TrustMode mode)
    {
        trusted = false;   // the proof is this open's, not an earlier read-only look
        opened = seams.openMutating(mode, gate);
        fileChanges += opened.fileChanges;
        if (opened.refused) throw *opened.refused;   // after its changes so far are recorded
        if (!opened.usable)
            return refuse(mode == TrustMode::Repair ? QStringLiteral("repair the data folder")
                                                    : QStringLiteral("open the data folder"),
                          opened.error);
        trusted = true;
        // P5: our own leftover temps go (by handle, regular files only); the engine's are left alone.
        for (const QString& n : opened.inspection.children) {
            if (!applierTemp(n)) continue;
            QString err;
            seams.removeOwnTemp(gate.admit(Step::TempSweep), n, &err);
        }
        // P5: a pending file equal to config.json is dropped (D48 reload rule, INV-108).
        const ConfigFiles::Loaded onDisk = seams.readCommitted(opened.inspection);
        const ConfigFiles::Loaded staged = seams.readPending(opened.inspection);
        if (staged.present && staged.error.isEmpty() && onDisk.present && onDisk.error.isEmpty() &&
            staged.bytes == onDisk.bytes) {
            QString err;
            seams.dropEqualPending(gate.admit(Step::TempSweep), &err);
        }
        return std::nullopt;
    }

    // The committed and pending files, and P6 for an operation that carries a save request.
    struct Loaded {
        ConfigFiles::Loaded committed, pending;
    };
    std::optional<Result> loadAndCheckPrecedence(const std::optional<SaveRequest>& req, Loaded& out)
    {
        out.committed = seams.readCommitted(opened.inspection);
        out.pending = seams.readPending(opened.inspection);
        if (!req || !out.pending.present) return std::nullopt;
        if (!out.pending.error.isEmpty() && out.pending.bytes.isEmpty())
            return refuse(QStringLiteral("check the saved, not applied settings"),
                          QStringLiteral("config.pending.json cannot be read (%1)").arg(out.pending.error));
        const QByteArray p6 = sha256Hex(out.pending.bytes).toLatin1();
        const bool fromThatPending = req->source.kind == Source::Pending && req->source.sha256 == p6;
        if (fromThatPending || sha256Hex(req->config).toLatin1() == p6 || req->supersedePending) return std::nullopt;
        return refuse(QStringLiteral("check the saved, not applied settings"),
                      QStringLiteral("saved, not applied settings (config.pending.json) were changed since these were "
                                     "loaded; reload them, or replace them explicitly"));
    }

    std::optional<Result> validateSubmission(const QByteArray& bytes, BridgeConfig* parsed)
    {
        QString why;
        const std::optional<BridgeConfig> c = fromJson(bytes, &why);
        if (!c) return refuse(QStringLiteral("check the new settings"), QStringLiteral("the new settings cannot be read: %1").arg(why));
        const QStringList errors = c->validate();
        if (!errors.isEmpty())
            return refuse(QStringLiteral("check the new settings"),
                          QStringLiteral("the new settings are not valid: %1").arg(errors.join(QLatin1Char(' '))));
        if (parsed) *parsed = *c;
        return std::nullopt;
    }

    // ---- barrier ---------------------------------------------------------------------------------
    // B0-B4 under one 30 s deadline. On anything but success, the operation's Result.
    std::optional<Result> barrier(bool blockStarts, bool toleratePair)
    {
        enter(Phase::Barrier, Applier::kBarrier);
        barrierRan = true;
        Barrier b(seams, gate, clock);
        Barrier::Options o;
        o.revokeToken = trusted;
        o.blockStarts = blockStarts;
        o.toleratePair = toleratePair;
        o.deadline = phaseDeadline;
        Barrier::Outcome out = b.run(o, fx);
        pair = out.pair;
        switch (out.end) {
        case Barrier::End::Held:
            engineHold = std::move(out.hold);
            pairAuthoritative = true;   // read through the held lock (D58)
            if (pair == LockIdentity::PairState::Matching) {
                establishLockMatching();
                return std::nullopt;
            }
            // install/Repair tolerate a pair that does not match, but the hold proves nothing then: the
            // acquired engine.lock is not the recorded one, and an old engine may still hold another (an
            // unlinked inode). The engine-free proof comes now, before the folder repair or R3 can run, as for
            // a barrier that ended without a hold.
            return proveEngineFree();
        case Barrier::End::RevokeNotOpened:
            return nothing(QStringLiteral("start.token could not be opened safely (%1)").arg(out.error));
        case Barrier::End::RevokeNotFlushed:
            // Master §4.6 (N8-6): stops before B1. Nothing asked the bridge to stop, and whether the token is
            // already invalid is not known: the postcondition says both (WS-5 round 6 audit).
            return uncommitted(QStringLiteral("revoke the start token"), out.error,
                               QStringLiteral("the start token may already be invalidated, so the bridge may not start "
                                              "again until a Start or Save & apply succeeds; a running bridge was left "
                                              "running"));
        case Barrier::End::BlockFailed:
            return uncommitted(QStringLiteral("block service starts"), out.error, failClosed());
        case Barrier::End::StopFailed:
            if (!blockStarts) {   // Stop: StateUnknown as before; its cleanup retries the stop, keeping the start type
                const QString message =
                    QStringLiteral("Could not request the stop: %1. Already done: %2.").arg(out.error, fx.describe());
                return make(Outcome::StateUnknown, QStringLiteral("request the service to stop"), message, failClosed());
            }
            return uncommitted(QStringLiteral("request the service to stop"), out.error, failClosed());
        case Barrier::End::LockMissing:
            pairAuthoritative = true;
            // install/Repair publish it after the engine-free proof. Without a hold nothing proves the engine
            // stopped (an old engine may still hold an unlinked inode), so the proof comes now, before the
            // folder repair or R3 can run.
            if (toleratePair) return proveEngineFree();
            [[fallthrough]];
        case Barrier::End::PairNotMatching:
            return lockRefused(out.error, QStringLiteral("Run --install or Repair."));
        case Barrier::End::LockFileInsecure:
            // engine.lock itself has the wrong owner or permissions in a trusted lock directory: install and
            // Repair replace it like a mismatched pair, after the engine-free proof.
            lockFileInsecure = true;
            pairAuthoritative = true;
            if (toleratePair) return proveEngineFree();   // no hold either: the same proof first
            return lockRefused(out.error, QStringLiteral("Run --install or Repair."));
        case Barrier::End::LockInsecure:
            // Not something a replacement of engine.lock would fix: --install and Repair refuse it too, so the
            // advice is the manual step (WS-5 round 2, no dead end).
            return lockRefused(out.error, QString::fromLatin1(kLockDirectoryAdvice));
        case Barrier::End::LockUnsupported:
            return lockRefused(out.error, QString::fromLatin1(kLockFileSystemAdvice));
        case Barrier::End::Deadline:
            break;
        }
        // B4: nothing from R3 on runs. The barrier changed something it did not prove: B0-B2 ran, or one of
        // them (or B3) returned after the deadline, possibly before the stop was even requested. The
        // operation stops through the same handler as any late call, so the cleanup requests the stop and
        // reports what it proves (D57, master §4.6; WS-5 round 6). A hold a late acquisition
        // returned is handed over unvalidated: failClosed releases it and judges the lock afresh.
        engineHold = std::move(out.hold);
        return stopped(QStringLiteral("prove the bridge stopped"),
                       QStringLiteral("could not prove the bridge stopped within 30 s (%1); already done: %2")
                           .arg(out.error.isEmpty() ? QStringLiteral("a call returned after its deadline") : out.error,
                                fx.describe()));
    }

    // B3 refused the lock after B0-B2 succeeded on time: the stop was requested, but nothing proves it
    // happened (an old engine may hold another, unlinked engine.lock). The postcondition is the claim the
    // evidence allows, which here is none (WS-5 round 6: it said "stopped, not activated").
    Result lockRefused(const QString& why, const QString& advice) const
    {
        return make(Outcome::StateUnknown, QStringLiteral("prove the bridge stopped"),
                    QStringLiteral("The engine lock cannot be trusted (%1); nothing was saved or applied. Already "
                                   "done: %2. %3")
                            .arg(why, fx.describe(), advice) + editsNote() + folderNote(),
                    claim(keepStartType ? EngineClaim::StartTypeKept : EngineClaim::Disabled, fx.describe()));
    }

    // failClosed: the barrier again with the 10 s cleanup budget; the postcondition is what it proved, worded
    // by engineClaim from this cleanup's own evidence. Stop's cleanup (keepStartType) requests the stop
    // without blocking starts: Stop keeps the start type.
    QString failClosed()
    {
        // An earlier engine-free proof is not evidence for now (an install may have started an engine since;
        // round 4): the cleanup reports only what its own barrier proves.
        engineFree = false;
        enter(Phase::Cleanup, Applier::kCleanup);
        try {
            if (seams.query(phaseDeadline).state == ServiceQuery::Absent) {
                releaseEngineLock();
                return QStringLiteral("not installed");
            }
        } catch (const std::exception&) {
            // fall through to the barrier: it is the proof either way
        }
        Barrier b(seams, gate, clock);
        Barrier::Options o;
        o.revokeToken = trusted;
        o.blockStarts = !keepStartType;
        o.bestEffort = true;
        o.deadline = phaseDeadline;
        BarrierEffects cleanup;
        // Only a hold whose identity matched is passed on as B3's proof. Any other hold (a mismatched pair
        // that install/Repair tolerated, one whose identity was never judged on time, or one a late B3
        // returned) proves nothing about an engine on another inode, so it is released and B3 acquires and
        // judges the lock afresh: "verified" is never claimed from an unvalidated hold.
        std::unique_ptr<LockHold> validated = lockMatching ? std::move(engineHold) : nullptr;
        releaseEngineLock();
        const EngineClaim form = keepStartType ? EngineClaim::StartTypeKept : EngineClaim::Disabled;
        Barrier::Outcome out;
        try {
            out = b.run(o, cleanup, std::move(validated));
        } catch (const GateRefusal& g) {   // the cleanup never lets a refusal escape: it reports what it proved
            return claim(form, g.describe());
        }
        switch (out.end) {
        case Barrier::End::Held:
            // B3 kept the validated hold, or acquired engine.lock and read a matching id through it (the
            // cleanup does not tolerate a pair): the evidence is this hold.
            engineHold = std::move(out.hold);
            lockMatching = out.pair == LockIdentity::PairState::Matching;
            publishEvidence();
            if (!keepStartType && !cleanup.startsBlocked) return claim(EngineClaim::StartsNotBlocked, out.error);
            return claim(form);
        case Barrier::End::LockMissing:
            // Without a lock nothing proves an engine already running stopped: it may hold an unlinked inode.
            if (cleanup.startsBlocked)
                return claim(form, QStringLiteral("starts blocked and engine.lock does not exist, so no engine can "
                                                  "start; one already running is not proven stopped"));
            break;
        default:
            break;
        }
        return claim(form, out.error);
    }

    // ---- engine.lock acquisition and pair recovery (install / Repair) ------------------------------
    // Acquires engine.lock (a no-op when already held). On failure the operation's Result, except that a
    // caller passing `why` decides itself about a lock that is missing or an insecure file (no hold, no
    // Result): recovery treats those as pair states, not as failures.
    // The wait is bounded by the phase that runs it, never by a budget of its own (Interfaces.h: a blocking
    // call gets the remaining phase budget), and an acquisition that returns late is judged at once.
    std::optional<Result> acquireEngineLock(LockResult* why = nullptr)
    {
        if (engineHold) return std::nullopt;
        checkLate();   // an expired phase would make acquireUntil return Busy at once: name the lateness instead
        LockResult r = LockResult::Busy;
        engineHold = seams.acquireEngineLock(std::min(phaseDeadline, clock.now() + Applier::kBarrier), &r);
        if (why) *why = r;
        if (engineHold) {
            checkLate();
            return std::nullopt;
        }
        if (why && (r == LockResult::InsecureFile || r == LockResult::Missing)) return std::nullopt;
        return make(Outcome::StateUnknown, QStringLiteral("acquire the engine lock"),
                    QStringLiteral("Could not acquire the engine lock (%1); the settings were not changed.").arg(lockFailure(r)) +
                        editsNote() + folderNote(),
                    failClosed());
    }

    // The pair as judged through the lock itself: acquire, then read the id through the held handle. A
    // lock acquireUntil reports missing, or as an insecure file, is judged without a hold.
    std::optional<Result> judgePairThroughTheLock()
    {
        LockResult why = LockResult::Busy;
        if (auto r = acquireEngineLock(&why)) return r;
        if (engineHold) {
            pair = LockIdentity::classify(seams.readHeldLock(), seams.readRecord());
            if (pair == LockIdentity::PairState::Matching) establishLockMatching();
        } else if (why == LockResult::InsecureFile) {
            lockFileInsecure = true;
            pair = LockIdentity::PairState::Mismatched;
        } else {   // Missing: it vanished since the prologue
            pair = LockIdentity::classify(LockFileRead{LockFileRead::Status::Missing, {}, {}}, seams.readRecord());
        }
        pairAuthoritative = true;
        return std::nullopt;
    }

    // Master §4.6 pair-state table, install/Repair column. Publication only after the engine-free proof:
    // starts blocked, the service stopped with no process, and apply.lock held (this Txn).
    std::optional<Result> recoverLockPair()
    {
        using LockIdentity::PairState;
        using LockIdentity::Recovery;
        // The recovery is chosen from the pair as judged through the lock (D58), never from the prologue's
        // unheld read alone. Unless B3 already judged it, a pair whose engine.lock the prologue could not read
        // (Windows: "unknown", or "partial"/"mismatched" from an Unreadable lock), or that it judged matching,
        // is judged again through the lock first; this is what a fresh install over leftover lock files
        // depends on, since no B3 runs there. An insecure engine.lock file is a mismatched
        // pair and is replaced below under the same proof.
        if (pair == PairState::Matching && pairAuthoritative) return std::nullopt;
        if (!pairAuthoritative && (pair == PairState::Matching || lockUnreadableUnheld)) {
            if (auto r = judgePairThroughTheLock()) return r;
            if (pair == PairState::Matching) return std::nullopt;
        }
        Recovery recovery = LockIdentity::recoveryFor(pair);
        if (recovery == Recovery::None)
            return uncommitted(QStringLiteral("verify the engine lock"), LockIdentity::describe(pair), failClosed());
        enter(Phase::Install, Applier::kInstall);
        // A fresh proof, right before the publication it allows.
        if (auto r = proveEngineFree(QStringLiteral("recreate the engine lock"),
                                     QStringLiteral("the engine lock was not recreated")))
            return r;
        // Leftovers of an interrupted publication, under the same proof.
        for (const QString& n : seams.lockDirectoryChildren()) {
            if (!isTempName(n)) continue;
            QString e;
            seams.removeLockTemp(gate.admit(Step::LockPublish), n, &e);
        }
        QString before = describePair();
        auto publish = [&](const char* name, const QByteArray& id) -> std::optional<Result> {
            const WriteResult w = seams.publishLockFile(gate.admit(Step::LockPublish), QString::fromLatin1(name), id);
            // Recorded before lateness or failure is judged: a replaced file is a change either way.
            if (w.replaced)
                fileChanges << QStringLiteral("%1 published%2 (%3)")
                                   .arg(QString::fromLatin1(name),
                                        w.durable ? QString() : QStringLiteral(", not yet durable"), before);
            checkLate();
            if (w.replaced && w.durable) return std::nullopt;
            return uncommitted(QStringLiteral("publish %1").arg(QString::fromLatin1(name)), w.error, failClosed());
        };
        QByteArray id;
        if (recovery == Recovery::PublishRecordFromHeldLock) {
            // The record is published from the id read THROUGH the held lock. If that read is not a valid
            // lock after all (it changed, or the lock turns out to be an insecure file), the pair is
            // mismatched and both files are replaced: never a refusal that the same recovery repeats.
            LockResult why = LockResult::Busy;
            if (auto r = acquireEngineLock(&why)) return r;
            const std::optional<QByteArray> held =
                engineHold ? LockIdentity::parse(seams.readHeldLock().bytes) : std::nullopt;
            if (held) {
                id = *held;
                if (auto r = publish(LockIdentity::kRecordName, id)) return r;
            } else {
                if (why == LockResult::InsecureFile) lockFileInsecure = true;
                pair = PairState::Mismatched;
                before = describePair();
                recovery = Recovery::ReplaceBoth;
            }
        }
        if (recovery != Recovery::PublishRecordFromHeldLock) {
            // Release, replace, re-acquire. On Windows whoever holds engine.lock holds it with
            // share mode 0, the Applier included, so the file cannot be replaced while our own hold is
            // open. The engine-free proof does not rest on the hold (starts blocked, no process,
            // apply.lock held), so it is released first; the new lock is acquired again below and its
            // identity re-checked through that hold.
            releaseEngineLock();
            id = LockIdentity::newId();
            if (auto r = publish(LockIdentity::kLockName, id)) return r;
            if (auto r = publish(LockIdentity::kRecordName, id)) return r;
            if (auto r = acquireEngineLock()) return r;
        }
        pair = LockIdentity::classify(seams.readHeldLock(), seams.readRecord());
        if (pair != PairState::Matching)
            return uncommitted(QStringLiteral("verify the engine lock"), LockIdentity::describe(pair), failClosed());
        establishLockMatching();
        lockFileInsecure = false;
        return std::nullopt;
    }

    // ---- proofs ------------------------------------------------------------------------------------
    // The engine-free proof (master §4.6 "Lock identity"): starts blocked by this operation, and the service
    // stopped with an empty process set. It is what proves termination when no engine.lock hold can: the
    // lock is missing, or an insecure file that cannot be acquired, or the lock that was acquired is not the
    // recorded one (round 4). AllowStarts retires it.
    std::optional<Result> proveEngineFree(const QString& step = QStringLiteral("prove the bridge stopped"),
                                          const QString& consequence = QStringLiteral(
                                              "the operation stopped before changing the data folder, the service or "
                                              "the engine lock"))
    {
        ServiceQuery q;
        QString err;
        if (!call([&] { q = seams.query(phaseDeadline); }, &err))
            return uncommitted(QStringLiteral("prove no engine runs"), err, failClosed());
        const bool startsBlocked = fx.startsBlocked || installedNow;
        const bool noProcess = (q.state == ServiceQuery::Absent || q.state == ServiceQuery::Stopped) && q.pid == 0;
        if (!startsBlocked || !noProcess)
            return make(Outcome::StateUnknown, step,
                        QStringLiteral("Could not prove that no engine runs, so %1 (%2). The settings were not changed.")
                                .arg(consequence, describePair()) + editsNote() + folderNote(),
                        failClosed());
        engineFree = true;
        publishEvidence();
        return std::nullopt;
    }
    // The lock identity read through the hold matched: a proof only when it was read inside the phase's
    // deadline, so lateness is judged before it is recorded.
    void establishLockMatching()
    {
        checkLate();
        lockMatching = engineHold != nullptr && pair == LockIdentity::PairState::Matching;
        publishEvidence();
    }
    void releaseEngineLock()
    {
        engineHold.reset();
        lockMatching = false;
        publishEvidence();
    }
    QString describePair() const
    {
        return lockFileInsecure ? QStringLiteral("engine.lock has the wrong owner or permissions")
                                : LockIdentity::describe(pair);
    }

    // ---- R9 + R10 ----------------------------------------------------------------------------------
    // From the firewall sweep to the ACK. `b` = the bytes the firewall was synced from.
    // `restart`: Start (the committed settings, unchanged) rather than Save & apply / --apply / --install.
    Result activate(const FirewallGeneration& g, const QByteArray& b, bool restart = false)
    {
        QString err;
        enter(Phase::Firewall, Applier::kFirewall);
        if (!call([&] { seams.settleServiceJobs(gate.admit(Step::SettleJobs), phaseDeadline); }, &err))
            return committedNotRunning(QStringLiteral("settle outstanding service jobs"), err, failClosed());
        if (!call([&] { seams.sweepFirewall(gate.admit(Step::FirewallSweep), g, phaseDeadline); }, &err))
            return committedNotRunning(QStringLiteral("remove stale firewall rules"), err, failClosed());
        enter(Phase::Activate, Applier::kFiles);
        const IStartToken::Written w = seams.publishStartToken(gate.admit(Step::TokenPublish), g, phaseDeadline);
        checkLate();
        if (!w.replaced)
            return committedNotRunning(QStringLiteral("write the start token"), w.error, failClosed());
        const QString durability = w.durable ? QString()
                                             : QStringLiteral(" Warning: the start token may not survive a power loss "
                                                              "(%1); if it is lost, the bridge cannot run again until "
                                                              "Start.")
                                                   .arg(w.error);
        // Released immediately before enable/start (INV-92).
        releaseEngineLock();
        enter(Phase::Activate, Applier::kStart);
        if (!call([&] { seams.enableStarts(gate.admit(Step::AllowStarts), phaseDeadline); }, &err))
            return committedNotRunning(QStringLiteral("enable the service"), err, failClosed());
        if (!call([&] { seams.startService(gate.admit(Step::ServiceStart), phaseDeadline); }, &err))
            return committedNotRunning(QStringLiteral("start the service"), err, failClosed());
        // R10: the ACK (D25, D52, INV-109).
        enter(Phase::Ack, Applier::kAck);
        const QString digest = sha256Hex(b);
        for (;;) {
            ServiceQuery q;
            try {
                q = seams.query(phaseDeadline);
            } catch (const std::exception&) {
            }
            const std::optional<StatusReport> s = seams.readStatus();
            if (s && q.pid != 0 && s->startToken == w.token && s->pid == q.pid && s->configSha256 == digest) {
                if (s->configError)
                    return committedNotRunning(QStringLiteral("load the settings"), *s->configError, failClosed());
                return make(Outcome::Ok, QString(),
                            (restart ? QStringLiteral("Started: the bridge is running the saved settings.")
                                     : QStringLiteral("Saved and applied: the bridge is running the new settings.")) +
                                durability + folderNote(),
                            QStringLiteral("running (confirmed by the bridge)"));
            }
            if (clock.now() >= phaseDeadline) break;
            clock.sleepFor(Applier::kAckPoll);
        }
        QString observed = QStringLiteral("state unknown");
        QString cause;   // user report 2026-10-16: a bridge that stopped says why, not only that the wait ran out
        try {
            const ServiceQuery q = seams.query(clock.now() + Applier::kStart);
            observed = q.state == ServiceQuery::Running ? QStringLiteral("the service reports running, unconfirmed")
                                                        : QStringLiteral("the service is not running");
            if (q.state != ServiceQuery::Running) {
                const QString exit = describeServiceExit(q);
                cause = exit.isEmpty() ? QStringLiteral(" The service is not running.")
                                       : QStringLiteral(" The service is not running: %1.").arg(exit);
                if (!exit.isEmpty()) observed += QStringLiteral(" (%1)").arg(exit);
            }
        } catch (const std::exception& e) {
            observed = QStringLiteral("could not query the service: %1").arg(what(e));
        }
        return make(Outcome::StateUnknown, QStringLiteral("confirm the bridge loaded the settings"),
                    QStringLiteral("The new settings are saved and the firewall matches them; the bridge did not confirm "
                                   "loading them within 15 s.") + cause + durability + folderNote(),
                    observed);
    }
};

}  // namespace

Applier::Applier(Platform& p, IClock& clock, PhaseClock* phases) : m_p(p), m_clock(clock), m_phases(phases) {}

// ---------------------------------------------------------------------------------------------------
Result Applier::run(const std::optional<SaveRequest>& req, bool install, bool repairFirst)
{
    Txn t(m_p, m_clock, m_phases);
    if (auto r = t.lockAndDetect()) return *r;
    // The window's install route (D67): the user consented on the installation the window read before
    // its questions. One that changed since (installed elsewhere, or another build appeared while a question was open)
    // is refused here, under apply.lock and before anything is written, never installed over without its question.
    if (req && req->expectInstallation.set) {
        const SaveRequest::InstallationBasis& b = req->expectInstallation;
        const bool same = t.inst.state == b.state && t.inst.otherBuild == b.otherBuild &&
                          (!b.otherBuild || t.inst.installedSha256 == b.installedSha256);
        if (!same) {
            const QString now = t.inst.state == InstallState::NotInstalled ? QStringLiteral("not installed")
                                : t.inst.state == InstallState::Current    ? QStringLiteral("installed")
                                : t.inst.otherBuild ? QStringLiteral("another build, not finished installing (%1)")
                                                          .arg(t.inst.installedSha256.isEmpty()
                                                                   ? QStringLiteral("program sha256 unknown")
                                                                   : QStringLiteral("program sha256 %1").arg(t.inst.installedSha256.left(12)))
                                                    : QStringLiteral("partly installed: %1")
                                                          .arg(t.inst.detail.isEmpty() ? QStringLiteral("unfinished") : t.inst.detail);
            return t.make(Outcome::NothingChanged, QStringLiteral("check the installation"),
                          QStringLiteral("Nothing was saved: the installation changed since the window last read it (the "
                                         "bridge service is now %1). Nothing was written and the service was left as it "
                                         "is; check what the window shows and click again.")
                              .arg(now));
        }
    }
    // R1
    // A complete installation of another build may be kept when the request says so (the window asked, user report
    // 2026-10-16): the settings go to the existing service and its program is not replaced.
    const bool keepOtherBuild = req && req->keepInstalledBuild && t.inst.otherBuild;
    if (t.inst.state == InstallState::IncompleteInstall && !install && !keepOtherBuild)
        return t.nothing(QStringLiteral("the installation is incomplete; finish it with --install"));
    if (t.inst.state == InstallState::NotInstalled && !install)
        return t.nothing(QStringLiteral("the service is not installed; install it first"));
    if (auto r = t.lockPairPrologue(install)) return *r;
    // A request's bytes are parsed and validated in memory before anything changes; with --force-recreate
    // that is before Repair's barrier too.
    if (req)
        if (auto r = t.validateSubmission(req->config, nullptr)) return *r;

    try {
        if (repairFirst) {
            // --force-recreate = Repair first, under the same lock (master §4.6): Repair's barrier, then
            // the folder repair. The live folder is moved aside only once the engine is proven stopped
            // (engine.lock held, or the engine-free proof when no hold is possible) or not installed (the
            // gate refuses FolderRepair otherwise); a refusal after the barrier is Uncommitted with
            // failClosed's proof, never "nothing changed". As in repair(), B0 touches the token only when a
            // read-only inspection trusts the folder: an untrusted folder is left whole until its rename.
            t.trusted = t.seams.openReadOnly().usable;
            if (t.installed)   // R2, here before the folder repair
                if (auto r = t.barrier(true, install)) return *r;
            t.enter(Phase::Folder, kFiles);
            if (auto r = t.openFolder(TrustMode::Repair)) return *r;
        } else {
            if (auto r = t.openFolder(TrustMode::Mutating)) return *r;
        }
        Txn::Loaded loaded;
        if (auto r = t.loadAndCheckPrecedence(req, loaded)) return *r;

        // N: the request, else the pending file (--apply promotes it), else none (the committed bytes).
        std::optional<QByteArray> n;
        if (req) {
            n = req->config;
        } else if (loaded.pending.present) {
            if (!loaded.pending.error.isEmpty())
                return t.refuse(QStringLiteral("check the saved, not applied settings"), loaded.pending.error);
            n = loaded.pending.bytes;
            if (auto r = t.validateSubmission(*n, nullptr)) return *r;
        }
        if (n) {
            t.hasSubmission = true;
            t.submittedSha = sha256Hex(*n).toLatin1();
        }

        // R2 (with --force-recreate it already ran, before the folder repair)
        if (!repairFirst)
            if (t.installed)
                if (auto r = t.barrier(true, install)) return *r;
        // R3
        if (install) {
            QString err;
            t.enter(Phase::Install, kInstall);
            if (!t.call([&] { t.seams.installService(t.gate.admit(Step::InstallService), t.inst.state, t.phaseDeadline); },
                        &err))
                return t.uncommitted(QStringLiteral("install the service"), err, t.failClosed());
            t.installedNow = true;
            if (auto r = t.recoverLockPair()) return *r;
            // This build's program is installed (the copy was verified against this build's digest, or the step threw):
            // the Apps & features entry names it (D63, INV-126). Not a failure of the install when it cannot be written.
            const QString notWritten = t.seams.publishAppsEntry(t.gate.admit(Step::EntryWrite));
            if (!notWritten.isEmpty()) t.notes << QStringLiteral("Apps & features entry not written: %1.").arg(notWritten);
        }
        // R4: the exact N staged durably before anything is committed (D48, INV-108). A pending file that
        // already holds N (a retry, or --apply promoting it) is Staged only once its durability is
        // established: visible bytes may come from a write whose directory sync failed.
        if (n) {
            t.enter(Phase::Stage, kFiles);
            QString err;
            const bool staged = t.stagePending(Step::StagePending, *n, loaded.pending, &err);
            t.checkLate();
            if (!staged) return t.uncommitted(QStringLiteral("save the settings"), err, t.failClosed());
        }
        // R5: the temp that will become config.json; B is what reads back through its handle.
        QByteArray bytes;
        if (n) bytes = *n;
        else if (loaded.committed.present && loaded.committed.error.isEmpty()) bytes = loaded.committed.bytes;
        else if (!loaded.committed.present) bytes = toJson(BridgeConfig{});   // first install (INV-49)
        else return t.uncommitted(QStringLiteral("read the saved config"), loaded.committed.error, t.failClosed());
        t.enter(Phase::Commit, kFiles);
        QString err;
        std::unique_ptr<TempFile> temp = t.seams.createConfigTemp(&err);
        if (!temp) return t.uncommitted(QStringLiteral("write the new settings"), err, t.failClosed());
        if (!temp->write(bytes, &err) || !temp->flush(&err))
            return t.uncommitted(QStringLiteral("write the new settings"), err, t.failClosed());
        const QByteArray b = temp->readBack(ConfigFiles::kConfigMaxBytes, &err);
        t.checkLate();
        if (!err.isEmpty()) return t.uncommitted(QStringLiteral("read the new settings back"), err, t.failClosed());
        QString why;
        const std::optional<BridgeConfig> config = fromJson(b, &why);
        if (!config) return t.uncommitted(QStringLiteral("read the new settings back"), why, t.failClosed());
        if (const QStringList errors = config->validate(); !errors.isEmpty())
            return t.uncommitted(QStringLiteral("read the new settings back"), errors.join(QLatin1Char(' ')), t.failClosed());
        // R6: the firewall is synced from B, the read-back bytes (INV-25, INV-73).
        const FirewallGeneration g = FirewallGeneration::fresh();
        t.enter(Phase::Firewall, kFirewall);
        if (!t.connectFirewall(&err)) return t.uncommitted(QStringLiteral("update the firewall"), err, t.failClosed());
        if (!t.call([&] { t.seams.syncFirewall(t.gate.admit(Step::FirewallSync), *config, g, t.phaseDeadline); }, &err)) {
            try {
                t.seams.rollbackFirewall(t.gate.admit(Step::FirewallSync), g, t.phaseDeadline);
            } catch (const std::exception&) {
            }
            return t.uncommitted(QStringLiteral("update the firewall"), err, t.failClosed());
        }
        // R7: THE COMMIT — the synced temp renamed over config.json, then the directory flushed.
        t.enter(Phase::Commit, kFiles);
        const TempFile::Commit c = t.seams.commitConfig(t.gate.admit(Step::ConfigCommit), *temp);
        // What happened on disk is recorded before lateness is judged: a durable commit that
        // returned late is still Committed, and the late result says so.
        t.commitVisible = c.replaced;
        if (c.replaced && c.durable) {
            t.committed = true;
            t.setPersisted(Persisted::Committed, n ? t.submittedSha : sha256Hex(b).toLatin1());
        }
        t.checkLate();
        if (!c.replaced) return t.uncommitted(QStringLiteral("commit the new settings"), c.error, t.failClosed());
        if (!c.durable)
            return t.uncommitted(QStringLiteral("make the committed settings durable"), c.error, t.failClosed());
        // R8: pending equals config.json now (failure harmless: P5 drops an equal pending).
        if (t.hasSubmission) {
            QString e;
            if (t.seams.removePendingFile(t.gate.admit(Step::DropPending), &e)) t.seams.syncFolder(&e);
        }
        // R8's calls belong to the Commit phase: one that returned late is latched when R9 enters its phase,
        // and the gate refuses every R9 step (D57). Committed stays recorded; failClosed runs.
        temp.reset();
        return t.activate(g, b);
    } catch (const Late& late) {
        return t.stopped(late);
    } catch (const GateRefusal& g) {
        return t.stopped(g);
    }
}

// ---------------------------------------------------------------------------------------------------
Result Applier::start(bool acceptOtherBuild)
{
    Txn t(m_p, m_clock, m_phases);
    if (auto r = t.lockAndDetect()) return *r;
    // The window starts a complete installation of another build as it is (its program is not replaced by a Start).
    if (t.inst.state == InstallState::IncompleteInstall && !(acceptOtherBuild && t.inst.otherBuild))
        return t.nothing(QStringLiteral("the installation is incomplete; finish it with --install"));
    if (t.inst.state == InstallState::NotInstalled) return t.nothing(QStringLiteral("the service is not installed"));
    if (auto r = t.lockPairPrologue(false)) return *r;
    try {
        if (auto r = t.openFolder(TrustMode::Mutating)) return *r;
        if (auto r = t.barrier(true, false)) return *r;
        // R5 without a temp: B = the committed bytes, through the trusted folder. The read is a native
        // call of the barrier's phase, judged before the next phase resets the deadline (round 2).
        const ConfigFiles::Loaded committed = t.seams.readCommitted(t.opened.inspection);
        t.checkLate();
        // No config.json (a data folder Repair recreated): Start has nothing to run; the advice is the
        // operation that works from here (round 2: no dead end).
        if (!committed.present)
            return t.uncommitted(QStringLiteral("read the saved config"),
                                 QStringLiteral("there are no saved settings (config.json does not exist); use Save & "
                                                "apply"),
                                 t.failClosed());
        if (!committed.error.isEmpty() || !committed.config)
            return t.uncommitted(QStringLiteral("read the saved config"), committed.error, t.failClosed());
        if (const QStringList errors = committed.config->validate(); !errors.isEmpty())
            return t.uncommitted(QStringLiteral("read the saved config"), errors.join(QLatin1Char(' ')), t.failClosed());
        // Start reports persisted = Committed for these bytes: their rename may be one whose directory
        // sync failed (an earlier R7), so the sync is established first.
        QString err;
        t.enter(Phase::Commit, kFiles);
        if (!t.seams.syncFolder(&err))
            return t.uncommitted(QStringLiteral("make the saved settings durable"), err, t.failClosed());
        t.checkLate();
        // R6: Start always re-syncs the firewall from the committed bytes (INV-29).
        const FirewallGeneration g = FirewallGeneration::fresh();
        t.enter(Phase::Firewall, kFirewall);
        if (!t.connectFirewall(&err)) return t.uncommitted(QStringLiteral("update the firewall"), err, t.failClosed());
        if (!t.call([&] { t.seams.syncFirewall(t.gate.admit(Step::FirewallSync), *committed.config, g, t.phaseDeadline); },
                    &err)) {
            try {
                t.seams.rollbackFirewall(t.gate.admit(Step::FirewallSync), g, t.phaseDeadline);
            } catch (const std::exception&) {
            }
            return t.uncommitted(QStringLiteral("update the firewall"), err, t.failClosed());
        }
        t.committed = true;
        t.setPersisted(Persisted::Committed, sha256Hex(committed.bytes).toLatin1());
        return t.activate(g, committed.bytes, /*restart=*/true);
    } catch (const Late& late) {
        return t.stopped(late);
    } catch (const GateRefusal& g) {
        return t.stopped(g);
    }
}

// ---------------------------------------------------------------------------------------------------
Result Applier::stop()
{
    Txn t(m_p, m_clock, m_phases);
    t.keepStartType = true;   // parity: Stop and its cleanup never block starts
    if (auto r = t.lockAndDetect()) return *r;
    if (!t.installed)   // the evidence is "not installed"
        return t.make(Outcome::Ok, QString(), t.claim(EngineClaim::StopSentence), t.claim(EngineClaim::StartTypeKept));
    if (auto r = t.lockPairPrologue(false)) return *r;
    // No P4: a ReadOnly inspection only decides whether B0 may touch the token (trusted folder only).
    const OpenedFolder ro = t.seams.openReadOnly();
    t.trusted = ro.usable;
    try {
        if (auto r = t.barrier(false, false)) return *r;
    } catch (const Late& late) {
        return t.stopped(late);   // the stop-only cleanup: the start type is kept
    } catch (const GateRefusal& g) {
        return t.stopped(g);
    }
    // The barrier ended Held with a matching identity: the evidence is that hold, read before its release.
    const Result done = t.make(Outcome::Ok, QString(), t.claim(EngineClaim::StopSentence), t.claim(EngineClaim::StartTypeKept));
    t.releaseEngineLock();
    return done;
}

// ---------------------------------------------------------------------------------------------------
namespace {

// SaveAndRemove and Deactivate: the pending file first, then the service (D38, INV-91). `saveOnly` (the window's Save
// with nothing installed): the same pending write, only while nothing is installed.
Result untick(Platform& p, IClock& clock, PhaseClock* phases, const std::optional<SaveRequest>& req, bool removeService,
              bool saveOnly = false)
{
    Txn t(p, clock, phases);
    if (auto r = t.lockAndDetect()) return *r;
    // Save only promises to change nothing but the saved, not applied settings. The window chose it on its own read of
    // the installation, before its questions; one installed since then (another window, the CLI) is refused here, under
    // apply.lock and before anything is written, so the click never stops or disables a service it did not ask about
    //. lockAndDetect already refused foreign installations.
    if (saveOnly && t.installed)
        return t.make(Outcome::NothingChanged, QStringLiteral("check the installation"),
                      QStringLiteral("Nothing was saved: the installation changed since the window last read it (the "
                                     "bridge service is now %1). Nothing was written and the service was left as it is; "
                                     "check what the window shows and click again.")
                          .arg(t.inst.state == InstallState::Current
                                   ? QStringLiteral("installed")
                                   : QStringLiteral("partly installed: %1")
                                         .arg(t.inst.detail.isEmpty() ? QStringLiteral("unfinished") : t.inst.detail)));
    if (t.installed)
        if (auto r = t.lockPairPrologue(false)) return *r;
    QString notDurable;   // the pending write is visible but its durability is not established
    try {
        if (auto r = t.openFolder(TrustMode::Mutating)) return *r;
        Txn::Loaded loaded;
        if (auto r = t.loadAndCheckPrecedence(req, loaded)) return *r;
        if (req)
            if (auto r = t.validateSubmission(req->config, nullptr)) return *r;
        if (req) {
            t.hasSubmission = true;
            t.submittedSha = sha256Hex(req->config).toLatin1();
            t.enter(Phase::Stage, Applier::kFiles);
            // A pending write that did not even replace the file changed nothing: refused before the service.
            if (!t.stagePending(Step::SavePending, req->config, loaded.pending, &notDurable) && !t.pendingWritten)
                return t.nothing(QStringLiteral("could not save the settings: %1").arg(notDurable));
        }
    } catch (const GateRefusal& g) {   // before the pending write: nothing changed but the folder's P4/P5
        return t.stopped(g);
    }
    // The result says only what holds: "saved" only with a durable pending write (else the
    // same power-loss warning as edit()), "the bridge is off" only when the barrier and every removal step
    // succeeded. Without N, a failed step is not Ok: it is Uncommitted with failClosed's postcondition.
    const Outcome done = req ? Outcome::SavedNotApplied : Outcome::Ok;
    // "the bridge is off" is engineClaim's, from the evidence held when the result is made (WS-5 round 6).
    auto withService = [&](const QString& step, const QString& post, bool failed) {
        const QString off = failed ? QString() : t.claim(EngineClaim::OffSentence);
        QString text;
        if (req) {
            text = off.isEmpty() ? QStringLiteral("Your edits are saved but not applied. ")
                                 : QStringLiteral("Your edits are saved but not applied; %1. ").arg(off);
            if (t.persisted != Persisted::Staged)
                text += QStringLiteral("Warning: they may not survive a power loss (%1); they are still open. ").arg(notDurable);
        } else if (!off.isEmpty()) {
            text = off.left(1).toUpper() + off.mid(1) + QStringLiteral(". ");
        }
        return t.make(done, step, text + QStringLiteral("Service: %1.").arg(post) + t.folderNote(), post);
    };
    // `notDone` is what the failed step left undone, said in the postcondition too: the service line is the
    // true state (round 4).
    auto stepFailed = [&](const QString& step, const QString& notDone, const QString& err, const QString& post) {
        if (!req) return t.uncommitted(step, err, QStringLiteral("%1; %2").arg(notDone, post));
        return withService(step, QStringLiteral("%1 (%2); %3").arg(notDone, err, post), true);
    };
    try {
        // The pending write comes first by design (D38); a late one is judged before the barrier.
        if (req) t.checkLate();
        if (t.installed) {
            if (auto r = t.barrier(true, false)) {
                if (!req) return *r;
                return withService(r->step, r->postcondition.isEmpty() ? r->message : r->postcondition, true);
            }
        }
        if (!removeService)   // the evidence: the barrier's validated hold, or no service installed
            return withService(QString(), t.claim(EngineClaim::Disabled), false);
        QString err;
        t.enter(Phase::Remove, Applier::kFirewall);
        // Windows: the rules go BEFORE the service (INV-91), through a connected firewall. When
        // they cannot all be removed the service stays installed, disabled, and failClosed proves it stopped:
        // uninstalling it would leave allow rules behind with nothing installed to remove them, and the
        // removal can be retried (master §4.6: rules before the service). Nothing else is left to clean up.
        // The service is named only when one is installed; failClosed then says "not installed".
        const QString keptService = t.installed ? QStringLiteral(" and the service not removed (the rules go first)")
                                                : QString();
        if (!t.connectFirewall(&err))
            return stepFailed(QStringLiteral("remove the firewall rules"),
                              QStringLiteral("firewall rules not removed") + keptService,
                              QStringLiteral("could not connect to the firewall: %1").arg(err), t.failClosed());
        if (!t.call([&] { t.seams.removeFirewallRules(t.gate.admit(Step::FirewallRemove), t.phaseDeadline); }, &err))
            return stepFailed(QStringLiteral("remove the firewall rules"),
                              QStringLiteral("firewall rules not removed, or only some") +
                                  (t.installed ? QStringLiteral(",") + keptService : QString()),
                              err, t.failClosed());
        if (t.installed) {
            t.enter(Phase::Remove, Applier::kInstall);
            if (!t.call([&] { t.seams.settleServiceJobs(t.gate.admit(Step::SettleJobs), t.phaseDeadline); }, &err) ||
                !t.call([&] { t.seams.uninstallService(t.gate.admit(Step::Uninstall), t.phaseDeadline); }, &err))
                return stepFailed(QStringLiteral("remove the service"), QStringLiteral("service not removed"), err,
                                  t.failClosed());
        }
        const Result removed = withService(QString(), QStringLiteral("removed, with its firewall rules"), false);
        t.releaseEngineLock();
        return removed;
    } catch (const Late& late) {
        if (!req) return t.stopped(late);
        const QString post = t.failClosed();
        return withService(phaseName(late.phase),
                           QStringLiteral("a call while %1 returned after its deadline, so the operation stopped "
                                          "there; %2")
                               .arg(phaseName(late.phase), post),
                           true);
    } catch (const GateRefusal& g) {
        if (!req) return t.stopped(g);
        if (g.kind == GateRefusal::Kind::Late) {
            const QString post = t.failClosed();
            return withService(phaseName(g.phase),
                               QStringLiteral("a call while %1 returned after its deadline, so the operation stopped "
                                              "there; %2")
                                   .arg(phaseName(g.phase), post),
                               true);
        }
        const QString post = t.failClosed();
        return withService(stepName(g.step), QStringLiteral("%1; %2").arg(g.describe(), post), true);
    }
}

}  // namespace

Result Applier::saveAndRemove(const std::optional<SaveRequest>& req)
{
    return untick(m_p, m_clock, m_phases, req, true);
}

Result Applier::deactivate(const std::optional<SaveRequest>& req)
{
    return untick(m_p, m_clock, m_phases, req, false);
}

Result Applier::saveOnly(const SaveRequest& req)
{
    // removeService false, saveOnly true (the line is matched by tst_IrreversibleGate::noDirectIrreversibleCalls).
    return untick(m_p, m_clock, m_phases, req, false, true);
}

// ---------------------------------------------------------------------------------------------------
namespace {

QString inFolder(const QString& folder, const QString& name)
{
    if (folder.isEmpty()) return name;
    const QChar sep = folder.contains(QLatin1Char('\\')) ? QLatin1Char('\\') : QLatin1Char('/');
    return folder.endsWith(sep) ? folder + name : folder + sep + name;
}

}  // namespace

Result Applier::uninstall(UninstallOptions options)
{
    Txn t(m_p, m_clock, m_phases);
    if (auto r = t.lockAndDetect()) return *r;
    if (t.installed)
        if (auto r = t.lockPairPrologue(false)) return *r;
    // No P4: uninstall creates and normalises nothing in the data folder. A read-only inspection decides whether B0 may
    // touch the token (as Stop does); the settings get their own inspection once the program is gone (step 3).
    t.trusted = t.seams.openReadOnly().usable;
    const bool separate = t.seams.separateProgramRemoval();
    const QString folder = t.seams.programFolder();
    bool locksMarked = false;
    QString clearFailure;   // the disposition could not be cleared on a kept program: the lock files go anyway
    // D69: a kept program keeps its lock files. Called the moment the program is known to be kept and on every way out
    // before it is gone, BEFORE anything can close a hold (failClosed and releaseEngineLock close engine.lock).
    auto unmark = [&] {
        if (!locksMarked || t.programGone) return;
        locksMarked = false;
        QString e;
        if (t.applyHold && !t.seams.clearLockDeletion(*t.applyHold, &e)) clearFailure = e;
        if (t.engineHold && !t.seams.clearLockDeletion(*t.engineHold, &e)) clearFailure = e;
    };
    auto clearNote = [&] {
        return clearFailure.isEmpty()
                   ? QString()
                   : QStringLiteral(" The lock files could not be kept with it (%1); the next install recreates them.")
                         .arg(clearFailure);
    };
    const QString serviceGone = t.installed ? QStringLiteral("The service and its firewall rules were removed.")
                                            : QStringLiteral("No service was installed; its firewall rules were removed.");
    bool entryRemoved = false;
    // A stop after the program was gone (a late call, a refused step) still removes the entry, in the cleanup phase
    // (EntryRemove is a cleanup step), so it never names a program that is not there (INV-126). Before failClosed: the
    // hold that proves the engine stopped is still held.
    auto removeEntryAfterStop = [&]() -> QString {
        if (!separate || !t.programGone || entryRemoved) return {};
        t.enter(Phase::Cleanup, kCleanup);
        QString e;
        try {
            t.seams.deleteAppsEntry(t.gate.admit(Step::EntryRemove), t.phaseDeadline);
            t.fileChanges << QStringLiteral("removed the Apps & features entry");
            return QStringLiteral(" The program is gone, so its Apps & features entry was removed.");
        } catch (const GateRefusal& g) {
            e = g.describe();
        } catch (const std::exception& ex) {
            e = what(ex);
        }
        t.fileChanges << QStringLiteral("kept the Apps & features entry (%1)").arg(e);
        return QStringLiteral(" The Apps & features entry could not be removed (%1); remove it with --uninstall.").arg(e);
    };
    // Every way out after a stop: a kept program keeps its lock files (unmark, before any hold closes); once the program
    // is gone the entry still goes, and so does the empty program folder once the holds are released.
    auto afterStop = [&](const std::function<Result()>& stop) {
        unmark();
        const QString entry = removeEntryAfterStop();
        Result r = stop();
        r.message += clearNote() + entry;
        if (separate && t.programGone) {
            t.releaseEngineLock();
            t.applyHold.reset();
            r.fileChanges += t.seams.deleteEmptyProgramFolder();
        }
        return r;
    };
    try {
        if (t.installed)
            if (auto r = t.barrier(true, false)) return *r;
        // ---- 1. rules (before the service, INV-91), jobs, the service (Linux: with its program) ----------------
        QString err;
        t.enter(Phase::Remove, kFirewall);
        const QString keptService = t.installed ? QStringLiteral(" and the service not removed (the rules go first)") : QString();
        if (!t.connectFirewall(&err))
            return t.uncommitted(QStringLiteral("remove the firewall rules"),
                                 QStringLiteral("could not connect to the firewall: %1").arg(err),
                                 QStringLiteral("firewall rules not removed%1; %2").arg(keptService, t.failClosed()));
        if (!t.call([&] { t.seams.removeFirewallRules(t.gate.admit(Step::FirewallRemove), t.phaseDeadline); }, &err))
            return t.uncommitted(QStringLiteral("remove the firewall rules"), err,
                                 QStringLiteral("firewall rules not removed, or only some%1; %2")
                                     .arg(t.installed ? QStringLiteral(",") + keptService : QString(), t.failClosed()));
        // Linux: an earlier uninstall removed the unit and kept the program (it said "uninstall again to finish"): no
        // service, so no barrier and no jobs, but uninstall() runs again for the program, the policy and the lock
        // directory. Windows removes a kept program in step 2 either way.
        const bool finishLinux = !separate && !t.installed && t.inst.programRemains;
        if (t.installed || finishLinux) {
            t.enter(Phase::Remove, kInstall);
            if ((t.installed &&
                 !t.call([&] { t.seams.settleServiceJobs(t.gate.admit(Step::SettleJobs), t.phaseDeadline); }, &err)) ||
                !t.call([&] { t.seams.uninstallService(t.gate.admit(Step::Uninstall), t.phaseDeadline); }, &err))
                return t.uncommitted(finishLinux ? QStringLiteral("remove the program") : QStringLiteral("remove the service"), err,
                                     finishLinux ? QStringLiteral("not installed; the program is kept")
                                                 : QStringLiteral("service not removed; %1").arg(t.failClosed()));
        }
        QString programLine;   // what happened to the program, for the message
        if (!separate) {
            t.programGone = true;   // Linux: uninstall() removed it with the unit (or nothing was installed)
            t.publishEvidence();
        } else {
            // ---- 2.1 the held lock files marked for deletion on close (D69) -----------------------------------------
            // engine.lock: the barrier holds it; with no service installed it is taken now when it exists (nothing can
            // be running it), so that it can be marked too.
            LockResult why = LockResult::Busy;
            if (auto r = t.acquireEngineLock(&why)) return *r;
            t.enter(Phase::Remove, kFiles);
            if (!t.seams.markLocksForDeletion(t.gate.admit(Step::LockDisposition), *t.applyHold, t.engineHold.get(), &err)) {
                t.checkLate();
                const QString message =
                    QStringLiteral("%1 The lock files in %2 could not be marked for removal (%3), so the program and its "
                                   "Apps & features entry were kept: uninstall again to finish.")
                        .arg(serviceGone, folder, err);
                return t.make(Outcome::Uncommitted, QStringLiteral("mark the lock files for removal"), message,
                              QStringLiteral("not installed; the program and its lock files are kept"));
            }
            locksMarked = true;
            t.checkLate();
            // ---- 2.2 the program (removed, or this process's running image renamed aside and scheduled) -------------
            t.enter(Phase::Remove, kInstall);
            const IServiceControl::ProgramRemoval pr =
                t.seams.deleteProgram(t.gate.admit(Step::ProgramRemove), t.phaseDeadline);
            t.fileChanges += pr.changes;   // recorded before lateness is judged: a removal is a change either way
            if (pr.kind == IServiceControl::ProgramRemoval::Kind::Kept) {
                unmark();
                t.checkLate();
                return t.make(Outcome::Uncommitted, QStringLiteral("remove the program"),
                              QStringLiteral("%1 The program in %2 was kept (%3), with its lock files and its Apps & "
                                             "features entry: uninstall again to finish.")
                                      .arg(serviceGone, folder, pr.error) +
                                  clearNote(),
                              QStringLiteral("not installed; the program is kept"));
            }
            t.programGone = true;
            t.publishEvidence();
            if (pr.kind == IServiceControl::ProgramRemoval::Kind::ScheduledAtRestart)
                programLine = QStringLiteral(" The program is removed when Windows restarts.");
            t.checkLate();
            // ---- engine.lock.id: once the program is gone, so a kept program keeps every lock file ------------------
            t.enter(Phase::Remove, kFiles);
            const QString record = QString::fromLatin1(LockIdentity::kRecordName);
            if (t.seams.removeLockRecord(t.gate.admit(Step::LockRecordRemove), &err))
                t.fileChanges << QStringLiteral("removed %1").arg(inFolder(folder, record));
            else if (err != fileNotFoundError())
                t.fileChanges << QStringLiteral("kept %1 (%2)").arg(inFolder(folder, record), err);
            t.checkLate();
        }
        // ---- 3. the settings, only when asked, only the known files of a trusted folder (INV-125) ------------------
        QString settingsLine;
        if (options.removeSettings) {
            t.enter(Phase::Remove, kFiles);
            const OpenedFolder s = t.seams.openReadOnly();
            t.trusted = s.usable;
            t.publishEvidence();
            if (s.missing) {
                settingsLine = QStringLiteral(" There were no settings to remove.");
            } else if (!s.usable) {
                settingsLine = QStringLiteral(" The settings were kept: the data folder is not trusted (%1), so nothing in "
                                              "it was touched.")
                                   .arg(s.error);
                t.fileChanges << QStringLiteral("kept the data folder (not trusted: %1)").arg(s.error);
            } else {
                const GatedSeams::SettingsRemoval sr = t.seams.removeSettings(t.gate.admit(Step::SettingsRemove), s.inspection);
                for (const QString& n : sr.removed) t.fileChanges << QStringLiteral("removed %1").arg(n);
                for (const QString& n : sr.failed) t.fileChanges << QStringLiteral("kept %1").arg(n);
                for (const QString& n : sr.kept)
                    t.fileChanges << QStringLiteral("kept %1 (not a file this program writes)").arg(n);
                if (sr.folderRemoved) {
                    t.fileChanges << QStringLiteral("removed the data folder");
                    settingsLine = QStringLiteral(" The settings were removed.");
                } else if (!sr.failed.isEmpty()) {
                    settingsLine = QStringLiteral(" Some settings could not be removed: %1.").arg(sr.failed.join(QStringLiteral(", ")));
                } else if (!sr.kept.isEmpty()) {
                    settingsLine = QStringLiteral(" The settings were removed; the data folder was kept because it holds "
                                                  "other files: %1.")
                                       .arg(sr.kept.join(QStringLiteral(", ")));
                } else {
                    t.fileChanges << QStringLiteral("kept the data folder (%1)").arg(sr.folderError);
                    settingsLine = QStringLiteral(" The settings were removed; the empty data folder was kept (%1).")
                                       .arg(sr.folderError);
                }
            }
            t.checkLate();
        }
        // Repair's renamed-aside folders (D23) are never removed; they are listed.
        for (const QString& aside : t.seams.asideFolders())
            t.fileChanges << QStringLiteral("kept %1 (moved aside by Repair; never removed)").arg(aside);
        // ---- 4. the Apps & features entry, last, once the program it names is gone (INV-126) ----------------------
        QString entryLine;
        if (separate) {
            t.enter(Phase::Remove, kFiles);
            if (t.call([&] { t.seams.deleteAppsEntry(t.gate.admit(Step::EntryRemove), t.phaseDeadline); }, &err)) {
                entryRemoved = true;
                t.fileChanges << QStringLiteral("removed the Apps & features entry");
            }
            else {
                t.fileChanges << QStringLiteral("kept the Apps & features entry (%1)").arg(err);
                entryLine = QStringLiteral(" The Apps & features entry could not be removed (%1).").arg(err);
            }
        }
        const QString message =
            (separate ? (t.installed ? QStringLiteral("Uninstalled: the service, its firewall rules and the program were removed.")
                                     : QStringLiteral("Uninstalled: the program and its firewall rules were removed."))
                      : (t.installed  ? QStringLiteral("Uninstalled: the service and the program were removed.")
                         : finishLinux ? QStringLiteral("Uninstalled: the service was already removed; the program left "
                                                        "behind was removed.")
                                       : QStringLiteral("Nothing was installed; nothing else was removed."))) +
            programLine + settingsLine + entryLine;
        Result done = t.make(Outcome::Ok, QString(), message, QStringLiteral("not installed"));
        if (!separate) return done;
        // The holds close here: the marked lock files go with them. Then the folder, when nothing else is in it (another
        // process, or a file someone added, keeps it and is named).
        t.releaseEngineLock();
        t.applyHold.reset();
        done.fileChanges += t.seams.deleteEmptyProgramFolder();
        return done;
    } catch (const Late& late) {
        return afterStop([&] { return t.stopped(late); });
    } catch (const GateRefusal& g) {
        return afterStop([&] { return t.stopped(g); });
    } catch (const std::exception& e) {
        // A seam that throws outside t.call (removeProgram's own deadline check): the same way out, so a kept
        // program keeps its lock files (D69).
        const QString why = what(e);
        return afterStop([&] { return t.stopped(phaseName(t.phase), why); });
    }
}

// ---------------------------------------------------------------------------------------------------
Result Applier::edit(const std::function<void(BridgeConfig&)>& change)
{
    Txn t(m_p, m_clock, m_phases);
    if (auto r = t.lockAndDetect()) return *r;
    try {
        if (auto r = t.openFolder(TrustMode::Mutating)) return *r;
        Txn::Loaded loaded;
        if (auto r = t.loadAndCheckPrecedence(std::nullopt, loaded)) return *r;
        const ConfigFiles::Loaded& base = loaded.pending.present ? loaded.pending : loaded.committed;
        if (!base.error.isEmpty() || !base.config) return t.nothing(base.error);
        BridgeConfig config = *base.config;
        // A change the editor refuses (no such port, a duplicate name: WS-9 ConfigEditor) throws before anything
        // is written; it never escapes the operation worker.
        try {
            change(config);
        } catch (const std::exception& e) {
            return t.nothing(what(e));
        }
        if (const QStringList errors = config.validate(); !errors.isEmpty())
            return t.nothing(QStringLiteral("the change is not valid: %1").arg(errors.join(QLatin1Char(' '))));
        const QByteArray bytes = toJson(config);
        t.hasSubmission = true;
        t.submittedSha = sha256Hex(bytes).toLatin1();
        t.enter(Phase::Stage, kFiles);
        const WriteResult w = t.seams.writePendingFile(t.gate.admit(Step::SavePending), bytes);
        if (!w.replaced) return t.nothing(QStringLiteral("could not save the change: %1").arg(w.error));
        if (w.durable) t.setPersisted(Persisted::Staged, t.submittedSha);
        return t.make(Outcome::SavedNotApplied, QString(),
                      QStringLiteral("Saved to config.pending.json, not applied; run --apply to apply it.") +
                          (w.durable ? QString() : QStringLiteral(" Warning: it may not survive a power loss (%1).").arg(w.error)),
                      QStringLiteral("unchanged"));
    } catch (const GateRefusal& g) {
        return t.stopped(g);
    }
}

// ---------------------------------------------------------------------------------------------------
namespace {

// The deletion both discards share, in a folder the Txn already opened (Mutating).
Result removePending(Txn& t)
{
    QString err;
    t.enter(Phase::Stage, Applier::kFiles);
    if (!t.seams.removePendingFile(t.gate.admit(Step::DiscardPending), &err)) {
        if (err == fileNotFoundError()) return t.nothing(QStringLiteral("there are no saved, unapplied settings"));
        return t.nothing(QStringLiteral("could not delete config.pending.json: %1").arg(err));
    }
    // The deletion is durable only with the directory sync; without it a power loss brings the file back.
    const QString warning = t.seams.syncFolder(&err)
                                ? QString()
                                : QStringLiteral(" Warning: they may come back after a power loss (%1).").arg(err);
    return t.make(Outcome::Ok, QString(), QStringLiteral("The saved, unapplied settings were discarded.") + warning + t.folderNote(),
                  QStringLiteral("unchanged"));
}

}  // namespace

Result Applier::discardPending()
{
    Txn t(m_p, m_clock, m_phases);
    if (auto r = t.lockAndDetect()) return *r;
    try {
        if (auto r = t.openFolder(TrustMode::Mutating)) return *r;
        return removePending(t);
    } catch (const GateRefusal& g) {
        return t.stopped(g);
    }
}

Result Applier::discardPendingIf(const QString& sha256)
{
    Txn t(m_p, m_clock, m_phases);
    if (auto r = t.lockAndDetect()) return *r;
    try {
        if (auto r = t.openFolder(TrustMode::Mutating)) return *r;
        // Read under apply.lock: every writer of config.pending.json is an Applier operation, so the bytes judged here
        // are the bytes the deletion removes (INV-127).
        const ConfigFiles::Loaded pending = t.seams.readPending(t.opened.inspection);
        if (!pending.present)
            return t.make(Outcome::NothingChanged, QString(),
                          QStringLiteral("There are no saved changes to discard.") + t.folderNote(), QStringLiteral("unchanged"));
        if (!pending.error.isEmpty() && pending.bytes.isEmpty())
            return t.nothing(QStringLiteral("config.pending.json cannot be read (%1)").arg(pending.error));
        if (sha256Hex(pending.bytes) != sha256)
            return t.make(Outcome::NothingChanged, QString(),
                          QStringLiteral("The saved changes were replaced since this window read them; nothing was "
                                         "discarded. Reload to see them.") +
                              t.folderNote(),
                          QStringLiteral("unchanged"));
        return removePending(t);
    } catch (const GateRefusal& g) {
        return t.stopped(g);
    }
}

// ---------------------------------------------------------------------------------------------------
Result Applier::repair()
{
    Txn t(m_p, m_clock, m_phases);
    if (auto r = t.lockAndDetect()) return *r;
    if (t.installed)
        if (auto r = t.lockPairPrologue(true)) return *r;
    // B0 only touches the token of a trusted folder; an untrusted one is left whole (INV-113).
    t.trusted = t.seams.openReadOnly().usable;
    QString service = QStringLiteral("not installed");
    try {
        bool noSavedSettings = false;   // the repaired folder holds no config.json: Start has nothing to run
        // "verified: engine lock acquired" only for a hold whose identity matched; a mismatched hold Repair
        // tolerated is not that proof, the engine-free proof is (WS-5 round 4).
        // The stop is worded by engineClaim from the evidence (a validated hold, else the engine-free proof
        // the barrier took); the advice follows the state of the lock.
        auto serviceState = [&] {
            QString advice;
            if (t.lockMatching && noSavedSettings)
                advice = QStringLiteral("run Save & apply to start it again (the data folder holds no saved settings "
                                        "for Start)");
            else if (t.lockMatching)
                advice = QStringLiteral("run Start or Save & apply to start it again");
            else if (t.engineHold)
                advice = QStringLiteral("engine.lock does not match its record (%1); run Repair again to replace it")
                             .arg(t.describePair());
            else if (t.lockFileInsecure)
                advice = QStringLiteral("engine.lock has the wrong owner or permissions, so no managed engine can take "
                                        "it; run Repair again to replace it");
            else
                advice = QStringLiteral("engine.lock is missing, so no managed engine can run");
            return QStringLiteral("%1; %2").arg(t.claim(EngineClaim::Disabled), advice);
        };
        if (t.installed) {
            if (auto r = t.barrier(true, true)) return *r;
            service = serviceState();
        }
        // The folder is moved aside or replaced only once the engine is proven stopped: engine.lock held, or
        // the engine-free proof the barrier ran when no hold was possible, or no service installed. The gate
        // refuses FolderRepair without one of them.
        t.enter(Phase::Folder, kFiles);
        t.trusted = false;
        t.opened = t.seams.openMutating(TrustMode::Repair, t.gate);
        t.fileChanges += t.opened.fileChanges;
        if (t.opened.refused) throw *t.opened.refused;
        t.trusted = t.opened.usable;
        // A late folder call is judged before the lock pair is recovered (D57; round 2).
        t.checkLate();
        if (!t.opened.usable)
            return t.make(Outcome::NothingChanged, QStringLiteral("repair the data folder"),
                          QStringLiteral("The data folder was not repaired: %1 Service: %2.").arg(t.opened.error, service) +
                              t.folderNote(),
                          service);
        noSavedSettings = !t.opened.inspection.children.contains(QString::fromLatin1(ConfigFiles::kConfig));
        if (t.installed) {
            if (auto r = t.recoverLockPair()) return *r;
            service = serviceState();
        }
    } catch (const Late& late) {
        return t.stopped(late);
    } catch (const GateRefusal& g) {
        return t.stopped(g);
    }
    t.releaseEngineLock();
    const QString files = t.fileChanges.isEmpty() ? QStringLiteral("already trusted, nothing changed")
                                                  : t.fileChanges.join(QStringLiteral("; "));
    return t.make(Outcome::Ok, QString(), QStringLiteral("Repair finished. Folder: %1. Service: %2.").arg(files, service),
                  service);
}

}  // namespace asb
