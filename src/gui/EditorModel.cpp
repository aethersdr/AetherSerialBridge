#include "gui/EditorModel.h"

#include "core/ConfigJson.h"

namespace asb::gui {

SaveRequest Submission::request() const
{
    SaveRequest r;
    r.config = bytes;
    r.source = source;
    r.supersedePending = supersedePending;
    return r;
}

bool EditorModel::load(const Snapshot& s)
{
    m_loadError.clear();
    m_fromPending = false;
    m_loaded = false;
    m_config = BridgeConfig{};
    m_source = Source{};
    if (s.folder == FolderView::Untrusted) {
        m_loadError = QStringLiteral("Config not loaded: the data folder isn't trusted.");
        m_tracker.reset();
        return false;
    }
    if (s.committed.present && !s.committed.config) {
        m_loadError = s.committed.error.isEmpty() ? QStringLiteral("config.json could not be read.") : s.committed.error;
        m_tracker.reset();
        return false;
    }
    // D48: a pending file equal to config.json is dropped on reload; any other readable pending file is the working
    // copy (it holds edits that were saved but not applied, INV-94).
    const bool pendingUsable = s.pending.present && s.pending.config && s.pending.bytes != s.committed.bytes;
    if (pendingUsable) {
        m_config = *s.pending.config;
        m_source = Source{Source::Pending, sha256Hex(s.pending.bytes).toLatin1()};
        m_fromPending = true;
    } else {
        m_config = s.committed.config.value_or(BridgeConfig{});
        m_source = s.committed.present ? Source{Source::Committed, sha256Hex(s.committed.bytes).toLatin1()}
                                       : Source{Source::New, {}};
        if (s.pending.present && !s.pending.config && s.pending.bytes != s.committed.bytes)
            m_loadError = QStringLiteral("config.pending.json could not be read (%1); showing config.json.").arg(s.pending.error);
    }
    m_loaded = true;
    m_tracker.reset();
    return true;
}

BridgeConfig& EditorModel::edit()
{
    m_tracker.markEdited();
    return m_config;
}

void EditorModel::setAutoStart(bool on)
{
    if (on == m_autoStart) return;
    m_autoStart = on;
    m_tracker.markEdited();
}

Submission EditorModel::submission(bool install) const
{
    Submission s;
    s.bytes = toJson(m_config);
    s.sha256 = sha256Hex(s.bytes).toLatin1();
    s.revision = m_tracker.revision();
    s.install = install;
    s.source = m_source;
    return s;
}

bool EditorModel::pendingConflicts(const QByteArray& pendingSha, const Submission& s)
{
    // The Applier's P6 rule (Applier.cpp loadAndCheckPrecedence), asked before submitting.
    if (pendingSha.isEmpty()) return false;
    const bool fromThatPending = s.source.kind == Source::Pending && s.source.sha256 == pendingSha;
    return !fromThatPending && pendingSha != s.sha256;
}

bool EditorModel::persistedMatches(const Result& r, const Submission& s)
{
    return r.persisted != Persisted::None && !s.sha256.isEmpty() && r.persistedSha256 == s.sha256;
}

void EditorModel::detachFromPending(const ConfigView& committed)
{
    m_fromPending = false;
    m_source = committed.present ? Source{Source::Committed, sha256Hex(committed.bytes).toLatin1()} : Source{Source::New, {}};
}

bool EditorModel::finish(const Result& r, const Submission& s)
{
    if (!persistedMatches(r, s)) return !m_tracker.isDirty();
    m_source = Source{r.persisted == Persisted::Committed ? Source::Committed : Source::Pending, s.sha256};
    m_fromPending = r.persisted == Persisted::Staged;
    return m_tracker.markSaved(s.revision);
}

}  // namespace asb::gui
