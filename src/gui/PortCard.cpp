#include "gui/PortCard.h"

#include "core/HelpText.h"
#include "gui/ComboStyle.h"
#include "gui/EditorModel.h"
#include "gui/PortTableModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QVBoxLayout>
#include <QtGlobal>

#include <functional>

namespace asb::gui {
namespace {

QLabel* fieldLabel(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setObjectName(QStringLiteral("asbFieldLabel"));
    return l;
}

QStringList itemsOf(const QComboBox* c)
{
    QStringList out;
    for (int i = 0; i < c->count(); ++i) out << c->itemText(i);
    return out;
}

// Brings a combo in line with the model without emitting a user change, and only when it differs: the
// items (the choices, plus the model's value when no choice matches it in any case) or the shown text, judged by
// `shows`. A combo that already shows the value is left alone, so a refill never lands under the user's cursor,
// selection or open list. The value is selected by its exact spelling; an editable combo whose list holds it only in
// another case shows the model's own spelling (never "COM3" for a model that holds "com3").
void syncCombo(QComboBox* c, QStringList items, const QString& value, const std::function<bool(const QString&)>& shows)
{
    const QStringList choices = items;
    if (!value.isEmpty() && !items.contains(value, Qt::CaseInsensitive)) items << value;
    // Already showing the value over the same choices (the list may also hold an earlier value of the model as its
    // extra item): nothing to do. A value typed as you go is never a choice yet, so it must not force a refill.
    const QStringList now = itemsOf(c);
    const bool sameChoices = now == items || now == choices || (now.size() == choices.size() + 1 && now.mid(0, choices.size()) == choices);
    if (sameChoices && shows(c->currentText())) return;
    const QSignalBlocker block(c);
    if (itemsOf(c) != items) {
        c->clear();
        c->addItems(items);
    }
    const int i = c->findText(value, Qt::MatchExactly | Qt::MatchCaseSensitive);
    if (i >= 0) c->setCurrentIndex(i);
    else if (c->isEditable()) c->setEditText(value);
    else c->setCurrentIndex(-1);
}

// The number a field shows: 0 for blank or not a number (what the model stores for those).
int numberIn(const QString& text)
{
    bool ok = false;
    const int v = text.trimmed().toInt(&ok);
    return ok ? v : 0;
}

}  // namespace

QStringList PortCard::baudChoices()
{
    return {QStringLiteral("1200"),   QStringLiteral("2400"),   QStringLiteral("4800"),   QStringLiteral("9600"),
            QStringLiteral("19200"),  QStringLiteral("38400"),  QStringLiteral("57600"),  QStringLiteral("115200"),
            QStringLiteral("230400"), QStringLiteral("460800"), QStringLiteral("921600")};
}

QStringList PortCard::formatChoices()
{
    return {QStringLiteral("8N1"), QStringLiteral("8E1"), QStringLiteral("8O1"), QStringLiteral("8N2"),
            QStringLiteral("7E1"), QStringLiteral("7O1"), QStringLiteral("7N1"), QStringLiteral("7E2")};
}

QString PortCard::allowedHintText(bool windows)
{
    return help::allowedExampleLine(windows) + QStringLiteral(". ") + help::allowedEffect(windows);
}

PortCard::PortCard(EditorModel& editor, bool windows, QWidget* parent) : QFrame(parent), m_editor(editor)
{
    setObjectName(QStringLiteral("asbCard"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(8);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("asbCardTitle"));
    outer->addWidget(m_title);
    m_empty = new QLabel(QStringLiteral("Select a port to edit it, or click + Add port."), this);
    m_empty->setObjectName(QStringLiteral("asbHint"));
    outer->addWidget(m_empty);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    outer->addLayout(grid);

    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("asbCardName"));
    m_name->setAccessibleName(QStringLiteral("Name"));
    m_enabled = new QCheckBox(QStringLiteral("Enabled"), this);
    m_enabled->setObjectName(QStringLiteral("asbCardEnabled"));
    m_device = new QComboBox(this);
    m_device->setObjectName(QStringLiteral("asbCardDevice"));
    m_device->setEditable(true);
    m_device->setAccessibleName(QStringLiteral("Device"));
    m_device->setToolTip(windows ? QStringLiteral("The serial port the device is on, e.g. COM3.")
                                 : QStringLiteral("The serial device. Use its /dev/serial/by-id/ name: it stays the same "
                                                  "when the device is unplugged and plugged in again, while /dev/ttyUSB "
                                                  "numbers can swap."));
    m_baud = new QComboBox(this);
    m_baud->setObjectName(QStringLiteral("asbCardBaud"));
    m_baud->setEditable(true);
    m_baud->setValidator(new QIntValidator(1, 100000000, m_baud));
    m_baud->setAccessibleName(QStringLiteral("Baud"));
    m_format = new QComboBox(this);
    m_format->setObjectName(QStringLiteral("asbCardFormat"));
    m_format->setAccessibleName(QStringLiteral("Format"));
    m_listen = new QComboBox(this);
    m_listen->setObjectName(QStringLiteral("asbCardListen"));
    m_listen->setEditable(true);
    m_listen->setAccessibleName(QStringLiteral("Listen on"));
    m_listen->setToolTip(QStringLiteral("The address on this computer that AetherSDR connects to: 127.0.0.1 for this "
                                        "computer only, one of its network addresses, or 0.0.0.0 for every network "
                                        "interface."));
    m_tcpPort = new QLineEdit(this);
    m_tcpPort->setObjectName(QStringLiteral("asbCardTcpPort"));
    m_tcpPort->setValidator(new QIntValidator(0, 65535, m_tcpPort));
    m_tcpPort->setAccessibleName(QStringLiteral("TCP port"));
    m_tcpPort->setToolTip(QStringLiteral("The TCP port AetherSDR connects to, e.g. 4001: 1 to 65535, a different one for "
                                         "each port."));
    m_allowed = new QLineEdit(this);
    m_allowed->setObjectName(QStringLiteral("asbCardAllowed"));
    m_allowed->setPlaceholderText(QStringLiteral("blank = any address"));
    m_allowed->setToolTip(windows ? QStringLiteral("Who may connect: addresses, blocks (/24) or ranges (a-b) separated by "
                                                   "commas, or a keyword such as LocalSubnet. Blank = any address.")
                                  : QStringLiteral("Who may connect: addresses, blocks (/24) or ranges (a-b) separated by "
                                                   "commas. Blank = any address."));
    m_allowed->setAccessibleName(QStringLiteral("Allowed clients"));
    m_dtr = new QCheckBox(QStringLiteral("Assert DTR"), this);
    m_dtr->setObjectName(QStringLiteral("asbCardDtr"));
    m_dtr->setToolTip(QStringLiteral("Assert DTR while a client is connected. Leave off unless your device needs it."));
    m_rts = new QCheckBox(QStringLiteral("Assert RTS"), this);
    m_rts->setObjectName(QStringLiteral("asbCardRts"));
    m_rts->setToolTip(QStringLiteral("Assert RTS while a client is connected. Leave off unless your device needs it."));
    m_traffic = new QLabel(this);
    m_traffic->setObjectName(QStringLiteral("asbCardTraffic"));
    // applyComboStyle (ComboStyle.h:89) with the field padding and radius of the canon sheet's QLineEdit, so a combo
    // and a field side by side are one height ("a taller field wants more padding", ComboStyle.h).
    for (QComboBox* c : {m_device, m_baud, m_format, m_listen})
        AetherSDR::applyComboStyle(c, QStringLiteral("QComboBox { padding: 9px 8px 9px 12px; border-radius: 5px; }"));

    grid->addWidget(fieldLabel(QStringLiteral("Name"), this), 0, 0);
    grid->addWidget(m_name, 0, 1);
    grid->addWidget(m_enabled, 0, 2);
    grid->addWidget(fieldLabel(QStringLiteral("Device"), this), 0, 3);
    grid->addWidget(m_device, 0, 4);
    grid->addWidget(fieldLabel(QStringLiteral("Baud"), this), 0, 5);
    grid->addWidget(m_baud, 0, 6);
    grid->addWidget(fieldLabel(QStringLiteral("Format"), this), 0, 7);
    grid->addWidget(m_format, 0, 8);
    grid->addWidget(fieldLabel(QStringLiteral("Listen on"), this), 1, 0);
    grid->addWidget(m_listen, 1, 1);
    grid->addWidget(fieldLabel(QStringLiteral("TCP port"), this), 1, 3);
    grid->addWidget(m_tcpPort, 1, 4);
    grid->addWidget(fieldLabel(QStringLiteral("Allowed"), this), 2, 0);
    grid->addWidget(m_allowed, 2, 1, 1, 8);
    // Visible, not a tooltip (owner request 2026-10-19: what Allowed means, with an entry example). It wraps, so the
    // whole sentence stays readable at the window's minimum width.
    m_allowedHint = new QLabel(allowedHintText(windows), this);
    m_allowedHint->setObjectName(QStringLiteral("asbHint"));
    m_allowedHint->setWordWrap(true);
    grid->addWidget(m_allowedHint, 3, 1, 1, 8);
    grid->addWidget(fieldLabel(QStringLiteral("Lines"), this), 4, 0);
    auto* lines = new QHBoxLayout;
    lines->setSpacing(16);
    lines->addWidget(m_dtr);
    lines->addWidget(m_rts);
    lines->addStretch(1);
    grid->addLayout(lines, 4, 1, 1, 2);
    grid->addWidget(fieldLabel(QStringLiteral("Traffic"), this), 4, 3);
    grid->addWidget(m_traffic, 4, 4, 1, 5);
    grid->setColumnStretch(1, 2);
    grid->setColumnStretch(4, 1);

    connect(m_name, &QLineEdit::textEdited, this, [this](const QString& t) { change([&](PortMapping& p) { p.name = t; }); });
    connect(m_enabled, &QCheckBox::toggled, this, [this](bool on) { change([&](PortMapping& p) { p.enabled = on; }); });
    connect(m_device, &QComboBox::currentTextChanged, this,
            [this](const QString& t) { change([&](PortMapping& p) { p.comPort = t.trimmed(); }); });
    connect(m_baud, &QComboBox::currentTextChanged, this, [this](const QString& t) {
        change([&](PortMapping& p) {
            bool ok = false;
            const int v = t.trimmed().toInt(&ok);
            p.baudRate = ok ? v : 0;   // validate() names a baud rate that is not a positive number
        });
    });
    connect(m_format, &QComboBox::currentTextChanged, this,
            [this](const QString& t) { change([&](PortMapping& p) { parseFormat(t, &p); }); });
    connect(m_listen, &QComboBox::currentTextChanged, this,
            [this](const QString& t) { change([&](PortMapping& p) { p.bindAddress = t.trimmed(); }); });
    connect(m_tcpPort, &QLineEdit::textEdited, this, [this](const QString& t) {
        change([&](PortMapping& p) {
            bool ok = false;
            const int v = t.trimmed().toInt(&ok);
            p.tcpPort = ok ? v : 0;   // validate() names a port outside 1-65535
        });
    });
    connect(m_allowed, &QLineEdit::textEdited, this, [this](const QString& t) { change([&](PortMapping& p) { p.allowedClients = t; }); });
    connect(m_dtr, &QCheckBox::toggled, this, [this](bool on) { change([&](PortMapping& p) { p.dtrEnable = on; }); });
    connect(m_rts, &QCheckBox::toggled, this, [this](bool on) { change([&](PortMapping& p) { p.rtsEnable = on; }); });

    setRow(-1);
}

void PortCard::change(const std::function<void(PortMapping&)>& apply)
{
    if (m_filling || m_row < 0 || m_row >= m_editor.config().ports.size()) return;
    PortMapping copy = m_editor.config().ports.at(m_row);
    apply(copy);
    if (copy == m_editor.config().ports.at(m_row)) return;   // no change, no edit
    m_editor.edit().ports[m_row] = copy;
    m_title->setText(copy.displayName().trimmed().isEmpty() ? QStringLiteral("Port %1").arg(m_row + 1) : copy.displayName().trimmed());
    emit edited(m_row);
}

void PortCard::setRow(int row)
{
    m_row = (row >= 0 && row < m_editor.config().ports.size()) ? row : -1;
    fill();
}

void PortCard::setChoices(const QStringList& devices, const QStringList& addresses)
{
    m_devices = devices;
    m_addresses = addresses;
    fill();
}

void PortCard::setEditable(bool on)
{
    // Enabled state only: this runs on every keystroke and every 1 s tick, so it never refills a field.
    if (on == m_editable) return;
    m_editable = on;
    const bool has = m_row >= 0;
    for (QWidget* w : std::initializer_list<QWidget*>{m_name, m_enabled, m_device, m_baud, m_format, m_listen, m_tcpPort, m_allowed,
                                                      m_dtr, m_rts})
        w->setEnabled(has && m_editable);
}

void PortCard::setTraffic(const std::optional<PortStatus>& s)
{
    if (!s) {
        m_traffic->setText(QStringLiteral("—"));
        return;
    }
    const QLocale loc;
    m_traffic->setText(QStringLiteral("to device %1 B · to client %2 B").arg(loc.toString(s->bytesToSerial), loc.toString(s->bytesToClient)));
}

void PortCard::fill()
{
    // A row past the working copy (the list was replaced under the card) is no row. The window clamps the
    // row before anything refills the card, so this is defence only, and it says so when it is reached.
    if (m_row >= m_editor.config().ports.size()) {
        qWarning("PortCard: row %d is past the %d ports of the working copy; no port is shown", m_row,
                 int(m_editor.config().ports.size()));
        m_row = -1;
    }
    m_filling = true;
    const bool has = m_row >= 0;
    m_empty->setVisible(!has);
    for (QWidget* w : std::initializer_list<QWidget*>{m_name, m_enabled, m_device, m_baud, m_format, m_listen, m_tcpPort, m_allowed,
                                                      m_dtr, m_rts})
        w->setEnabled(has && m_editable);
    if (!has) {
        m_title->setText(QStringLiteral("No port selected"));
        for (QLineEdit* e : {m_name, m_tcpPort, m_allowed}) e->clear();
        for (QComboBox* c : {m_device, m_baud, m_format, m_listen}) {
            const QSignalBlocker b(c);
            c->clear();
        }
        for (QCheckBox* c : {m_enabled, m_dtr, m_rts}) c->setChecked(false);
        m_traffic->setText(QStringLiteral("—"));
        m_filling = false;
        return;
    }
    const PortMapping& p = m_editor.config().ports.at(m_row);
    m_title->setText(p.displayName().trimmed().isEmpty() ? QStringLiteral("Port %1").arg(m_row + 1) : p.displayName().trimmed());
    if (m_name->text() != p.name) m_name->setText(p.name);
    m_enabled->setChecked(p.enabled);
    // Every field is compared with the model by value and set only when it differs (what change() stores: trimmed
    // device and address, the baud and port as numbers, so a typed "0" or "038400" is left as typed).
    syncCombo(m_device, m_devices, p.comPort, [&](const QString& t) { return t.trimmed() == p.comPort; });
    syncCombo(m_baud, baudChoices(), p.baudRate > 0 ? QString::number(p.baudRate) : QString(),
              [&](const QString& t) { return numberIn(t) == p.baudRate; });
    syncCombo(m_format, formatChoices(), formatOf(p), [&](const QString& t) { return t == formatOf(p); });
    syncCombo(m_listen, m_addresses, p.bindAddress, [&](const QString& t) { return t.trimmed() == p.bindAddress; });
    if (numberIn(m_tcpPort->text()) != p.tcpPort) m_tcpPort->setText(p.tcpPort > 0 ? QString::number(p.tcpPort) : QString());
    if (m_allowed->text() != p.allowedClients) m_allowed->setText(p.allowedClients);
    m_dtr->setChecked(p.dtrEnable);
    m_rts->setChecked(p.rtsEnable);
    m_filling = false;
}

}  // namespace asb::gui
