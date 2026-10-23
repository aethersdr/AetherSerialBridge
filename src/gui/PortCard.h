#pragma once

// The selected port's card (WS-10 §5.3, style guide "card of label / value rows"): every field of one port of the
// working copy. A change the user makes is one edit of the working copy (EditorModel::edit) and emits edited();
// filling the fields from the model never is.

#include "core/BridgeConfig.h"
#include "core/StatusStore.h"

#include <QFrame>
#include <QStringList>

#include <functional>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;

namespace asb::gui {

class EditorModel;

class PortCard final : public QFrame {
    Q_OBJECT

public:
    // `windows`: the Allowed field's hint says what the list does on this OS (Windows: the firewall rule admits only
    // those addresses; Linux: saved, not enforced, D7) and the Device field's tip suits its names.
    PortCard(EditorModel& editor, bool windows, QWidget* parent = nullptr);

    // Shows port `row` of the working copy (-1: none selected).
    void setRow(int row);
    int row() const { return m_row; }
    // Choices for the device and listen combos (values from the config always join them).
    void setChoices(const QStringList& devices, const QStringList& addresses);
    void setEditable(bool on);
    void setTraffic(const std::optional<PortStatus>& status);

    // The line under Allowed: an example, what blank means, and what this OS does with the list.
    QLabel* allowedHint() const { return m_allowedHint; }
    static QString allowedHintText(bool windows);

    static QStringList baudChoices();
    static QStringList formatChoices();

signals:
    void edited(int row);

private:
    void fill();
    void change(const std::function<void(PortMapping&)>& apply);

    EditorModel& m_editor;
    int m_row = -1;
    bool m_filling = false;
    bool m_editable = true;
    QStringList m_devices, m_addresses;
    QLabel* m_title;
    QLineEdit* m_name;
    QCheckBox* m_enabled;
    QComboBox* m_device;
    QComboBox* m_baud;
    QComboBox* m_format;
    QComboBox* m_listen;
    QLineEdit* m_tcpPort;
    QLineEdit* m_allowed;
    QLabel* m_allowedHint;
    QCheckBox* m_dtr;
    QCheckBox* m_rts;
    QLabel* m_traffic;
    QLabel* m_empty;
};

}  // namespace asb::gui
