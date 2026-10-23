#include "core/EditTracker.h"

namespace asb {

void EditTracker::markEdited()
{
    ++m_revision;
    m_dirty = true;
}

void EditTracker::reset()
{
    m_dirty = false;
}

bool EditTracker::markSaved(quint64 savedRevision)
{
    if (savedRevision == m_revision) m_dirty = false;
    return !m_dirty;
}

}  // namespace asb
