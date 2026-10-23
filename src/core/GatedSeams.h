#pragma once

// The only route from the Applier and the Barrier to the platform seams (WS-5 round 3).
//
// Every method that makes an irreversible change takes the GateToken of exactly its step, and only
// IrreversibleGate::admit can mint one (its constructor is private), so code in the transaction cannot
// reach a mutation without passing the gate: forging a token does not compile, and a token admitted for
// another step is refused here at run time (GateToken::spend). Applier.cpp and Barrier.cpp hold no
// Platform: tst_IrreversibleGate::noDirectIrreversibleCalls scans them for any Platform field access or raw
// seam mutator, so a new step cannot be added beside this class by accident.
//
// The seams themselves (Interfaces.h) are unchanged: WS-6, WS-7 and WS-8 implement them as before. The
// gate is a property of the core's transaction, not of the adapters.

#include "core/AtomicJson.h"
#include "core/ConfigFiles.h"
#include "core/Interfaces.h"
#include "core/IrreversibleGate.h"
#include "core/TrustedConfig.h"

#include <chrono>
#include <memory>
#include <optional>

namespace asb {

class GatedSeams {
public:
    explicit GatedSeams(Platform& p) : m_p(p) {}

    // ---- not irreversible (IrreversibleGate.h lists why): reads, lock acquisition, the R5 temp,
    //      directory syncs, the firewall connection -----------------------------------------------------
    std::unique_ptr<LockHold> acquireApplyLock(std::chrono::seconds timeout);
    // Why the last acquireApplyLock returned nullptr when it was not a timeout (IApplyLock::refusalReason; WS-6).
    QString applyLockRefusalReason() const;
    Installation detect(Deadline);
    ServiceQuery query(Deadline);
    OpenedFolder openReadOnly();
    ConfigFiles::Loaded readCommitted(const Inspection&);
    ConfigFiles::Loaded readPending(const Inspection&);
    bool syncFolder(QString* err);
    std::unique_ptr<TempFile> createConfigTemp(QString* err);
    std::unique_ptr<LockHold> acquireEngineLock(Deadline, LockResult* why);
    LockFileRead readHeldLock();
    LockFileRead peekLock();
    LockFileRead readRecord();
    bool lockFilesystemSupported(QString* why);
    QStringList lockDirectoryChildren();
    std::optional<StatusReport> readStatus();
    void connectFirewall(Deadline);
    // Uninstall (WS-16). Whether the program goes in its own step (Windows) or with the service (Linux).
    bool separateProgramRemoval() const;
    QString programFolder() const;
    // Restores what LockDisposition did: the program was kept, so its lock files stay (D69). Not a change of its own.
    bool clearLockDeletion(LockHold&, QString* err);
    // After the operation released its locks: the program folder, if it is now empty (RemoveDirectoryW never removes a
    // folder with anything in it). The lines say what was removed or kept.
    QStringList deleteEmptyProgramFolder();
    // The folders Repair moved aside beside the data folder (listed, never removed).
    QStringList asideFolders();

    // ---- irreversible: each consumes the GateToken of its own step ----------------------------------
    // FolderSecure (Mutating) or FolderRepair (Repair): every create / normalise / rename aside is admitted
    // separately, at the moment it is made. A refusal comes back in OpenedFolder::refused.
    OpenedFolder openMutating(TrustMode, IrreversibleGate&);
    bool removeOwnTemp(GateToken, const QString& name, QString* err);                  // TempSweep
    bool dropEqualPending(GateToken, QString* err);                                     // TempSweep (+ dir sync)
    IStartToken::Revocation revokeStartToken(GateToken, Deadline, QString* err);        // TokenRevoke
    void blockStarts(GateToken, Deadline);                                              // BlockStarts
    void requestEngineStop(GateToken, Deadline);                                        // StopRequest
    void installService(GateToken, InstallState, Deadline);                             // InstallService
    bool removeLockTemp(GateToken, const QString& name, QString* err);                  // LockPublish
    WriteResult publishLockFile(GateToken, const QString& name, const QByteArray& id);  // LockPublish
    WriteResult writePendingFile(GateToken, const QByteArray& bytes);                   // StagePending, SavePending
    bool removePendingFile(GateToken, QString* err);                                    // DiscardPending, DropPending
    void syncFirewall(GateToken, const BridgeConfig&, const FirewallGeneration&, Deadline);   // FirewallSync
    void rollbackFirewall(GateToken, const FirewallGeneration&, Deadline);              // FirewallSync
    TempFile::Commit commitConfig(GateToken, TempFile&);                                // ConfigCommit
    void settleServiceJobs(GateToken, Deadline);                                        // SettleJobs
    void sweepFirewall(GateToken, const FirewallGeneration& keep, Deadline);            // FirewallSweep
    IStartToken::Written publishStartToken(GateToken, const FirewallGeneration&, Deadline);   // TokenPublish
    void enableStarts(GateToken, Deadline);                                             // AllowStarts
    void startService(GateToken, Deadline);                                             // ServiceStart
    void removeFirewallRules(GateToken, Deadline);                                      // FirewallRemove
    void uninstallService(GateToken, Deadline);                                         // Uninstall
    QString publishAppsEntry(GateToken);                                                  // EntryWrite
    // Both held locks marked for deletion on close, or neither (a second failure clears the first). `engine` may be null
    // (no engine.lock to hold). false + *err: nothing is marked.
    bool markLocksForDeletion(GateToken, LockHold& apply, LockHold* engine, QString* err);   // LockDisposition
    IServiceControl::ProgramRemoval deleteProgram(GateToken, Deadline);                 // ProgramRemove
    bool removeLockRecord(GateToken, QString* err);                                     // LockRecordRemove
    // The known files of the folder the inspection trusted, deleted through its handle; then the folder itself when
    // nothing else is in it (INV-125). Anything else in it is kept and named.
    struct SettingsRemoval {
        QStringList removed, kept, failed;   // file names; `failed` as "<name> (<why>)"
        bool folderRemoved = false;
        QString folderError;                 // why the (otherwise empty) folder was not removed
    };
    SettingsRemoval removeSettings(GateToken, const Inspection&);                       // SettingsRemove
    void deleteAppsEntry(GateToken, Deadline);                                          // EntryRemove

private:
    Platform& m_p;
};

}  // namespace asb
