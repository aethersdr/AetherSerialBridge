#pragma once

// The ONE way to read configuration (master §4.4). The folder is
// inspected, and then secured, created, renamed aside or refused according to the caller's mode, before
// anything in it is read. This translation unit is the only one that references the ConfigFiles read
// functions (INV-20, checked on the object files by cmake/ConfigReadSymbols.cmake).
//
// Mode table (master §4.4): ReadOnly never writes (INV-44); Mutating creates or normalises and refuses an
// untrusted folder ("run Repair"); Repair renames an untrusted folder aside and starts a fresh one,
// deleting nothing (INV-21, INV-74); Service and Scratch only read, and refuse anything but Trust
// without changing anything (INV-22).

#include "core/ConfigFiles.h"
#include "core/Interfaces.h"
#include "core/IrreversibleGate.h"

#include <QString>
#include <QStringList>

namespace asb {

struct OpenedFolder {
    bool usable = false;       // the folder is trusted (after any change the mode allowed) and may be read
    bool missing = false;      // nothing at the path (ReadOnly: "not created yet")
    Inspection inspection;     // the inspection reads go through
    QString error;             // why it is not usable
    QStringList fileChanges;   // what this call changed on disk (created, normalised, moved aside)
    // The gate refused one of the changes (FolderMutations threw it): the caller rethrows it after
    // recording fileChanges, which still lists what was done before the refusal.
    std::optional<GateRefusal> refused;
};

// The three changes a mutating open can make. The Applier passes GatedSeams' implementation, which admits
// each one through the IrreversibleGate (WS-5 round 3); the two-argument open() calls the folder directly.
class FolderMutations {
public:
    virtual ~FolderMutations() = default;
    virtual void createLockedDown() = 0;
    virtual QStringList normalise(const Inspection&) = 0;
    virtual QString renameAside(const Inspection&) = 0;
};

namespace TrustedConfig {

OpenedFolder open(IDataFolder&, TrustMode);
// Mutating and Repair modes with every change made through `mutations` (the read-only modes ignore it).
OpenedFolder open(IDataFolder&, TrustMode, FolderMutations& mutations);

// config.json through the inspected folder. Missing loads as an empty config.
ConfigFiles::Loaded readCommitted(IDataFolder&, const Inspection&);
// config.pending.json through the inspected folder. Missing: present = false.
ConfigFiles::Loaded readPending(IDataFolder&, const Inspection&);
// A scratch config by its own file name, through a folder opened in Scratch mode (WS-9 --config <file>). Missing
// loads as an empty config.
ConfigFiles::Loaded readScratch(IDataFolder&, const Inspection&, const QString& fileName);

}  // namespace TrustedConfig
}  // namespace asb
