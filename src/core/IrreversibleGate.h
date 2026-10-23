#pragma once

// One gate for every irreversible step of the Applier, the Barrier and lock-pair recovery (WS-5 round 3).
//
// Three review rounds in a row found the same class of defect: an irreversible step ran before its proof,
// or after its phase deadline had passed unjudged. Patching instances did not converge, so the guarantee
// is structural:
//  - stepTable() declares, once, every irreversible step: the proofs it needs and the phase whose deadline
//    it must be inside. It is the only statement of audit (b), and tst_IrreversibleGate generates its rows
//    from it (every step x every precondition).
//  - IrreversibleGate::admit(step) checks that declaration against the operation's Proofs at the moment of
//    the call and either mints a GateToken or throws GateRefusal. Nothing else can mint a GateToken.
//  - GatedSeams (GatedSeams.h) is the only route from the Applier and the Barrier to a platform seam, and
//    each of its mutating methods takes the GateToken for exactly its step.
//
// Deliberately NOT gated (each changes nothing a later reader depends on; the WS-5 doc lists them under
// "Not gated, each with its reason"): the R5 temp (invisible until R7 renames it; its destructor deletes
// only that temp), directory syncs (they make durable what is already visible), the firewall connection,
// apply.lock and engine.lock acquisition and release, and every read. The firewall connection is a proof
// instead (NeedFirewallConnected), which every firewall step needs.

#include "core/EngineClaim.h"
#include "core/Interfaces.h"
#include "core/PhaseClock.h"

#include <QString>

#include <array>
#include <functional>
#include <optional>

