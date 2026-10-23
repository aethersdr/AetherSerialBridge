#include "gui/Dialogs.h"

#include "core/ThemeManager.h"
#include "gui/BridgeStyle.h"
#include "gui/CanonIndicators.h"
#include "gui/CanonWindow.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace asb::gui {
namespace {

// One modal Dialog-kind CanonWindow: header, the text, a bounded list, more text, a checkbox, the buttons (left to
// right as given; the reference's close rows put the affirmative one last). Returns the clicked choice, or an empty
// choice when the dialog was rejected: Esc, the close button, or the dialog going away (D67, INV-131).
Answer run(QWidget* parent, const Question& q)
{
    AetherSDR::CanonWindow dlg(QStringLiteral("Aether Serial Bridge"), parent, AetherSDR::CanonWindow::Kind::Dialog);
    dlg.setObjectName(QStringLiteral("asbDialog_") + q.id);
    AetherSDR::theme::setContainer(&dlg, QStringLiteral("canon/serialBridge"));
    AetherSDR::applyCanonSheet(dlg.bodyWidget(), [] { return composedBodySheet(); });
    auto* col = new QVBoxLayout(dlg.bodyWidget());
    col->setContentsMargins(14, 12, 14, 14);
    col->setSpacing(10);
    col->addWidget(AetherSDR::makeCanonHeader(QStringLiteral("Aether Serial Bridge")));
    auto addText = [&](const QString& text, const QString& name) {
        auto* label = new QLabel(text, dlg.bodyWidget());
        label->setObjectName(name);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setMinimumWidth(420);
        col->addWidget(label);
    };
    addText(q.text, QStringLiteral("asbDialogText"));
    if (!q.list.isEmpty()) {
        // Bounded (owner debt, WS-17 §2): at most kListRows rows visible, the rest scrolls; never one long label.
        auto* list = new QListWidget(dlg.bodyWidget());
        list->setObjectName(QStringLiteral("asbDialogList"));
        list->addItems(q.list);
        list->setSelectionMode(QAbstractItemView::NoSelection);
        list->setFocusPolicy(Qt::NoFocus);
        list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        const int rows = int(std::min<qsizetype>(q.list.size(), Question::kListRows));
        const int rowHeight = std::max(list->sizeHintForRow(0), list->fontMetrics().height() + 4);
        list->setFixedHeight(rows * rowHeight + 2 * list->frameWidth() + 4);
        col->addWidget(list);
    }
    if (!q.after.isEmpty()) addText(q.after, QStringLiteral("asbDialogAfter"));
    QCheckBox* box = nullptr;
    if (!q.checkbox.isEmpty()) {
        box = new QCheckBox(q.checkbox, dlg.bodyWidget());
        box->setObjectName(QStringLiteral("asbDialogCheck"));
        box->setChecked(false);   // unticked every time the dialog opens
        col->addWidget(box);
    }
    auto* row = new QHBoxLayout;
    row->addStretch(1);
    QPushButton* focus = nullptr;
    for (qsizetype i = 0; i < q.choices.size(); ++i) {
        const Choice& c = q.choices.at(i);
        auto* b = new QPushButton(c.label, dlg.bodyWidget());
        b->setObjectName(QStringLiteral("asbDialogChoice_") + c.id);
        // Only the focused button is the default: Return presses the safe choice, never the destructive one.
        b->setAutoDefault(c.id == q.focus);
        b->setDefault(c.id == q.focus);
        if (c.id == q.destructive) b->setProperty("tone", QStringLiteral("bad"));
        row->addWidget(b);
        const int code = int(i) + 2;   // 0 = Rejected (Esc, close), 1 = Accepted (never used here)
        QObject::connect(b, &QPushButton::clicked, &dlg, [&dlg, code] { dlg.done(code); });
        if (c.id == q.focus) focus = b;
    }
    col->addLayout(row);
    if (focus) focus->setFocus();
    dlg.setModal(true);
    QPointer<QWidget> guard(&dlg);
    const int code = dlg.exec();
    Answer a;
    if (code >= 2 && code - 2 < q.choices.size()) a.choice = q.choices.at(code - 2).id;
    a.checked = box && guard && box->isChecked();
    return a;
}

}  // namespace

bool CanonAsker::confirm(const QString& id, const QString& text, const QString& yes, const QString& no)
{
    Question q;
    q.id = id;
    q.text = text;
    if (!no.isEmpty()) q.choices << Choice{QStringLiteral("no"), no};
    q.choices << Choice{QStringLiteral("yes"), yes};
    // The safe choice has the focus: Return on an unread question never says yes.
    q.focus = no.isEmpty() ? QStringLiteral("yes") : QStringLiteral("no");
    return run(m_parent, q).is("yes");
}

void CanonAsker::notify(const QString& id, const QString& text)
{
    confirm(id, text, QStringLiteral("OK"), QString());
}

Answer CanonAsker::ask(const Question& q)
{
    return run(m_parent, q);
}

}  // namespace asb::gui
