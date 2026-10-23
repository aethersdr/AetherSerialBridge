#pragma once

// The TUI's help (owner request 2026-10-19: "we need some help in the tui about everything"): what each field of the
// edit screen means, with an example, and every key every screen answers. The `?` page and the edit screen's hint
// line are built from these tables, and the tests hold them to the form's fields (Field, kFieldCount) and to the
// keys the controller actually answers, so a new field or key without help fails a test.

#include "tui/TuiState.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace asb::tui {

// The form field's label, as the edit screen shows it.
QString fieldLabel(Field);
// What the focused field means, shown under the form: one short line per entry, one entry for every field but
// Allowed on Windows, which shows the window's example line and then its sentence (help::allowedExampleLine,
// help::allowedEffect). That example line holds a middle dot: the edit screen draws it as '-' without UTF-8.
QStringList fieldHint(Field, bool windows);
// The help page's paragraph for the field: what it means, with an example.
QString fieldAbout(Field, bool windows);

// One row of a screen's keys: how the help shows them, the key names it covers (the tests' names: "Up", "Down",
// "Enter", "Esc", "Tab", "Shift+Tab", "PgUp", "PgDn", "Backspace", "Space", "Ctrl+C", or the character itself),
// and what they do.
struct KeyHelp {
    QString shown;
    QStringList keys;
    QString does;
};
// Every key the screen answers (Confirm: every question; Page: list ports, the log and this help).
QList<KeyHelp> keyHelp(Screen);

struct HelpPage {
    QStringList lines;
    QList<int> headings;   // indices into lines
};
// The `?` page. `readOnlyReason` is the banner when not elevated (empty when elevated).
HelpPage helpPage(bool windows, const QString& readOnlyReason, const QString& dataFolder, const QString& version);

}  // namespace asb::tui