namespace asb {

enum class Step {
    FolderSecure,     // P4 (Mutating): create the data folder, or normalise its owner and ACL / mode
    FolderRepair,     // Repair mode: rename an untrusted folder aside, create a fresh one, normalise
    TempSweep,        // P5: remove our own stale temps; drop a pending file equal to config.json
    TokenRevoke,      // B0: overwrite start.token with the revoked form, then delete it
    BlockStarts,      // B1: starts blocked (disabled + flushed / masked)
    StopRequest,      // B2: stop requested
    InstallService,   // R3: binary and service, starts blocked
    LockPublish,      // lock-pair recovery: lock-directory temps removed; engine.lock / engine.lock.id published
    StagePending,     // R4: N written to config.pending.json
    SavePending,      // Edit, SaveAndRemove, Deactivate: the pending write (before any barrier, by design: D38)
    DiscardPending,   // DiscardPending: config.pending.json deleted
    FirewallSync,     // R6: the rules synced from B (and the rollback of a failed sync)
    ConfigCommit,     // R7: the temp renamed over config.json
    DropPending,      // R8: config.pending.json deleted after the commit
    SettleJobs,       // before R9 and before uninstall: outstanding service jobs waited for or cancelled (Linux)
    FirewallSweep,    // R9: every rule of another generation deleted
    TokenPublish,     // R9: a fresh start.token
    AllowStarts,      // R9: starts allowed (enable / unmask)
    ServiceStart,     // R9: the service started
    FirewallRemove,   // SaveAndRemove, Uninstall: every rule of the group deleted
    Uninstall,        // SaveAndRemove, Uninstall: the service removed (Linux: with its program)
    // WS-16 (D63, D69; INV-125, INV-126)
    EntryWrite,       // R3: the Apps & features entry naming this build's installed program (Windows)
    LockDisposition,  // Uninstall: apply.lock and engine.lock marked for deletion when their holds close (Windows)
    ProgramRemove,    // Uninstall: the program removed, or this process's running image renamed aside and scheduled
    LockRecordRemove, // Uninstall: engine.lock.id deleted, once the program is gone (Windows)
    SettingsRemove,   // Uninstall --remove-settings: the known files of the trusted data folder deleted, then the folder if empty
    EntryRemove,      // Uninstall: the Apps & features entry removed, last, once the program it names is gone (Windows)
};
inline constexpr int kStepCount = 27;

// The proofs a step can require. The deadline is not in this set: every step outside Phase::Cleanup must
// be inside its phase's deadline, and no earlier phase may have overrun its own (Proofs::overrun).
// apply.lock is not in it either: the operation's Txn holds it from P1 until it is destroyed, around every
// step (INV-24, tst_Applier::lockHeldAcrossEveryStep).
enum Need : unsigned {
    NeedTrustedFolder = 1u << 0,   // this operation found the data folder trusted (P4, Repair, or ReadOnly for B0)
    NeedEngineStopped = 1u << 1,   // engine.lock held by this operation WITH a matching identity (LockMatching),
                                   // or the engine-free proof, or no service. A hold whose identity does not
                                   // match proves nothing: an old engine may hold another (unlinked) inode
    NeedEngineFree = 1u << 2,      // the engine-free proof itself: starts blocked by this operation, the service
                                   // stopped and its process set empty (master §4.6 "Lock identity")
    NeedLockMatching = 1u << 3,    // engine.lock held, and the id read through that hold matches the record,
                                   // judged inside the deadline of the phase that read it
    NeedCommitted = 1u << 4,       // config.json durably holds the bytes the firewall was synced from
    NeedFirewallConnected = 1u << 5,   // this operation's IFirewall::connect returned, inside its phase deadline
                                       // (Windows: COM set up and ConnectServer done; WS-5 round 4)
    NeedProgramGone = 1u << 6,     // this uninstall removed the installed program, or renamed its running image aside and
                                   // scheduled it, or found none (WS-16: the settings, engine.lock.id and the Apps &
                                   // features entry go only after it, so a kept entry always names a program that exists)
};
inline constexpr std::array<Need, 7> kNeeds{NeedTrustedFolder, NeedEngineStopped, NeedEngineFree, NeedLockMatching,
                                             NeedCommitted, NeedFirewallConnected, NeedProgramGone};

struct StepRule {
    Step step;
    const char* id;      // the enumerator's name, for test rows and reports
    const char* name;    // what the step does, for messages
    unsigned needs;      // Need bits
    Phase phase;         // the phase whose deadline the step must be inside
    Phase otherPhase;    // a second phase it may run in (Phase::None: none)
    bool cleanup;        // failClosed may run it in Phase::Cleanup: best effort, no deadline (needs still apply); also
                         // EntryRemove, which a stopped uninstall runs there once the program is gone (WS-16)
};

// The one table (audit (b)). Indexed by Step.
const std::array<StepRule, kStepCount>& stepTable();
const StepRule& ruleFor(Step);
QString stepName(Step);
QString stepId(Step);
QString needName(Need);
QString needNames(unsigned needs);

// What the operation has proven, read by the gate at the moment of each admission.
struct Proofs {
    Phase phase = Phase::None;
    Deadline phaseDeadline{};
    std::optional<Phase> overrun;   // an earlier phase that was left after its deadline had passed
    bool trustedFolder = false;
    bool engineLockHeld = false;
    bool engineFree = false;
    bool notInstalled = false;
    bool lockMatching = false;
    bool committed = false;
    bool firewallConnected = false;
    bool programGone = false;
    unsigned have() const;          // the Need bits these proofs satisfy
    // What proves the engine stopped, if anything. The one derivation of EngineStopped: have() grants that
    // need from it, and every stop wording of a result is made from it (EngineClaim.h; WS-5 round 6).
    StopEvidence stopEvidence() const;
};

// Why the gate refused. Not a std::exception: no `catch (const std::exception&)` around a native call can
// swallow it; the operation's own handler turns it into its fail-closed or NothingChanged result.
struct GateRefusal {
    enum class Kind { Late, Missing, WrongPhase, WrongToken };
    Step step;
    Kind kind;
    Phase phase;            // Late: the phase that ran out; otherwise the phase the step was attempted in
    unsigned missing = 0;   // Missing: the Need bits not proven
    QString describe() const;
};

// The pure check: the refusal for `step` under `proofs` at `now`, or nullopt to admit it.
std::optional<GateRefusal> judge(Step, const Proofs&, Deadline now);

// Proof that the gate admitted one step at one moment. Move-only, single use; its constructor is private
// to IrreversibleGate, so code that has not been through admit() cannot hold one.
class GateToken {
public:
    GateToken(GateToken&& o) noexcept : m_step(o.m_step), m_live(o.m_live) { o.m_live = false; }
    GateToken& operator=(GateToken&&) = delete;
    GateToken(const GateToken&) = delete;
    GateToken& operator=(const GateToken&) = delete;
    ~GateToken() = default;
    // Consumes the token for `expected` (or either of two steps that share one seam method); throws
    // GateRefusal{WrongToken} for any other step or a spent token.
    void spend(Step expected) { spend(expected, expected); }
    void spend(Step a, Step b);

private:
    friend class IrreversibleGate;
    explicit GateToken(Step s) : m_step(s) {}
    Step m_step;
    bool m_live = true;
};

class IrreversibleGate {
public:
    using ProofSource = std::function<Proofs()>;
    IrreversibleGate(IClock& clock, ProofSource proofs) : m_clock(clock), m_proofs(std::move(proofs)) {}

    // Admits `step` now, or throws GateRefusal. Every irreversible action passes through here first.
    GateToken admit(Step step);
    // A step other than FolderSecure and TempSweep was admitted: from here a stop is a change, not
    // NothingChanged (those two are P4/P5, which master §4.6 runs before anything else and reports in
    // fileChanges).
    bool changedBeyondPrologue() const { return m_changed; }
    // Called after each admission (the Txn retires proofs a step ends, e.g. engine-free after AllowStarts).
    std::function<void(Step)> onAdmitted;

private:
    IClock& m_clock;
    ProofSource m_proofs;
    bool m_changed = false;
};

}  // namespace asb
