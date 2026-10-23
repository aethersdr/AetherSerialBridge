#include "core/IrreversibleGate.h"

#include <QStringList>

namespace asb {
namespace {

constexpr unsigned kNone = 0;

// Audit (b) as data. A step's needs are the proofs that must hold at the moment it is admitted; its phase
// is the one whose deadline it must be inside. `cleanup` marks the barrier steps failClosed repeats as a
// best-effort cleanup after a failure: there no deadline applies (D57 asks for the cleanup precisely when
// something ran late), but the needs still do.
constexpr std::array<StepRule, kStepCount> kTable{{
    {Step::FolderSecure, "FolderSecure", "create or normalise the data folder", kNone, Phase::Prologue, Phase::None, false},
    {Step::FolderRepair, "FolderRepair", "repair the data folder", NeedEngineStopped, Phase::Folder, Phase::None, false},
    {Step::TempSweep, "TempSweep", "remove stale temps", NeedTrustedFolder, Phase::Prologue, Phase::Folder, false},
    {Step::TokenRevoke, "TokenRevoke", "revoke the start token", NeedTrustedFolder, Phase::Barrier, Phase::None, true},
    {Step::BlockStarts, "BlockStarts", "block service starts", kNone, Phase::Barrier, Phase::None, true},
    {Step::StopRequest, "StopRequest", "request the service to stop", kNone, Phase::Barrier, Phase::None, true},
    {Step::InstallService, "InstallService", "install the service", NeedEngineStopped, Phase::Install, Phase::None, false},
    {Step::LockPublish, "LockPublish", "publish the engine lock", NeedEngineFree, Phase::Install, Phase::None, false},
    {Step::StagePending, "StagePending", "save the settings", NeedTrustedFolder | NeedLockMatching, Phase::Stage, Phase::None, false},
    {Step::SavePending, "SavePending", "save the settings", NeedTrustedFolder, Phase::Stage, Phase::None, false},
    {Step::DiscardPending, "DiscardPending", "discard the saved settings", NeedTrustedFolder, Phase::Stage, Phase::None, false},
    {Step::FirewallSync, "FirewallSync", "update the firewall", NeedLockMatching | NeedFirewallConnected, Phase::Firewall,
     Phase::None, false},
    {Step::ConfigCommit, "ConfigCommit", "commit the new settings", NeedTrustedFolder | NeedLockMatching, Phase::Commit, Phase::None,
     false},
    {Step::DropPending, "DropPending", "delete the applied pending file", NeedTrustedFolder | NeedLockMatching | NeedCommitted,
     Phase::Commit, Phase::None, false},
    {Step::SettleJobs, "SettleJobs", "settle outstanding service jobs", NeedEngineStopped, Phase::Firewall, Phase::Remove, false},
    {Step::FirewallSweep, "FirewallSweep", "remove stale firewall rules", NeedLockMatching | NeedCommitted | NeedFirewallConnected,
     Phase::Firewall, Phase::None, false},
    {Step::TokenPublish, "TokenPublish", "write the start token", NeedTrustedFolder | NeedLockMatching | NeedCommitted,
     Phase::Activate, Phase::None, false},
    {Step::AllowStarts, "AllowStarts", "enable the service", NeedCommitted, Phase::Activate, Phase::None, false},
    {Step::ServiceStart, "ServiceStart", "start the service", NeedCommitted, Phase::Activate, Phase::None, false},
    {Step::FirewallRemove, "FirewallRemove", "remove the firewall rules", NeedEngineStopped | NeedFirewallConnected, Phase::Remove,
     Phase::None, false},
    {Step::Uninstall, "Uninstall", "remove the service", NeedEngineStopped, Phase::Remove, Phase::None, false},
    {Step::EntryWrite, "EntryWrite", "write the Apps & features entry", NeedEngineStopped, Phase::Install, Phase::None, false},
    {Step::LockDisposition, "LockDisposition", "mark the lock files for removal", NeedEngineStopped, Phase::Remove,
     Phase::None, false},
    {Step::ProgramRemove, "ProgramRemove", "remove the program", NeedEngineStopped, Phase::Remove, Phase::None, false},
    {Step::LockRecordRemove, "LockRecordRemove", "remove engine.lock.id", NeedEngineStopped | NeedProgramGone, Phase::Remove,
     Phase::None, false},
    {Step::SettingsRemove, "SettingsRemove", "remove the settings", NeedTrustedFolder | NeedEngineStopped | NeedProgramGone,
     Phase::Remove, Phase::None, false},
    // Also a cleanup step: once the program is gone, an uninstall that stops for lateness still removes the entry, so it
    // never names a program that is not there (INV-126). Its needs still apply.
    {Step::EntryRemove, "EntryRemove", "remove the Apps & features entry", NeedEngineStopped | NeedProgramGone, Phase::Remove,
     Phase::None, true},
}};

constexpr bool tableIndexedByStep()
{
    for (int i = 0; i < kStepCount; ++i)
        if (int(kTable[size_t(i)].step) != i) return false;
    return true;
}
static_assert(tableIndexedByStep(), "stepTable() is indexed by Step: one row per step, in enum order");

}  // namespace

const std::array<StepRule, kStepCount>& stepTable()
{
    return kTable;
}

const StepRule& ruleFor(Step s)
{
    return stepTable()[size_t(s)];
}

QString stepName(Step s)
{
    return QString::fromLatin1(ruleFor(s).name);
}

QString stepId(Step s)
{
    return QString::fromLatin1(ruleFor(s).id);
}

QString needName(Need n)
{
    switch (n) {
    case NeedTrustedFolder: return QStringLiteral("a trusted data folder");
    case NeedEngineStopped: return QStringLiteral("that the engine stopped");
    case NeedEngineFree: return QStringLiteral("that no engine runs");
    case NeedLockMatching: return QStringLiteral("the engine lock held with a matching identity");
    case NeedCommitted: return QStringLiteral("durably committed settings");
    case NeedFirewallConnected: return QStringLiteral("a connection to the firewall");
    case NeedProgramGone: return QStringLiteral("that the program was removed");
    }
    return {};
}

QString needNames(unsigned needs)
{
    QStringList parts;
    for (Need n : kNeeds)
        if (needs & n) parts << needName(n);
    return parts.join(QStringLiteral(", "));
}

StopEvidence Proofs::stopEvidence() const
{
    // A hold proves the engine stopped only once its identity matched: acquiring an engine.lock that is not
    // the recorded one says nothing about an engine holding another, unlinked inode.
    if (engineLockHeld && lockMatching) return StopEvidence::ValidatedHold;
    if (engineFree) return StopEvidence::EngineFree;
    if (notInstalled) return StopEvidence::NotInstalled;
    return StopEvidence::None;
}

unsigned Proofs::have() const
{
    unsigned h = 0;
    if (trustedFolder) h |= NeedTrustedFolder;
    if (stopEvidence() != StopEvidence::None) h |= NeedEngineStopped;
    if (engineFree) h |= NeedEngineFree;
    if (stopEvidence() == StopEvidence::ValidatedHold) h |= NeedLockMatching;
    if (committed) h |= NeedCommitted;
    if (firewallConnected) h |= NeedFirewallConnected;
    if (programGone) h |= NeedProgramGone;
    return h;
}

QString GateRefusal::describe() const
{
    switch (kind) {
    case Kind::Late:
        return QStringLiteral("a call while %1 returned after its deadline, so the operation stopped there").arg(phaseName(phase));
    case Kind::Missing:
        return QStringLiteral("could not prove %1 before it could %2, so the operation stopped there")
            .arg(needNames(missing), stepName(step));
    case Kind::WrongPhase:
        return QStringLiteral("the step \"%1\" was reached while %2, so the operation stopped there")
            .arg(stepName(step), phaseName(phase));
    case Kind::WrongToken:
        return QStringLiteral("the step \"%1\" was reached without its admission, so the operation stopped there")
            .arg(stepName(step));
    }
    return {};
}

std::optional<GateRefusal> judge(Step step, const Proofs& p, Deadline now)
{
    const StepRule& rule = ruleFor(step);
    const bool cleanup = p.phase == Phase::Cleanup;
    // The step runs only in its declared phase: that phase's deadline is the one judged below.
    const bool declared = p.phase == rule.phase || (rule.otherPhase != Phase::None && p.phase == rule.otherPhase);
    if (cleanup ? !rule.cleanup : !declared) return GateRefusal{step, GateRefusal::Kind::WrongPhase, p.phase, 0};
    if (!cleanup) {
        // A phase that was left late is never forgiven by the next phase's fresh deadline.
        if (p.overrun) return GateRefusal{step, GateRefusal::Kind::Late, *p.overrun, 0};
        if (now > p.phaseDeadline) return GateRefusal{step, GateRefusal::Kind::Late, p.phase, 0};
    }
    if (const unsigned missing = rule.needs & ~p.have())
        return GateRefusal{step, GateRefusal::Kind::Missing, p.phase, missing};
    return std::nullopt;
}

void GateToken::spend(Step a, Step b)
{
    if (!m_live || (m_step != a && m_step != b)) throw GateRefusal{a, GateRefusal::Kind::WrongToken, Phase::None, 0};
    m_live = false;
}

GateToken IrreversibleGate::admit(Step step)
{
    if (std::optional<GateRefusal> r = judge(step, m_proofs(), m_clock.now())) throw *r;
    if (step != Step::FolderSecure && step != Step::TempSweep) m_changed = true;
    if (onAdmitted) onAdmitted(step);
    return GateToken(step);
}

}  // namespace asb
