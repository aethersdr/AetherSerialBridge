#pragma once

// The port table (WS-10 §5.3): one row per port of the working copy. Columns On | Name | Device | Listen | Status.
// "On" is the only cell edited in the table (a check box, an edit of the working copy); the card edits the rest.
// Status comes from the service's published report, matched by device and TCP port, shown as a dot
// plus a word: the state is never colour alone.

#include "core/BridgeConfig.h"
#include "core/StatusStore.h"

#include <QAbstractTableModel>
#include <QColor>

#include <functional>
#include <optional>

namespace asb::gui {

class EditorModel;

class PortTableModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { On, Name, Device, Listen, Status, ColumnCount };

    explicit PortTableModel(EditorModel& editor, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    // The working copy changed wholesale (load, add, remove).
    void reset();
    // One row's fields changed (the card).
    void rowChanged(int row);
    // The published status; `running` = the service reports Running; `fresh` = the report is recent.
    void setStatus(const std::optional<StatusReport>& report, bool running, bool fresh);
    // The status of a port as the table shows it (also the card's traffic line).
    std::optional<PortStatus> statusOf(const PortMapping&) const;
    QString statusText(int row) const;
    // Edits disabled (read-only window).
    void setEditable(bool on) { m_editable = on; }
    // Colours for the status dots (resolved by the window from the theme).
    std::function<QColor(const QString& tone)> toneColour;

signals:
    // The user ticked or unticked "On" (an edit of the working copy has been made).
    void edited(int row);

private:
    QString tone(int row) const;
    EditorModel& m_editor;
    std::optional<StatusReport> m_report;
    bool m_running = false;
    bool m_fresh = false;
    bool m_editable = true;
};

// "8N1" etc.
QString formatOf(const PortMapping&);
bool parseFormat(const QString& text, PortMapping* into);
// "10.0.0.5:51234"-style endpoint of a port (Listen column).
QString listenText(const PortMapping&);

}  // namespace asb::gui
