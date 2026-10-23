#include "gui/PortTableModel.h"

#include "gui/EditorModel.h"

#include <QBrush>
#include <QLocale>
#include <QPainter>
#include <QPixmap>

namespace asb::gui {

QString formatOf(const PortMapping& p)
{
    QString parity;
    switch (p.parity) {
    case Parity::Even: parity = QStringLiteral("E"); break;
    case Parity::Odd: parity = QStringLiteral("O"); break;
    case Parity::Mark: parity = QStringLiteral("M"); break;
    case Parity::Space: parity = QStringLiteral("S"); break;
    default: parity = QStringLiteral("N"); break;
    }
    QString stop;
    switch (p.stopBits) {
    case StopBits::Two: stop = QStringLiteral("2"); break;
    case StopBits::OnePointFive: stop = QStringLiteral("1.5"); break;
    default: stop = QStringLiteral("1"); break;
    }
    return QString::number(p.dataBits) + parity + stop;
}

bool parseFormat(const QString& text, PortMapping* into)
{
    // A digit, a parity letter, then 1 / 1.5 / 2.
    if (text.size() < 3 || !text[0].isDigit() || text[0].unicode() > 127) return false;
    PortMapping p = *into;
    p.dataBits = text[0].unicode() - '0';
    switch (text[1].toUpper().unicode()) {
    case 'N': p.parity = Parity::None; break;
    case 'E': p.parity = Parity::Even; break;
    case 'O': p.parity = Parity::Odd; break;
    case 'M': p.parity = Parity::Mark; break;
    case 'S': p.parity = Parity::Space; break;
    default: return false;
    }
    const QString stop = text.mid(2);
    if (stop == QLatin1String("1")) p.stopBits = StopBits::One;
    else if (stop == QLatin1String("1.5")) p.stopBits = StopBits::OnePointFive;
    else if (stop == QLatin1String("2")) p.stopBits = StopBits::Two;
    else return false;
    *into = p;
    return true;
}

QString listenText(const PortMapping& p)
{
    const QString addr = p.bindAddress.trimmed().isEmpty() ? QStringLiteral("0.0.0.0") : p.bindAddress.trimmed();
    const QString host = addr.contains(QLatin1Char(':')) ? QStringLiteral("[%1]").arg(addr) : addr;
    return QStringLiteral("%1:%2").arg(host).arg(p.tcpPort);
}

PortTableModel::PortTableModel(EditorModel& editor, QObject* parent) : QAbstractTableModel(parent), m_editor(editor) {}

int PortTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_editor.config().ports.size());
}

int PortTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

std::optional<PortStatus> PortTableModel::statusOf(const PortMapping& p) const
{
    if (!m_report || !m_fresh) return std::nullopt;
    for (const PortStatus& s : m_report->ports)
        if (equalsOrdinalIgnoreCase(s.comPort, p.comPort) && s.tcpPort == p.tcpPort) return s;
    return std::nullopt;
}

QString PortTableModel::statusText(int row) const
{
    const PortMapping& p = m_editor.config().ports.at(row);
    if (!p.enabled) return QStringLiteral("Off");
    const std::optional<PortStatus> s = statusOf(p);
    if (!s) return m_running ? QStringLiteral("Not applied yet") : QStringLiteral("Not running");
    switch (s->state) {
    case BridgeState::Connected:
        return QStringLiteral("Connected · %1").arg(s->client.value_or(QStringLiteral("a client")));
    case BridgeState::Listening:
        return s->lastError ? QStringLiteral("Listening · last: %1").arg(*s->lastError) : QStringLiteral("Listening");
    case BridgeState::Error: return QStringLiteral("Error · %1").arg(s->lastError.value_or(QStringLiteral("unknown")));
    case BridgeState::Stopped: return QStringLiteral("Stopped");
    }
    return {};
}

QString PortTableModel::tone(int row) const
{
    const PortMapping& p = m_editor.config().ports.at(row);
    if (!p.enabled) return QStringLiteral("info");
    const std::optional<PortStatus> s = statusOf(p);
    if (!s) return QStringLiteral("info");
    switch (s->state) {
    case BridgeState::Connected: return QStringLiteral("good");
    case BridgeState::Listening: return s->lastError ? QStringLiteral("warn") : QStringLiteral("good");
    case BridgeState::Error: return QStringLiteral("bad");
    case BridgeState::Stopped: return QStringLiteral("info");
    }
    return QStringLiteral("info");
}

QVariant PortTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount()) return {};
    const PortMapping& p = m_editor.config().ports.at(index.row());
    switch (index.column()) {
    case On:
        if (role == Qt::CheckStateRole) return p.enabled ? Qt::Checked : Qt::Unchecked;
        if (role == Qt::AccessibleTextRole) return p.enabled ? QStringLiteral("On") : QStringLiteral("Off");
        return {};
    case Name:
        if (role == Qt::DisplayRole) return p.name.trimmed().isEmpty() ? p.displayName().trimmed() : p.name.trimmed();
        return {};
    case Device:
        if (role == Qt::DisplayRole) return p.comPort.trimmed();
        return {};
    case Listen:
        if (role == Qt::DisplayRole) return listenText(p);
        return {};
    case Status:
        if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole) return statusText(index.row());
        if (role == Qt::DecorationRole && toneColour) {
            QPixmap dot(10, 10);
            dot.fill(Qt::transparent);
            QPainter painter(&dot);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(toneColour(tone(index.row())));
            painter.drawEllipse(QRectF(1, 1, 8, 8));
            return dot;
        }
        if (role == Qt::ToolTipRole) {
            const std::optional<PortStatus> s = statusOf(p);
            if (!s) return {};
            const QLocale loc;
            return QStringLiteral("to device %1 B · to client %2 B")
                .arg(loc.toString(s->bytesToSerial), loc.toString(s->bytesToClient));
        }
        return {};
    default: return {};
    }
}

bool PortTableModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (!index.isValid() || index.column() != On || role != Qt::CheckStateRole || !m_editable) return false;
    const bool on = value.toInt() == Qt::Checked;
    if (m_editor.config().ports.at(index.row()).enabled == on) return false;
    m_editor.edit().ports[index.row()].enabled = on;
    emit dataChanged(this->index(index.row(), 0), this->index(index.row(), ColumnCount - 1));
    emit edited(index.row());
    return true;
}

Qt::ItemFlags PortTableModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (index.column() == On && m_editable) f |= Qt::ItemIsUserCheckable;
    return f;
}

QVariant PortTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case On: return QStringLiteral("On");
    case Name: return QStringLiteral("Name");
    case Device: return QStringLiteral("Device");
    case Listen: return QStringLiteral("Listen");
    case Status: return QStringLiteral("Status");
    default: return {};
    }
}

void PortTableModel::reset()
{
    beginResetModel();
    endResetModel();
}

void PortTableModel::rowChanged(int row)
{
    if (row < 0 || row >= rowCount()) return;
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
}

void PortTableModel::setStatus(const std::optional<StatusReport>& report, bool running, bool fresh)
{
    m_report = report;
    m_running = running;
    m_fresh = fresh;
    if (rowCount() > 0) emit dataChanged(index(0, Status), index(rowCount() - 1, Status));
}

}  // namespace asb::gui
