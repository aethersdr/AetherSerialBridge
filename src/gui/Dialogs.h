#pragma once

// Confirmations and notices (WS-10 §5.4 "confirm/notify: Dialog-kind CanonWindow, modal"). The window asks
// through IAsker so the flows can be driven by tests; every question has a stable id the tests name:
//
//   exposure        a port listens on every interface with no client restriction (INV-35)
//   untick          Save & apply with the auto-start box unticked on an installed service (INV-35)
//   supersede       config.pending.json changed since it was loaded (INV-102)
//   repair          move the untrusted folder aside (D26, S2)
//   discardPending  drop the saved, not applied settings
//   discardEdits    unsaved edits would be lost (close, Escape, Ctrl+W, Reload; INV-13)
//   invalid         notice: fix these before saving
//   busy            notice: an operation is still finishing; the window can close when it ends (D57)
//
// The lifecycle questions (v1.1 WS-17 §2, D67, INV-131) are asked through ask(): several choices, one with the focus,
// and Esc, the close button or the dialog going away answer Cancelled (an empty choice), which changes nothing:
//
//   build           Save & apply on another build: update / keep (focus) / cancel
//   install         Install the bridge service: install / cancel (focus)
//   update          Update to this build: update / cancel (focus)
//   uninstall       Uninstall…: uninstall (destructive) / cancel (focus), with "Also remove my settings" (unticked)
//   restartAdmin    Restart as administrator with unsaved changes: restart / cancel (focus)
//
// A question's `list` (the ports a question names) is shown as a bounded, scrollable list, never as one long label.

#include <QList>
#include <QString>
#include <QStringList>

class QWidget;

namespace asb::gui {

struct Choice {
    QString id;      // what ask() returns ("update", "keep", "cancel", ...)
    QString label;   // the button
};

struct Question {
    QString id;
    QString text;                   // above the list
    QStringList list;               // shown bounded (at most kListRows rows visible), scrollable
    QString after;                  // below the list
    QList<Choice> choices;          // left to right
    QString focus;                  // the choice with the focus (Return on an unread question): always a safe one
    QString destructive;            // the choice drawn as destructive, if any
    QString checkbox;               // a checkbox's text, if any; it starts unticked every time
    static constexpr int kListRows = 8;
};

struct Answer {
    QString choice;                 // the chosen id; empty = Cancelled (Esc, close, the dialog destroyed)
    bool checked = false;           // the checkbox, when the question has one
    bool is(const char* id) const { return choice == QLatin1String(id); }
};

class IAsker {
public:
    virtual ~IAsker() = default;
    // true = the affirmative choice (`yes`).
    virtual bool confirm(const QString& id, const QString& text, const QString& yes, const QString& no) = 0;
    virtual void notify(const QString& id, const QString& text) = 0;
    // The lifecycle questions. The default answers Cancelled (an asker that cannot ask changes nothing).
    virtual Answer ask(const Question&) { return {}; }
};

// The product's: a modal Dialog-kind CanonWindow over `parent`, styled like the window body.
class CanonAsker final : public IAsker {
public:
    explicit CanonAsker(QWidget* parent) : m_parent(parent) {}
    bool confirm(const QString& id, const QString& text, const QString& yes, const QString& no) override;
    void notify(const QString& id, const QString& text) override;
    Answer ask(const Question&) override;

private:
    QWidget* m_parent;
};

}  // namespace asb::gui
