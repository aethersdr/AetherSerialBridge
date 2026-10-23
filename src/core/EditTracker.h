#pragma once

// Unsaved-changes state for an editor (INV-13 is closed in WS-10).
// Every edit bumps a revision; a save clears "dirty" only if nothing was edited since the revision
// that save captured, so edits made while a save runs are never silently discarded. The revision only
// ever moves forward (a reload does not rewind it).

#include <QtGlobal>

namespace asb {

class EditTracker {
public:
    quint64 revision() const { return m_revision; }
    bool isDirty() const { return m_dirty; }
    void markEdited();
    // The config was just (re)loaded: nothing is unsaved. The revision is kept.
    void reset();
    // A save of `savedRevision` succeeded (WS-10: persisted ≠ None with a matching digest). Returns
    // whether everything is now saved.
    bool markSaved(quint64 savedRevision);

private:
    quint64 m_revision = 0;
    bool m_dirty = false;
};

}  // namespace asb
