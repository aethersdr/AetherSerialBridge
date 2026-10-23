#pragma once

// The editor's state, without widgets (WS-10 §5.5; INV-13, INV-94, INV-102, INV-107). The window owns one; the
// table, the card and the auto-start box edit it.
//
//  - Working copy: the config being edited, loaded from config.pending.json when that file exists and differs from
//    config.json (D48: a pending file equal to config.json is ignored), else from config.json. Loading is never an
//    edit: the EditTracker is reset, the revision kept (it only moves forward).
//  - Source (INV-102): the digest of the bytes the working copy was loaded from, and whether they were the pending
//    or the committed file. A save sends it; the Applier refuses a pending file that is neither that source nor
//    the submitted bytes unless the save says supersedePending (the window asks first).
//  - Submission: the working copy serialised ONCE; the exact bytes, their sha256Hex and the revision they cover.
//  - finish(): dirty is cleared only when result.persisted != None AND result.persistedSha256 equals the
//    submission's digest (INV-107); then EditTracker::markSaved(revision), which keeps dirty if anything was edited
//    after the submission (INV-13). The outcome alone never clears dirty.

#include "core/BridgeConfig.h"
#include "core/EditTracker.h"
#include "core/Interfaces.h"
#include "gui/ViewSource.h"

#include <QByteArray>
#include <QString>

#include <optional>

namespace asb::gui {

struct Submission {
    QByteArray bytes;          // what the Applier gets, exactly
    QByteArray sha256;         // sha256Hex(bytes)
    quint64 revision = 0;      // EditTracker revision these bytes cover
    bool install = false;      // the auto-start box was ticked: Run; unticked: SaveAndRemove / Deactivate
    SaveRequest request() const;
    Source source;
    bool supersedePending = false;
};

class EditorModel {
public:
    // Load (or reload) from what is on disk. Returns false when there is nothing usable to edit (an untrusted
    // folder, an unreadable config.json): the working copy is then empty and `loadError` says why.
    bool load(const Snapshot&);

    const BridgeConfig& config() const { return m_config; }
    // Every edit goes through here: the change, then one revision.
    BridgeConfig& edit();
    void markEdited() { m_tracker.markEdited(); }
    bool dirty() const { return m_tracker.isDirty(); }
    quint64 revision() const { return m_tracker.revision(); }

    bool autoStart() const { return m_autoStart; }
    // The box changed by the user: an edit.
    void setAutoStart(bool on);
    // The real start type after a load or an operation: never an edit (INV-23).
    void setAutoStartFromService(bool on) { m_autoStart = on; }

    const Source& source() const { return m_source; }
    bool fromPending() const { return m_fromPending; }   // the working copy is config.pending.json (saved, not applied)
    bool loaded() const { return m_loaded; }
    QString loadError() const { return m_loadError; }

    // Serialises the working copy once. `install` = the auto-start box.
    Submission submission(bool install) const;
    // Whether the pending file on disk (its digest; empty = none) conflicts with this submission (INV-102): it is
    // neither the pending file the working copy came from nor the submitted bytes.
    static bool pendingConflicts(const QByteArray& pendingSha, const Submission&);

    // The final result of `s` (INV-107). Returns true when everything is saved now (dirty cleared).
    // When persisted with the matching digest, the source becomes what was persisted (Staged: pending;
    // Committed: config.json), so the next save does not conflict with the file this one wrote.
    bool finish(const Result&, const Submission& s);

    // config.pending.json was discarded while edits stay open here: the working copy no longer stands for it; its
    // source is now the committed file (or none).
    void detachFromPending(const ConfigView& committed);

    // A save of the submitted digest counts as persisted.
    static bool persistedMatches(const Result&, const Submission&);

private:
    BridgeConfig m_config;
    EditTracker m_tracker;
    Source m_source;
    bool m_fromPending = false;
    bool m_loaded = false;
    bool m_autoStart = false;
    QString m_loadError;
};

}  // namespace asb::gui
