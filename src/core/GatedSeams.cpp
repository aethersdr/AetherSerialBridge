#include "core/GatedSeams.h"

#include "core/LockIdentity.h"

namespace asb {
namespace {

QString pendingName()
{
    return QString::fromLatin1(ConfigFiles::kPending);
}

// The folder mutations of a mutating open, each admitted by the gate at the moment it is made.
class GatedFolderMutations final : public FolderMutations {
public:
    GatedFolderMutations(IDataFolder& f, IrreversibleGate& g, Step s) : m_f(f), m_gate(g), m_step(s) {}
    void createLockedDown() override
    {
        m_gate.admit(m_step).spend(m_step);
        m_f.createLockedDown();
    }
    QStringList normalise(const Inspection& i) override
    {
        m_gate.admit(m_step).spend(m_step);
        return m_f.normalise(i);
    }
    QString renameAside(const Inspection& i) override
    {
        m_gate.admit(Step::FolderRepair).spend(Step::FolderRepair);   // only Repair mode moves a folder aside
        return m_f.renameAside(i);
    }

private:
    IDataFolder& m_f;
    IrreversibleGate& m_gate;
    Step m_step;
};

}  // namespace

// ---- not irreversible ------------------------------------------------------------------------------
std::unique_ptr<LockHold> GatedSeams::acquireApplyLock(std::chrono::seconds timeout)
{
    return m_p.applyLock.acquire(timeout);
}
QString GatedSeams::applyLockRefusalReason() const
{
    return m_p.applyLock.refusalReason();
}
Installation GatedSeams::detect(Deadline d)
{
    return m_p.service.detect(d);
}
ServiceQuery GatedSeams::query(Deadline d)
{
    return m_p.service.query(d);
}
bool GatedSeams::separateProgramRemoval() const
{
    return m_p.service.separateProgramRemoval();
}
QString GatedSeams::programFolder() const
{
    return m_p.service.programFolder();
}
bool GatedSeams::clearLockDeletion(LockHold& h, QString* err)
{
    return h.clearDeletion(err);
}
QStringList GatedSeams::deleteEmptyProgramFolder()
{
    return m_p.service.removeProgramFolder();
}
QStringList GatedSeams::asideFolders()
{
    return m_p.folder.asideFolders();
}
OpenedFolder GatedSeams::openReadOnly()
{
    return TrustedConfig::open(m_p.folder, TrustMode::ReadOnly);
}
ConfigFiles::Loaded GatedSeams::readCommitted(const Inspection& i)
{
    return TrustedConfig::readCommitted(m_p.folder, i);
}
ConfigFiles::Loaded GatedSeams::readPending(const Inspection& i)
{
    return TrustedConfig::readPending(m_p.folder, i);
}
bool GatedSeams::syncFolder(QString* err)
{
    return m_p.io.syncDir(m_p.folder.handle(), err);
}
std::unique_ptr<TempFile> GatedSeams::createConfigTemp(QString* err)
{
    return m_p.io.createTemp(m_p.folder.handle(), OwnerPolicy::Config, defaultNameGen(QString::fromLatin1(ConfigFiles::kConfig)),
                             err);
}
std::unique_ptr<LockHold> GatedSeams::acquireEngineLock(Deadline d, LockResult* why)
{
    return m_p.engineLock.acquireUntil(d, why);
}
LockFileRead GatedSeams::readHeldLock()
{
    return m_p.engineLock.readHeldLock();
}
LockFileRead GatedSeams::peekLock()
{
    return m_p.engineLock.peekLock();
}
LockFileRead GatedSeams::readRecord()
{
    return m_p.engineLock.readRecord();
}
bool GatedSeams::lockFilesystemSupported(QString* why)
{
    return m_p.engineLock.filesystemSupported(why);
}
QStringList GatedSeams::lockDirectoryChildren()
{
    return m_p.engineLock.children();
}
std::optional<StatusReport> GatedSeams::readStatus()
{
    return m_p.status.read();
}
void GatedSeams::connectFirewall(Deadline d)
{
    m_p.firewall.connect(d);
}

// ---- irreversible ----------------------------------------------------------------------------------
OpenedFolder GatedSeams::openMutating(TrustMode mode, IrreversibleGate& gate)
{
    Q_ASSERT(mode == TrustMode::Mutating || mode == TrustMode::Repair);
    GatedFolderMutations m(m_p.folder, gate, mode == TrustMode::Repair ? Step::FolderRepair : Step::FolderSecure);
    return TrustedConfig::open(m_p.folder, mode, m);
}
bool GatedSeams::removeOwnTemp(GateToken t, const QString& name, QString* err)
{
    t.spend(Step::TempSweep);
    return m_p.io.remove(m_p.folder.handle(), name, err);
}
bool GatedSeams::dropEqualPending(GateToken t, QString* err)
{
    t.spend(Step::TempSweep);
    if (!m_p.io.remove(m_p.folder.handle(), pendingName(), err)) return false;
    return m_p.io.syncDir(m_p.folder.handle(), err);
}
IStartToken::Revocation GatedSeams::revokeStartToken(GateToken t, Deadline d, QString* err)
{
    t.spend(Step::TokenRevoke);
    return m_p.token.revoke(d, err);
}
void GatedSeams::blockStarts(GateToken t, Deadline d)
{
    t.spend(Step::BlockStarts);
    m_p.service.inhibitStarts(d);
}
void GatedSeams::requestEngineStop(GateToken t, Deadline d)
{
    t.spend(Step::StopRequest);
    m_p.service.requestStop(d);
}
void GatedSeams::installService(GateToken t, InstallState s, Deadline d)
{
    t.spend(Step::InstallService);
    m_p.service.installOrConfigure(s, d);
}
bool GatedSeams::removeLockTemp(GateToken t, const QString& name, QString* err)
{
    t.spend(Step::LockPublish);
    return m_p.engineLock.io().remove(m_p.engineLock.dir(), name, err);
}
WriteResult GatedSeams::publishLockFile(GateToken t, const QString& name, const QByteArray& id)
{
    t.spend(Step::LockPublish);
    return LockIdentity::publish(m_p.engineLock.io(), m_p.engineLock.dir(), name, id);
}
WriteResult GatedSeams::writePendingFile(GateToken t, const QByteArray& bytes)
{
    t.spend(Step::StagePending, Step::SavePending);
    return ConfigFiles::writePending(m_p.io, m_p.folder.handle(), bytes);
}
bool GatedSeams::removePendingFile(GateToken t, QString* err)
{
    t.spend(Step::DiscardPending, Step::DropPending);
    return m_p.io.remove(m_p.folder.handle(), pendingName(), err);
}
void GatedSeams::syncFirewall(GateToken t, const BridgeConfig& c, const FirewallGeneration& g, Deadline d)
{
    t.spend(Step::FirewallSync);
    m_p.firewall.sync(c, g, d);
}
void GatedSeams::rollbackFirewall(GateToken t, const FirewallGeneration& g, Deadline d)
{
    t.spend(Step::FirewallSync);
    m_p.firewall.rollback(g, d);
}
TempFile::Commit GatedSeams::commitConfig(GateToken t, TempFile& temp)
{
    t.spend(Step::ConfigCommit);
    return temp.commitAs(QString::fromLatin1(ConfigFiles::kConfig));
}
void GatedSeams::settleServiceJobs(GateToken t, Deadline d)
{
    t.spend(Step::SettleJobs);
    m_p.service.reconcileOutstandingJobs(d);
}
void GatedSeams::sweepFirewall(GateToken t, const FirewallGeneration& keep, Deadline d)
{
    t.spend(Step::FirewallSweep);
    m_p.firewall.sweep(keep, d);
}
IStartToken::Written GatedSeams::publishStartToken(GateToken t, const FirewallGeneration& g, Deadline d)
{
    t.spend(Step::TokenPublish);
    return m_p.token.writeFresh(g, d);
}
void GatedSeams::enableStarts(GateToken t, Deadline d)
{
    t.spend(Step::AllowStarts);
    m_p.service.allowStarts(d);
}
void GatedSeams::startService(GateToken t, Deadline d)
{
    t.spend(Step::ServiceStart);
    m_p.service.start(d);
}
void GatedSeams::removeFirewallRules(GateToken t, Deadline d)
{
    t.spend(Step::FirewallRemove);
    m_p.firewall.removeAll(d);
}
void GatedSeams::uninstallService(GateToken t, Deadline d)
{
    t.spend(Step::Uninstall);
    m_p.service.uninstall(d);
}
QString GatedSeams::publishAppsEntry(GateToken t)
{
    t.spend(Step::EntryWrite);
    return m_p.service.writeAppsEntry();
}
bool GatedSeams::markLocksForDeletion(GateToken t, LockHold& apply, LockHold* engine, QString* err)
{
    t.spend(Step::LockDisposition);
    if (!apply.markForDeletion(err)) return false;
    if (!engine || engine->markForDeletion(err)) return true;
    QString ignored;
    apply.clearDeletion(&ignored);   // neither, never one of them
    return false;
}
IServiceControl::ProgramRemoval GatedSeams::deleteProgram(GateToken t, Deadline d)
{
    t.spend(Step::ProgramRemove);
    return m_p.service.removeProgram(d);
}
bool GatedSeams::removeLockRecord(GateToken t, QString* err)
{
    t.spend(Step::LockRecordRemove);
    return m_p.engineLock.io().remove(m_p.engineLock.dir(), QString::fromLatin1(LockIdentity::kRecordName), err);
}
GatedSeams::SettingsRemoval GatedSeams::removeSettings(GateToken t, const Inspection& insp)
{
    t.spend(Step::SettingsRemove);
    SettingsRemoval r;
    const QStringList known = m_p.folder.knownFiles();
    for (const QString& name : insp.children) {
        if (!known.contains(name)) {
            r.kept << name;
            continue;
        }
        QString err;
        if (m_p.io.remove(m_p.folder.handle(), name, &err) || err == fileNotFoundError()) r.removed << name;
        else r.failed << QStringLiteral("%1 (%2)").arg(name, err);
    }
    if (!r.kept.isEmpty() || !r.failed.isEmpty()) {
        QString ignored;
        m_p.io.syncDir(m_p.folder.handle(), &ignored);   // what was deleted stays deleted after a power loss
        return r;
    }
    r.folderRemoved = m_p.folder.removeIfEmpty(insp, &r.folderError);
    return r;
}
void GatedSeams::deleteAppsEntry(GateToken t, Deadline d)
{
    t.spend(Step::EntryRemove);
    m_p.service.removeAppsEntry(d);
}

}  // namespace asb
