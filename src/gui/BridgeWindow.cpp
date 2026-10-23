#include "gui/BridgeWindow.h"

#include "core/AppSettings.h"
#include "core/BuildInfo.h"
#include "core/BuildStamp.h"
#include "core/ConfigJson.h"
#include "core/TrustedConfig.h"
#include "core/UninstallPreview.h"
#include "core/ThemeManager.h"
#include "gui/Actions.h"
#include "gui/BindAddress.h"
#include "gui/BridgeStyle.h"
#include "gui/CanonIndicators.h"
#include "gui/Dialogs.h"
#include "gui/FramelessResizer.h"
#include "gui/PortCard.h"
#include "gui/PortTableModel.h"

#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QItemSelectionModel>
#include <QLabel>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStyle>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QVersionNumber>

#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace asb::gui {
namespace {

constexpr int kLaunchWidth = 1040;
constexpr int kLaunchHeight = 760;
constexpr int kMinWidth = 760;
constexpr int kMinHeightExpanded = 560;
constexpr int kMinHeightCollapsed = 380;
constexpr int kBodySpacing = 10;   // the body layout's spacing (AetherRX)
constexpr qint64 kLogTailBytes = 64 * 1024;
constexpr int kLogRefreshMs = 2000;
constexpr qint64 kStatusFreshSecs = 10;      // a status report older than this is not shown
// The Discard saved changes operation's name: the footer says it while it runs, and finished() recognises the
// discard by it (m_operation).
constexpr char kDiscarding[] = "Discarding";
constexpr char kUninstalling[] = "Uninstalling";

// "Amp", "Amp and Rotor", "3 ports (Amp, Rotor and 1 more)": a question's sentence names the count and the first two;
// the whole list is the dialog's bounded list (WS-17 §2, owner debt).
QString namedCount(const QStringList& names)
{
    if (names.size() == 1) return names.first();
    if (names.size() == 2) return QStringLiteral("%1 and %2").arg(names.at(0), names.at(1));
    return QStringLiteral("%1 ports (%2, %3 and %4 more)").arg(names.size()).arg(names.at(0), names.at(1)).arg(names.size() - 2);
}

void setTone(QWidget* w, const QString& tone)
{
    if (w->property("tone").toString() == tone) return;
    w->setProperty("tone", tone);
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

QLabel* sectionLabel(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setObjectName(QStringLiteral("SectionLabel"));
    return l;
}

QPushButton* makeButton(const QString& text, const QString& name, QWidget* parent)
{
    auto* b = new QPushButton(text, parent);
    b->setObjectName(name);
    b->setAutoDefault(false);
    return b;
}

QStringList defaultLocalAddresses()
{
    // The IPv4 addresses of interfaces that are up and not loopback, then 127.0.0.1 and
    // 0.0.0.0.
    QStringList found;
    for (const QNetworkInterface& n : QNetworkInterface::allInterfaces()) {
        if (!(n.flags() & QNetworkInterface::IsUp) || (n.flags() & QNetworkInterface::IsLoopBack)) continue;
        for (const QNetworkAddressEntry& e : n.addressEntries())
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol) found << e.ip().toString();
    }
    found.sort();
    found.removeDuplicates();
    return found << QStringLiteral("127.0.0.1") << QStringLiteral("0.0.0.0");
}

}  // namespace

// The questions, each holding the deferred start while it is open.
class BridgeWindow::HoldingAsker final : public IAsker {
public:
    explicit HoldingAsker(BridgeWindow& w) : m_w(w) {}
    bool confirm(const QString& id, const QString& text, const QString& yes, const QString& no) override
    {
        const Hold hold(m_w);
        return m_w.m_rawAsker->confirm(id, text, yes, no);
    }
    void notify(const QString& id, const QString& text) override
    {
        const Hold hold(m_w);
        m_w.m_rawAsker->notify(id, text);
    }
    Answer ask(const Question& q) override
    {
        const Hold hold(m_w);
        return m_w.m_rawAsker->ask(q);
    }

private:
    struct Hold {
        explicit Hold(BridgeWindow& w) : window(&w) { ++w.m_deferHolds; }
        ~Hold()
        {
            if (window) window->releaseDeferred();
        }
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;
        QPointer<BridgeWindow> window;
    };
    BridgeWindow& m_w;
};

BridgeWindow::BridgeWindow(IViewSource& view, IActions& actions, IAsker* asker, Options options, QWidget* parent)
    : CanonWindow(QStringLiteral("Aether Serial Bridge"), parent, Kind::Workspace)
    , m_view(view)
    , m_actions(actions)
    , m_rawAsker(asker)
    , m_options(std::move(options))
{
    // The AetherRX sequence (AetherRxDialog.cpp:91-121): container, geometry key, sizes, resizer, body sheet,
    // body layout, header.
    setObjectName(QStringLiteral("asbWindow"));
    AetherSDR::theme::setContainer(this, QStringLiteral("canon/serialBridge"));
    setGeometryKey(QStringLiteral("AetherSerialBridgeGeometry1"));
    setMinimumSize(kMinWidth, kMinHeightExpanded);
    setLaunchSize(QSize(kLaunchWidth, kLaunchHeight));
    AetherSDR::FramelessResizer::install(this);
    // The bridge is the application's main window: closing it ends the program (a Workspace CanonWindow clears this).
    setAttribute(Qt::WA_QuitOnClose, true);
    // One composed body sheet (INV-69); applyCanonSheet rebuilds it on a theme change for the painted indicators.
    AetherSDR::applyCanonSheet(bodyWidget(), [] { return composedBodySheet(); });
    if (!m_rawAsker) {
        m_ownAsker = std::make_unique<CanonAsker>(this);
        m_rawAsker = m_ownAsker.get();
    }
    m_holding = std::make_unique<HoldingAsker>(*this);
    m_asker = m_holding.get();
    build();
    // The Linux window learns whether its own program can be staged on a worker: the buttons follow when it ends.
    QPointer<BridgeWindow> self(this);
    m_actions.setLifecycleListener([self] {
        if (!self) return;
        self->updateServiceRow(self->m_service);
        self->updateButtons();
    });
    // The details card's first state: the test's, else gui.ini's (collapsed on first run), and collapsed in a read-only
    // window whatever was saved. A collapsed first state opens a window shorter by the card.
    const bool initiallyExpanded =
        m_options.detailsExpanded.value_or(m_actions.available() &&
                                           AetherSDR::AppSettings::instance().value(QString::fromLatin1(kDetailsKey), false).toBool());
    if (!initiallyExpanded) {
        const int cardHeight = m_cardHost->sizeHint().height() + kBodySpacing;
        m_expandedHeight = kLaunchHeight;   // never open yet: opening restores the launch height
        applyDetails(false);
        setLaunchSize(QSize(kLaunchWidth, std::max(kMinHeightCollapsed, kLaunchHeight - cardHeight)));
    }

    if (m_options.refreshMs > 0) {
        m_timer = new QTimer(this);
        m_timer->setInterval(m_options.refreshMs);
        connect(m_timer, &QTimer::timeout, this, &BridgeWindow::refreshStatus);
        m_timer->start();
    }
    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(500);
    connect(m_progressTimer, &QTimer::timeout, this, &BridgeWindow::updateProgress);
    m_logTimer = new QTimer(this);
    m_logTimer->setInterval(kLogRefreshMs);
    connect(m_logTimer, &QTimer::timeout, this, &BridgeWindow::refreshLog);

    reload(true);
}

BridgeWindow::~BridgeWindow()
{
    m_actions.setLifecycleListener({});
}

void BridgeWindow::build()
{
    QWidget* body = bodyWidget();
    auto* outer = new QVBoxLayout(body);
    outer->setContentsMargins(14, 12, 14, 14);
    outer->setSpacing(10);

    // Header: the logo with its spark, the canon header, the subtitle (MainWindow_Menus.cpp:1822,
    // CanonWindow.cpp:333-360).
    auto* header = new QWidget(body);
    header->setObjectName(QString::fromLatin1(kHeaderName));
    auto* headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(12);
    auto* spark = new AetherSDR::SparkRing(QPixmap(QStringLiteral(":/logo-256.png")), 44, header);
    spark->setObjectName(QStringLiteral("asbLogo"));
    headerRow->addWidget(spark, 0, Qt::AlignTop);
    auto* headerCol = new QVBoxLayout;
    headerCol->setSpacing(6);
    headerCol->addWidget(AetherSDR::makeCanonHeader(QStringLiteral("Aether Serial Bridge")));
    auto* subtitle = new QLabel(QStringLiteral("Serial gear on the network for AetherSDR: one port, one client."), header);
    subtitle->setObjectName(QStringLiteral("asbSubtitle"));
    headerCol->addWidget(subtitle);
    headerRow->addLayout(headerCol, 1);
    outer->addWidget(header);

    // Banner (read-only, still processing) and the pending note.
    auto* bannerRow = new QHBoxLayout;
    m_banner = new QLabel(body);
    m_banner->setObjectName(QStringLiteral("asbBanner"));
    m_banner->setWordWrap(true);
    m_banner->hide();
    bannerRow->addWidget(m_banner, 1);
    m_restartAdmin = makeButton(QStringLiteral("Restart as administrator"), QStringLiteral("asbRestartAdmin"), body);
    m_restartAdmin->hide();
    bannerRow->addWidget(m_restartAdmin);
    outer->addLayout(bannerRow);
    connect(m_restartAdmin, &QPushButton::clicked, this, &BridgeWindow::restartAsAdmin);
    auto* pendingRow = new QHBoxLayout;
    m_pendingNote = new QLabel(body);
    m_pendingNote->setObjectName(QStringLiteral("asbPendingNote"));
    m_pendingNote->setWordWrap(true);
    m_discardPending = makeButton(QStringLiteral("Discard saved changes"), QStringLiteral("asbDiscardPending"), body);
    pendingRow->addWidget(m_pendingNote, 1);
    pendingRow->addWidget(m_discardPending);
    outer->addLayout(pendingRow);
    connect(m_discardPending, &QPushButton::clicked, this, &BridgeWindow::discardPending);

    // Service row.
    auto* serviceRow = new QWidget(body);
    serviceRow->setObjectName(QString::fromLatin1(kServiceRowName));
    auto* srow = new QHBoxLayout(serviceRow);
    srow->setContentsMargins(0, 0, 0, 0);
    srow->setSpacing(10);
    srow->addWidget(sectionLabel(QStringLiteral("SERVICE"), serviceRow));
    m_serviceDot = new QLabel(serviceRow);
    m_serviceDot->setObjectName(QStringLiteral("StatusDot"));
    srow->addWidget(m_serviceDot);
    m_serviceText = new QLabel(serviceRow);
    m_serviceText->setObjectName(QStringLiteral("asbServiceText"));
    m_serviceText->setWordWrap(true);
    srow->addWidget(m_serviceText, 1);
    m_lifecycle = makeButton(QStringLiteral("Install the bridge service"), QStringLiteral("asbLifecycle"), serviceRow);
    m_lifecycle->hide();
    srow->addWidget(m_lifecycle);
    connect(m_lifecycle, &QPushButton::clicked, this, [this] {
        if (m_service.installation == InstallState::IncompleteInstall && m_service.otherBuild) updateService();
        else installService();
    });
    m_start = makeButton(QStringLiteral("Start"), QStringLiteral("asbStart"), serviceRow);
    m_stop = makeButton(QStringLiteral("Stop"), QStringLiteral("asbStop"), serviceRow);
    m_showLog = makeButton(QStringLiteral("Show log"), QStringLiteral("asbShowLog"), serviceRow);
    srow->addWidget(m_start);
    srow->addWidget(m_stop);
    srow->addWidget(m_showLog);
    outer->addWidget(serviceRow);
    connect(m_start, &QPushButton::clicked, this, &BridgeWindow::startService);
    connect(m_stop, &QPushButton::clicked, this, &BridgeWindow::stopService);
    connect(m_showLog, &QPushButton::clicked, this, &BridgeWindow::toggleLog);

    // Ports: the table and its buttons.
    auto* ports = new QWidget(body);
    ports->setObjectName(QString::fromLatin1(kPortsPanelName));
    auto* pcol = new QVBoxLayout(ports);
    pcol->setContentsMargins(0, 0, 0, 0);
    pcol->setSpacing(8);
    pcol->addWidget(sectionLabel(QStringLiteral("PORTS"), ports));
    m_model = new PortTableModel(m_editor, this);
    m_model->toneColour = [this](const QString& tone) {
        auto& tm = AetherSDR::ThemeManager::instance();
        if (tone == QLatin1String("good")) return tm.color(this, QStringLiteral("color.accent.success"));
        if (tone == QLatin1String("warn")) return tm.color(this, QStringLiteral("color.accent.warning"));
        if (tone == QLatin1String("bad")) return tm.color(this, QStringLiteral("color.accent.danger"));
        return tm.color(this, QStringLiteral("color.canon.muted"));
    };
    m_table = new QTableView(ports);
    m_table->setObjectName(QStringLiteral("asbPortTable"));
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(PortTableModel::On, QHeaderView::ResizeToContents);
    m_table->setColumnWidth(PortTableModel::Name, 150);
    m_table->setColumnWidth(PortTableModel::Device, 140);
    m_table->setColumnWidth(PortTableModel::Listen, 200);
    m_table->setMinimumHeight(140);
    pcol->addWidget(m_table, 1);
    auto* prow = new QHBoxLayout;
    m_add = makeButton(QStringLiteral("+ Add port"), QStringLiteral("asbAddPort"), ports);
    m_remove = makeButton(QStringLiteral("Remove"), QStringLiteral("asbRemovePort"), ports);
    prow->addWidget(m_add);
    prow->addWidget(m_remove);
    prow->addStretch(1);
    // The details disclosure at the bottom edge of the table area, with the unsaved-changes marker it shows folded.
    m_detailsMarker = new QLabel(QStringLiteral("● Unsaved changes in the details"), ports);
    m_detailsMarker->setObjectName(QStringLiteral("asbDetailsMarker"));
    setTone(m_detailsMarker, QStringLiteral("warn"));
    m_detailsMarker->hide();
    prow->addWidget(m_detailsMarker);
    m_details = makeButton(QStringLiteral("Details ▴"), QStringLiteral("asbDetailsToggle"), ports);
    m_details->setAccessibleName(QStringLiteral("Port details"));
    m_details->setFocusPolicy(Qt::StrongFocus);
    prow->addWidget(m_details);
    connect(m_details, &QPushButton::clicked, this, [this] { setDetailsExpanded(!m_detailsExpanded); });
    pcol->addLayout(prow);
    outer->addWidget(ports, 1);
    connect(m_add, &QPushButton::clicked, this, &BridgeWindow::addPort);
    connect(m_remove, &QPushButton::clicked, this, &BridgeWindow::removeSelectedPort);
    connect(m_model, &PortTableModel::edited, this, [this](int row) {
        if (m_card->row() == row) m_card->setRow(row);
        onEdited();
    });
    // Starting to edit a row opens the details: double-click, or Enter on the row (activated).
    connect(m_table, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex&) { expandForEditing(); });
    connect(m_table, &QAbstractItemView::activated, this, [this](const QModelIndex&) { expandForEditing(); });
    connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex& cur) {
        m_card->setRow(cur.isValid() ? cur.row() : -1);
        if (cur.isValid() && cur.row() < m_editor.config().ports.size())
            m_card->setTraffic(m_model->statusOf(m_editor.config().ports.at(cur.row())));
        updateButtons();
    });

    // The card.
    auto* cardHost = new QFrame(body);
    m_cardHost = cardHost;
    cardHost->setObjectName(QString::fromLatin1(kCardHostName));
    auto* ccol = new QVBoxLayout(cardHost);
    ccol->setContentsMargins(0, 0, 0, 0);
    m_card = new PortCard(m_editor, /*windows=*/!m_options.linuxWords, cardHost);
    ccol->addWidget(m_card);
    outer->addWidget(cardHost);
    connect(m_card, &PortCard::edited, this, [this](int row) {
        m_model->rowChanged(row);
        onEdited();
    });

    // The log pane (Show log), hidden until asked for.
    m_log = new QPlainTextEdit(body);
    m_log->setObjectName(QStringLiteral("asbLog"));
    m_log->setReadOnly(true);
    m_log->setMinimumHeight(120);
    m_log->hide();
    outer->addWidget(m_log);

    // Auto-start, Reload, Save & apply.
    auto* bottom = new QHBoxLayout;
    m_autoStart = new QCheckBox(m_options.linuxWords ? QStringLiteral("Start automatically at boot")
                                                     : QStringLiteral("Start automatically with Windows"),
                                body);
    m_autoStart->setObjectName(QStringLiteral("asbAutoStart"));
    bottom->addWidget(m_autoStart);
    bottom->addStretch(1);
    m_reload = makeButton(QStringLiteral("Reload"), QStringLiteral("asbReload"), body);
    m_save = makeButton(QStringLiteral("Save && apply"), QStringLiteral("asbSaveApply"), body);
    bottom->addWidget(m_reload);
    bottom->addWidget(m_save);
    outer->addLayout(bottom);
    connect(m_autoStart, &QCheckBox::toggled, this, [this](bool on) {
        if (on == m_editor.autoStart()) return;   // set from the service, not by the user
        m_editor.setAutoStart(on);
        onEdited();
    });
    connect(m_reload, &QPushButton::clicked, this, [this] { reload(false); });
    connect(m_save, &QPushButton::clicked, this, &BridgeWindow::saveAndApply);

    // Footer: what happened, and Repair when the folder is untrusted.
    auto* footerRow = new QHBoxLayout;
    m_footer = new QLabel(body);
    m_footer->setObjectName(QStringLiteral("asbFooter"));
    m_footer->setWordWrap(true);
    m_footer->setTextInteractionFlags(Qt::TextSelectableByMouse);
    footerRow->addWidget(m_footer, 1);
    // A Linux action polkit could not ask for (no agent, refused) names the terminal command that does the same thing
    // (WS-15, INV-124); the window offers to copy it (WS-17).
    m_copyCommand = makeButton(QStringLiteral("Copy the command"), QStringLiteral("asbCopyCommand"), body);
    m_copyCommand->hide();
    footerRow->addWidget(m_copyCommand);
    connect(m_copyCommand, &QPushButton::clicked, this, [this] {
        if (QClipboard* c = QGuiApplication::clipboard()) c->setText(m_terminalCommand);
    });
    m_waitCancel = makeButton(QStringLiteral("Cancel"), QStringLiteral("asbWaitCancel"), body);
    m_waitCancel->hide();
    footerRow->addWidget(m_waitCancel);
    connect(m_waitCancel, &QPushButton::clicked, this, &BridgeWindow::cancelWaiting);
    m_repair = makeButton(QStringLiteral("Repair…"), QStringLiteral("asbRepair"), body);
    m_repair->hide();
    footerRow->addWidget(m_repair);
    m_uninstall = makeButton(QStringLiteral("Uninstall…"), QStringLiteral("asbUninstall"), body);
    m_uninstall->hide();
    footerRow->addWidget(m_uninstall);
    outer->addLayout(footerRow);
    connect(m_repair, &QPushButton::clicked, this, &BridgeWindow::repair);
    connect(m_uninstall, &QPushButton::clicked, this, &BridgeWindow::uninstallService);
}

// ---------------------------------------------------------------------------------------------------
// Load and refresh
// ---------------------------------------------------------------------------------------------------

void BridgeWindow::reload(bool force)
{
    if (!force && m_editor.dirty() &&
        !m_asker->confirm(QStringLiteral("discardEdits"), QStringLiteral("Discard unsaved changes?"),
                          QStringLiteral("Discard"), QStringLiteral("Keep editing")))
        return;
    applyLoad(m_view.load());
    autoCollapse();
}

void BridgeWindow::applyLoad(const Snapshot& s)
{
    m_snapshot = s;
    m_service = s.service;
    newFirewallEpisode();   // new evidence: an outstanding check judged the old one
    m_editor.load(s);
    // The real start type, never an edit (INV-23). A start type that could not be read, or a service whose
    // installation could not be read, is not "off": the box is shown undetermined and disabled, and Save & apply never
    // takes the untick route from it.
    m_autoStartKnown = s.service.autoStart.has_value();
    // v1.1 fix after WS-17: with nothing installed there is no start type to show; the box shows the one an install
    // sets (Install the bridge service always installs a service that starts automatically), so the main button reads
    // Install and apply and installs. Unticked, it reads Save (mainActionFor).
    const bool nothingInstalled = s.service.queried && s.service.installation == InstallState::NotInstalled;
    m_editor.setAutoStartFromService(nothingInstalled || s.service.autoStart.value_or(true));
    {
        const QSignalBlocker b(m_autoStart);
        m_autoStart->setTristate(!m_autoStartKnown);
        if (m_autoStartKnown) m_autoStart->setChecked(m_editor.autoStart());
        else m_autoStart->setCheckState(Qt::PartiallyChecked);
        m_autoStart->setToolTip(m_autoStartKnown ? QString()
                                                 : QStringLiteral("The service's start type could not be read%1.")
                                                       .arg(!s.service.error.isEmpty() ? QStringLiteral(": ") + s.service.error
                                                            : !s.service.stateError.isEmpty() ? QStringLiteral(": ") + s.service.stateError
                                                                                               : QString()));
    }
    fillTableAndCard();
    updateServiceRow(s.service);
    const bool running = s.service.state && s.service.state->state == ServiceQuery::Running;
    const bool fresh = s.status && qAbs(s.status->updated.secsTo(QDateTime::currentDateTimeUtc())) < kStatusFreshSecs;
    m_model->setStatus(s.status, running, running && fresh);
    updatePendingNote();

    m_repair->setVisible(s.folder == FolderView::Untrusted);
    if (s.folder == FolderView::Untrusted) {
        setFooter(QStringLiteral("Config not loaded: the data folder isn't trusted.") +
                      (s.folderReason.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(s.folderReason)),
                  QStringLiteral("bad"));
    } else if (m_editor.loaded() && !s.service.queried) {
        setFooter(QStringLiteral("The service's state could not be read (%1). Save & apply reads it again first and changes "
                                 "nothing while it is unknown.").arg(s.service.error),
                  QStringLiteral("warn"));
    } else if (!m_editor.loaded()) {
        setFooter(m_editor.loadError(), QStringLiteral("bad"));
    } else if (s.service.installation == InstallState::Foreign) {
        setFooter(s.service.installationDetail.isEmpty() ? QString() : s.service.installationDetail + QLatin1Char('.'),
                  QStringLiteral("bad"));
    } else if (s.service.installation == InstallState::Current && s.service.autoStart == false) {
        setFooter(QStringLiteral("The bridge is disabled. Save & apply to start it."), QStringLiteral("warn"));
    } else if (!m_editor.loadError().isEmpty()) {
        setFooter(m_editor.loadError(), QStringLiteral("warn"));
    } else if (m_editor.config().ports.isEmpty()) {
        setFooter(QStringLiteral("No ports yet. Click + Add port."), QStringLiteral("info"));
    } else {
        setFooter(QStringLiteral("Loaded %1.").arg(m_editor.fromPending() ? QStringLiteral("the saved, not applied settings")
                                                                          : QStringLiteral("the applied settings")),
                  QStringLiteral("info"));
    }
    setBusy(busy());
    updateDetailsMarker();
    maybeCheckFirewall();
    // The elevated window started by Restart as administrator opens the intended action's confirmation once, after its
    // first load; the user still confirms (WS-17 §3). A standard user's window ignores it.
    if (!m_intentDone && !m_options.intent.isEmpty() && m_options.intent != QLatin1String("none")) {
        m_intentDone = true;
        if (m_actions.available()) {
            const QString intent = m_options.intent;
            QTimer::singleShot(0, this, [this, intent] {
                if (intent == QLatin1String("uninstall")) uninstallService();
                else if (intent == QLatin1String("update") && m_service.installation == InstallState::IncompleteInstall &&
                         m_service.otherBuild)
                    updateService();
                else if (intent == QLatin1String("install") || intent == QLatin1String("update")) installService();
            });
        }
    }
}

void BridgeWindow::refreshEvidence(const Snapshot& s)
{
    m_snapshot = s;
    m_service = s.service;
    newFirewallEpisode();   // new evidence: an outstanding check judged the old one
    updateServiceRow(s.service);
    const bool running = s.service.state && s.service.state->state == ServiceQuery::Running;
    const bool fresh = s.status && qAbs(s.status->updated.secsTo(QDateTime::currentDateTimeUtc())) < kStatusFreshSecs;
    m_model->setStatus(s.status, running, running && fresh);
    if (m_card->row() >= 0 && m_card->row() < m_editor.config().ports.size())
        m_card->setTraffic(m_model->statusOf(m_editor.config().ports.at(m_card->row())));
    m_repair->setVisible(s.folder == FolderView::Untrusted);
    updatePendingNote();
    setBusy(false);
    maybeCheckFirewall();
}

void BridgeWindow::fillTableAndCard()
{
    // The working copy was replaced (load, Reload, Discard, an operation's result): the card's row is clamped to the
    // replacement list before anything refills the card (a selected row past a shorter list). The model reset
    // clears the table's current index without a signal, so the card's row is the only index that survives it.
    const int keep = m_card->row();
    m_model->reset();
    const int rows = m_model->rowCount();
    const int row = rows == 0 ? -1 : std::clamp(keep < 0 ? 0 : keep, 0, rows - 1);
    m_card->setRow(row);
    m_card->setChoices(deviceChoices(), m_options.localAddresses ? m_options.localAddresses() : defaultLocalAddresses());
    if (row >= 0) m_table->selectionModel()->setCurrentIndex(m_model->index(row, 0),
                                                            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
}

void BridgeWindow::refreshStatus()
{
    // The state only (no installation evidence, no start type: those are read at load and after each operation).
    m_service = m_view.serviceState(m_service, !busy());
    const std::optional<StatusReport> status = m_view.status();
    m_snapshot.status = status;
    updateServiceRow(m_service);
    const bool running = m_service.state && m_service.state->state == ServiceQuery::Running;
    const bool fresh = status && qAbs(status->updated.secsTo(QDateTime::currentDateTimeUtc())) < kStatusFreshSecs;
    m_model->setStatus(status, running, running && fresh);
    if (m_card->row() >= 0 && m_card->row() < m_editor.config().ports.size())
        m_card->setTraffic(m_model->statusOf(m_editor.config().ports.at(m_card->row())));
    // A fresh report naming a config error is said in the footer while nothing is unsaved.
    if (running && fresh && status->configError && !m_editor.dirty() && !busy())
        setFooter(QStringLiteral("The bridge rejected the config: %1").arg(*status->configError), QStringLiteral("bad"));
    updateButtons();
    maybeCheckFirewall();
}

void BridgeWindow::maybeCheckFirewall()
{
    // "stale firewall rule detected" (D53, S20): a bridge that found rules in its reserved group that do not match
    // the committed config ends itself with exit 7 and writes no reason; the window re-runs the same comparator while
    // the installed (this build, or another build kept by the user), activated bridge is stopped. A Failed check says
    // nothing. No check is started while the service is starting (it runs its own check then). A check that is still
    // running when the user starts an operation is waited for first: submit() defers the operation through
    // BridgeWindowOptions::firewallIdle (FirewallChecks::whenIdle) and startDeferred() runs it when the check ends, so
    // the window never starts, stops or reconfigures the service beside its own WMI query (INV-129).
    // A check started while the service is stopped can still be running when SCM's own recovery starts it; its
    // queries are bounded by the check's 3 s deadline (FirewallChecks, WbemSession::setDeadline).
    const ServiceQuery::State st = m_service.state ? m_service.state->state : ServiceQuery::Absent;
    const bool episode = m_options.firewallCheck && !busy() && m_snapshot.folder == FolderView::Trusted &&
                         m_service.queried &&
                         (m_service.installation == InstallState::Current ||
                          (m_service.installation == InstallState::IncompleteInstall && m_service.otherBuild)) &&
                         m_snapshot.tokenGeneration && m_snapshot.committed.config &&
                         (st == ServiceQuery::Stopped || st == ServiceQuery::Other ||
                          (m_options.checkWhileStarting && st == ServiceQuery::StartPending));
    if (!episode) {
        // The episode ends (running, busy, no longer installed or activated): a check still outstanding judged rules a
        // Start or a Save & apply may since have rewritten, so its verdict is dropped when it arrives.
        const bool noteShown = !m_firewallNote.isEmpty();
        if (m_firewallChecking || m_firewallChecked || noteShown) newFirewallEpisode();
        if (noteShown) updateServiceRow(m_service);
        return;
    }
    if (m_firewallChecked || m_firewallChecking) return;
    m_firewallChecking = true;
    QPointer<BridgeWindow> self(this);
    const quint64 episodeId = m_firewallEpisode;
    const bool started = m_options.firewallCheck(
        *m_snapshot.committed.config, *m_snapshot.tokenGeneration,
        [self, episodeId](IFirewallMonitor::Check check, const QString&) {
            // Only the current episode's check is a verdict: one that outlived its evidence (Start, Save &
            // apply, Stop, a reload in between) says nothing, and the current episode's own check is not blocked by it.
            if (!self || self->m_firewallEpisode != episodeId) return;
            self->m_firewallChecking = false;
            self->m_firewallChecked = true;
            self->m_firewallNote = check == IFirewallMonitor::Check::Mismatch
                                       ? QStringLiteral("stale firewall rule detected: the rules in the \"Aether Serial Bridge\" "
                                                        "group do not match the saved settings, so the bridge stops itself. Start "
                                                        "or Save & apply rewrites them.")
                                       : QString();
            self->updateServiceRow(self->m_service);
        });
    if (!started) m_firewallChecking = false;
}

void BridgeWindow::newFirewallEpisode()
{
    // The note is the verdict of the episode it was checked in: it goes with it (the caller repaints the row).
    ++m_firewallEpisode;
    m_firewallChecking = false;
    m_firewallChecked = false;
    m_firewallNote.clear();
}

void BridgeWindow::updateServiceRow(const ServiceView& v)
{
    QString text;
    QString tone = QStringLiteral("info");
    const QString onBoot = m_options.linuxWords ? QStringLiteral("starts at boot") : QStringLiteral("starts with Windows");
    if (!v.queried) {
        text = QStringLiteral("Unknown (%1)").arg(v.error);
        tone = QStringLiteral("warn");
    } else {
        switch (v.installation) {
        case InstallState::NotInstalled:
            text = QStringLiteral("The bridge service is not installed.");
            // An uninstall removed the service and kept the program; Uninstall… finishes it.
            if (v.programRemains) {
                text += QStringLiteral(" An uninstall did not finish: the program is still there. Uninstall… removes it.");
                tone = QStringLiteral("warn");
            }
            break;
        case InstallState::Foreign:
            text = v.installationDetail;
            tone = QStringLiteral("bad");
            break;
        case InstallState::IncompleteInstall:
        case InstallState::Current: {
            const ServiceQuery::State st = v.state ? v.state->state : ServiceQuery::Other;
            if (!v.state) {
                text = QStringLiteral("State unknown (%1)").arg(v.stateError);
                tone = QStringLiteral("warn");
            } else switch (st) {
            case ServiceQuery::Running: text = QStringLiteral("Running"); tone = QStringLiteral("good"); break;
            case ServiceQuery::Stopped: text = QStringLiteral("Stopped"); tone = QStringLiteral("warn"); break;
            case ServiceQuery::StartPending: text = QStringLiteral("Starting"); tone = QStringLiteral("warn"); break;
            case ServiceQuery::StopPending: text = QStringLiteral("Stopping"); tone = QStringLiteral("warn"); break;
            case ServiceQuery::Absent: text = QStringLiteral("Not installed"); break;
            case ServiceQuery::Other: text = QStringLiteral("In an unexpected state"); tone = QStringLiteral("warn"); break;
            }
            if (v.autoStart) text += *v.autoStart ? QStringLiteral(" · %1").arg(onBoot) : QStringLiteral(" · disabled");
            if (v.installation == InstallState::IncompleteInstall) {
                // Another build (WS-14 wording): what is installed and what this window would install.
                text += QStringLiteral(" · ") +
                        (v.otherBuild ? installedBuildPhrase(v)
                                      : QStringLiteral("The installation is not finished (%1).").arg(v.installationDetail));
                tone = QStringLiteral("warn");
            }
            // The ACK fields: does the running bridge report the committed config? (§5.5 RefreshStatus)
            const std::optional<StatusReport>& rep = m_snapshot.status;
            if (st == ServiceQuery::Running && rep && m_snapshot.committed.present && !m_snapshot.committed.bytes.isEmpty() &&
                rep->configSha256 != sha256Hex(m_snapshot.committed.bytes))
                text += QStringLiteral(" · running settings other than the saved config.json (Start loads them)");
            break;
        }
        }
    }
    if (!m_firewallNote.isEmpty()) {
        text += QStringLiteral(" · ") + m_firewallNote;
        tone = QStringLiteral("bad");
    }
    // The staged route cannot use this window's program (Linux, WS-14's reason): said before any click.
    const QString staged = m_actions.available() ? m_actions.lifecycleUnavailableReason() : QString();
    if (!staged.isEmpty() && v.queried && v.installation != InstallState::Current &&
        v.installation != InstallState::Foreign)
        text += QStringLiteral(" · ") + staged;
    m_serviceText->setText(text);
    setTone(m_serviceText, tone);
    setTone(m_serviceDot, tone);
    m_serviceDot->setAccessibleName(text);
}

void BridgeWindow::updatePendingNote()
{
    const bool pending = m_editor.fromPending();
    m_pendingNote->setVisible(pending);
    m_discardPending->setVisible(pending);
    if (pending) {
        m_pendingNote->setText(QStringLiteral("▲ Saved, not applied (config.pending.json)."));
        setTone(m_pendingNote, QStringLiteral("warn"));
    } else {
        m_pendingNote->clear();
    }
}

bool BridgeWindow::installed() const
{
    return m_service.queried &&
           (m_service.installation == InstallState::Current || m_service.installation == InstallState::IncompleteInstall);
}

QStringList BridgeWindow::deviceChoices()
{
    // Names only, never an open (INV-32); every device in the config joins the list even when unplugged.
    QStringList names = m_view.serialPorts();
    for (const PortMapping& p : m_editor.config().ports)
        if (!p.comPort.trimmed().isEmpty()) names << p.comPort.trimmed();
    QStringList out;
    for (const QString& n : names) {
        bool seen = false;
        for (const QString& o : out)
            if (equalsOrdinalIgnoreCase(o, n)) seen = true;
        if (!seen) out << n;
    }
    std::stable_sort(out.begin(), out.end(), [](const QString& a, const QString& b) {
        return a.size() != b.size() ? a.size() < b.size() : a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return out;
}

// ---------------------------------------------------------------------------------------------------
// Buttons and state
// ---------------------------------------------------------------------------------------------------

bool BridgeWindow::busy() const
{
    // Waiting for the window's own firewall check counts: nothing else may be submitted meanwhile (INV-129).
    return m_waiting || m_actions.busy();
}

bool BridgeWindow::readOnly() const
{
    return !m_actions.available();
}

void BridgeWindow::updateButtons()
{
    const bool can = m_actions.available();
    const bool b = busy();
    const bool trusted = m_snapshot.folder != FolderView::Untrusted;
    const ServiceQuery::State st = m_service.state ? m_service.state->state : ServiceQuery::Absent;
    // Start runs this build, or an installed other build the user kept, as it is (Applier::start(acceptOtherBuild),
    // vetoable call 22).
    const bool startable = m_service.queried &&
                           (m_service.installation == InstallState::Current ||
                            (m_service.installation == InstallState::IncompleteInstall && m_service.otherBuild));
    m_start->setEnabled(can && !b && startable && trusted && st == ServiceQuery::Stopped);
    m_stop->setEnabled(can && !b && installed() && (st == ServiceQuery::Running || st == ServiceQuery::StartPending));
    m_reload->setEnabled(!b);
    m_repair->setEnabled(can && !b);
    m_discardPending->setEnabled(can && !b);
    // Editing stays allowed while an operation runs (D57); a read-only window edits nothing.
    const bool editable = can && m_editor.loaded();
    m_add->setEnabled(editable);
    m_remove->setEnabled(editable && m_card->row() >= 0);
    // Nothing installed and no route that saves without installing (the Linux window): the box stays ticked, since
    // unticking it would leave the main button nothing to do. A box found unticked (the service was removed while
    // the window showed it) stays enabled, so that it can be ticked.
    const bool nothingInstalledHere = m_service.queried && m_service.installation == InstallState::NotInstalled;
    const bool installOnly = nothingInstalledHere && !m_actions.savesWithoutService();
    const bool boxHeld = installOnly && m_editor.autoStart();
    m_autoStart->setEnabled(editable && m_autoStartKnown && !boxHeld);
    const QString heldTip = QStringLiteral("Nothing is installed, so the settings are saved by installing the service, "
                                           "which starts at boot. Untick this after installing to keep it from starting.");
    if (boxHeld) m_autoStart->setToolTip(heldTip);
    else if (m_autoStart->toolTip() == heldTip) m_autoStart->setToolTip(QString());
    m_model->setEditable(editable);
    m_card->setEditable(editable);

    // The lifecycle (WS-17 §1): the service row's action for the installation's state, Uninstall… in the footer, and
    // Restart as administrator for a standard user's window. The staged route's reason (Linux) disables what needs the
    // window's own program, before any click.
    const InstallState inst = m_service.installation;
    const bool known = m_service.queried;
    const QString stagedWhy = can ? m_actions.lifecycleUnavailableReason() : QString();
    const bool notInstalled = known && inst == InstallState::NotInstalled;
    const bool other = known && inst == InstallState::IncompleteInstall && m_service.otherBuild;
    const bool unfinished = known && inst == InstallState::IncompleteInstall && !m_service.otherBuild;
    m_lifecycle->setVisible(notInstalled || other || unfinished);
    m_lifecycle->setText(other ? QStringLiteral("Update to this build")
                               : unfinished ? QStringLiteral("Finish installing") : QStringLiteral("Install the bridge service"));
    m_lifecycle->setEnabled(can && !b && m_editor.loaded() && trusted && stagedWhy.isEmpty());
    m_lifecycle->setToolTip(!can ? m_actions.unavailableReason() : stagedWhy);
    // The main button: its label and its route come from one predicate (v1.1 fix after WS-17).
    const MainAction main = mainActionFor(m_service);
    m_save->setText(mainLabel(main, true));
    m_save->setEnabled(can && !b && m_editor.loaded() && trusted && main != MainAction::None &&
                       (main != MainAction::Install || stagedWhy.isEmpty()));
    m_save->setToolTip(main == MainAction::None
                           ? QStringLiteral("Nothing is installed: tick %1 to install the service with these ports.")
                                 .arg(m_autoStart->text())
                       : main == MainAction::Install ? stagedWhy
                                                      : QString());
    const bool refusal = known && inst == InstallState::Foreign;
    // NotInstalled with the program kept (an uninstall that said "uninstall again to finish") keeps
    // Uninstall… so that the window can finish it; a clean machine has none.
    m_uninstall->setVisible(known && (inst != InstallState::NotInstalled || m_service.programRemains));
    // A foreign service: disabled, with the reason as the tooltip.
    m_uninstall->setEnabled(can && !b && !refusal && stagedWhy.isEmpty());
    m_uninstall->setToolTip(refusal ? m_service.installationDetail : !can ? m_actions.unavailableReason() : stagedWhy);
    m_restartAdmin->setVisible(!can && bool(m_options.restartAsAdmin));
    m_restartAdmin->setEnabled(!b);
    m_waitCancel->setVisible(m_waiting);
}

void BridgeWindow::setFooter(const QString& text, const QString& tone)
{
    m_footer->setText(text);
    setTone(m_footer, tone);
}

void BridgeWindow::setBanner(const QString& text, const QString& tone)
{
    m_banner->setText(text);
    m_banner->setVisible(!text.isEmpty());
    setTone(m_banner, tone);
}

void BridgeWindow::updateProgress()
{
    // Not a result and not the banner (an interim keeps the banner, D57): the footer while the operation runs, so a
    // slow step (the firewall provider, the 15 s wait for the bridge) does not look hung.
    if (m_waiting) {
        const qint64 secs = m_waitClock.isValid() ? m_waitClock.elapsed() / 1000 : 0;
        setFooter(QStringLiteral("Waiting for the window's firewall check (%1 s)… %2 starts when it ends.").arg(secs).arg(m_deferredWhat),
                  QStringLiteral("info"));
        return;
    }
    if (!busy() || m_operation.isEmpty()) {
        m_progressTimer->stop();
        return;
    }
    const QString step = m_actions.progress();
    const qint64 secs = m_operationClock.isValid() ? m_operationClock.elapsed() / 1000 : 0;
    setFooter(QStringLiteral("%1…%2 · %3 s").arg(m_operation, step.isEmpty() ? QString() : QStringLiteral(" ") + step).arg(secs),
              QStringLiteral("info"));
}

void BridgeWindow::setBusy(bool on)
{
    if (!on) {
        if (m_progressTimer) m_progressTimer->stop();
        // Back to the standing banner: read-only, or nothing.
        if (m_actions.available()) setBanner({}, QStringLiteral("info"));
        else setBanner(m_actions.unavailableReason(), QStringLiteral("warn"));
    }
#ifdef _WIN32
    // While an operation is outstanding, Windows is told why the session should not end (and WM_QUERYENDSESSION is
    // answered FALSE in nativeEvent). No promise if Windows ends it anyway (R18).
    if (on != m_busyShown && QGuiApplication::platformName() == QLatin1String("windows")) {
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        if (on) ShutdownBlockReasonCreate(hwnd, L"Aether Serial Bridge is still finishing an operation.");
        else ShutdownBlockReasonDestroy(hwnd);
    }
#endif
    m_busyShown = on;
    updateButtons();
}

// ---------------------------------------------------------------------------------------------------
// The details card
// ---------------------------------------------------------------------------------------------------

void BridgeWindow::applyDetails(bool on)
{
    if (on == m_detailsExpanded) {
        updateDetailsMarker();
        return;
    }
    // The card keeps everything (it is only hidden): the working copy, the fields, the selection.
    if (!on) {
        const int cardHeight = (m_cardHost->isVisible() ? m_cardHost->height() : m_cardHost->sizeHint().height()) + kBodySpacing;
        m_expandedHeight = height();
        m_cardHost->hide();
        setMinimumHeight(kMinHeightCollapsed);
        if (isVisible()) resize(width(), std::max(kMinHeightCollapsed, height() - cardHeight));
    } else {
        const int restore = m_expandedHeight > 0 ? m_expandedHeight : height() + m_cardHost->sizeHint().height() + kBodySpacing;
        m_cardHost->show();
        setMinimumHeight(kMinHeightExpanded);
        if (isVisible()) resize(width(), std::max(kMinHeightExpanded, restore));
    }
    m_detailsExpanded = on;
    m_details->setText(on ? QStringLiteral("Details ▴") : QStringLiteral("Details ▾"));
    m_details->setAccessibleDescription(on ? QStringLiteral("expanded") : QStringLiteral("collapsed"));
    updateDetailsMarker();
}

void BridgeWindow::setDetailsExpanded(bool on)
{
    m_autoExpanded = false;
    applyDetails(on);
    auto& settings = AetherSDR::AppSettings::instance();
    settings.setValue(QString::fromLatin1(kDetailsKey), on);
    settings.save();
}

void BridgeWindow::expandForEditing()
{
    if (m_detailsExpanded) return;
    m_autoExpanded = true;
    applyDetails(true);
}

bool BridgeWindow::autoCollapse()
{
    // Only a card an edit action opened, and never while something is unsaved or an operation runs.
    if (!m_autoExpanded || !m_detailsExpanded || m_editor.dirty() || busy()) return false;
    m_autoExpanded = false;
    applyDetails(false);
    return true;
}

bool BridgeWindow::detailsMarkerVisible() const
{
    return !m_detailsMarker->isHidden();
}

void BridgeWindow::updateDetailsMarker()
{
    if (m_detailsMarker) m_detailsMarker->setVisible(!m_detailsExpanded && m_editor.dirty());
}

void BridgeWindow::onEdited()
{
    updateDetailsMarker();
    setFooter(QStringLiteral("Unsaved changes. Click %1.").arg(mainLabel(mainActionFor(m_service), false)),
              QStringLiteral("warn"));
    updateButtons();
}

QString BridgeWindow::footerText() const { return m_footer->text(); }
QString BridgeWindow::bannerText() const { return m_banner->isHidden() ? QString() : m_banner->text(); }
QString BridgeWindow::pendingNoteText() const { return m_pendingNote->text(); }
QString BridgeWindow::serviceText() const { return m_serviceText->text(); }

QPushButton* BridgeWindow::button(const QString& objectName) const
{
    return findChild<QPushButton*>(objectName);
}

// ---------------------------------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------------------------------

void BridgeWindow::addPort()
{
    if (!m_actions.available() || !m_editor.loaded()) return;
    expandForEditing();   // the new port is edited in the card
    // A new port: the first enumerated device not used yet, the next TCP port, the first real address.
    QStringList used;
    int maxTcp = 0;
    for (const PortMapping& p : m_editor.config().ports) {
        used << p.comPort.trimmed();
        maxTcp = std::max(maxTcp, p.tcpPort);
    }
    QString com;
    for (const QString& d : deviceChoices()) {
        bool inUse = false;
        for (const QString& u : used)
            if (equalsOrdinalIgnoreCase(u, d)) inUse = true;
        if (!inUse) {
            com = d;
            break;
        }
    }
    QString bind = QStringLiteral("127.0.0.1");
    for (const QString& a : m_options.localAddresses ? m_options.localAddresses() : defaultLocalAddresses())
        if (a != QLatin1String("127.0.0.1") && a != QLatin1String("0.0.0.0")) {
            bind = a;
            break;
        }
    PortMapping p;
    p.name = com;
    p.comPort = com;
    p.tcpPort = m_editor.config().ports.isEmpty() ? 4001 : maxTcp + 1;
    p.bindAddress = bind;
    m_editor.edit().ports.append(p);
    m_model->reset();
    const int row = int(m_editor.config().ports.size()) - 1;
    m_table->selectionModel()->setCurrentIndex(m_model->index(row, 0),
                                               QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_card->setRow(row);
    onEdited();
}

void BridgeWindow::removeSelectedPort()
{
    const int row = m_card->row();
    if (!m_actions.available() || row < 0 || row >= m_editor.config().ports.size()) return;
    m_editor.edit().ports.removeAt(row);
    m_model->reset();
    const int next = std::min(row, int(m_editor.config().ports.size()) - 1);
    if (next >= 0)
        m_table->selectionModel()->setCurrentIndex(m_model->index(next, 0),
                                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_card->setRow(next);
    onEdited();
}

QStringList BridgeWindow::validationErrors() const
{
    return m_editor.config().validate();
}

// ---------------------------------------------------------------------------------------------------
// Operations (one busy gate, D57)
// ---------------------------------------------------------------------------------------------------

BridgeWindow::Basis BridgeWindow::basisOf(const ServiceView& v, const Snapshot* pending)
{
    Basis b;
    b.queried = v.queried;
    b.installation = v.installation;
    b.otherBuild = v.otherBuild;
    b.installedSha256 = v.installedSha256;
    b.programRemains = v.programRemains;
    if (pending) {
        b.withPending = true;
        if (pending->pending.present) b.pendingSha = sha256Hex(pending->pending.bytes).toLatin1();
    }
    return b;
}

bool BridgeWindow::submit(const std::function<bool(IOperationObserver&)>& op, const QString& what,
                          const std::optional<Basis>& basis)
{
    if (!m_actions.available() || busy()) return false;
    // The window's own stale-rule check may still be inside WMI: the operation (which syncs the firewall and starts or
    // stops the service) waits for it, but never on this thread (INV-129): it is deferred, visibly,
    // with a Cancel, and starts when the check ends.
    if (m_options.firewallIdle) {
        const quint64 epoch = ++m_waitEpoch;
        QPointer<BridgeWindow> self(this);
        if (!m_options.firewallIdle([self, epoch] {
                if (self) self->startDeferred(epoch);
            })) {
            m_waiting = true;
            m_idleWhileHeld.reset();
            m_deferredOp = op;
            m_deferredWhat = what;
            m_deferredBasis = basis ? *basis : basisOf(m_service);
            m_waitClock.start();
            updateProgress();
            m_progressTimer->start();
            updateButtons();
            return true;
        }
    }
    return startOperation(op, what);
}

bool BridgeWindow::startOperation(const std::function<bool(IOperationObserver&)>& op, const QString& what)
{
    m_operation = what;
    if (!op(*this)) {
        m_inFlight.reset();
        m_operation.clear();
        setFooter(QStringLiteral("%1 could not be started.").arg(what), QStringLiteral("bad"));
        return false;
    }
    setBusy(true);
    m_operationClock.start();
    setFooter(QStringLiteral("%1…").arg(what), QStringLiteral("info"));
    m_progressTimer->start();
    return true;
}

void BridgeWindow::startDeferred(quint64 epoch)
{
    // Only the wait it was registered for (a cancelled wait's callback, or one after the window moved on, does nothing).
    if (!m_waiting || epoch != m_waitEpoch) return;
    // A question is open (its modal loop delivered this): nothing starts behind it. The wait stays; the question's
    // end posts this again (releaseDeferred), so "Keep editing" resumes it and Discard has cancelled it by then.
    if (m_deferHolds > 0) {
        m_idleWhileHeld = epoch;
        return;
    }
    m_waiting = false;
    const auto op = std::move(m_deferredOp);
    const QString what = m_deferredWhat;
    m_deferredOp = {};
    m_deferredWhat.clear();
    m_waitCancel->hide();
    // The questions and the request were decided on the evidence of the click, maybe long ago (the check's connect is
    // bounded at about 2 minutes, WbemSession): read again, and an installation or a pending file that changed since
    // starts nothing. The user decides again on what the window now shows.
    const Snapshot fresh = m_view.load();
    const Basis now = basisOf(fresh.service, m_deferredBasis.withPending ? &fresh : nullptr);
    if (now != m_deferredBasis) {
        Basis installationOnly = now;
        installationOnly.pendingSha = m_deferredBasis.pendingSha;
        const QString changed = installationOnly == m_deferredBasis
                                    ? QStringLiteral("the saved, not applied settings (config.pending.json) changed")
                                    : QStringLiteral("the installation changed");
        m_inFlight.reset();
        m_discardConsentRevision.reset();
        refreshEvidence(fresh);
        setFooter(QStringLiteral("%1 was not started: %2 while it waited for the window's firewall check. Nothing was "
                                 "changed; check what the window shows now and try again.")
                      .arg(what, changed),
                  QStringLiteral("warn"));
        return;
    }
    if (!startOperation(op, what)) {
        m_discardConsentRevision.reset();
        updateButtons();
    }
}

void BridgeWindow::releaseDeferred()
{
    if (--m_deferHolds > 0 || !m_idleWhileHeld) return;
    const quint64 epoch = *m_idleWhileHeld;
    m_idleWhileHeld.reset();
    // Posted, as the check's own whenIdle is, never run here: the caller acts on the answer first (mayClose's Discard
    // cancels the wait, so the posted start does nothing), and the start reads the evidence again.
    QPointer<BridgeWindow> self(this);
    QMetaObject::invokeMethod(this, [self, epoch] { if (self) self->startDeferred(epoch); }, Qt::QueuedConnection);
}

void BridgeWindow::cancelWaiting()
{
    if (!m_waiting) return;
    m_waiting = false;
    ++m_waitEpoch;
    m_idleWhileHeld.reset();
    m_deferredOp = {};
    const QString what = m_deferredWhat;
    m_deferredWhat.clear();
    m_inFlight.reset();
    m_discardConsentRevision.reset();
    m_progressTimer->stop();
    setFooter(QStringLiteral("%1 was cancelled; nothing was started.").arg(what), QStringLiteral("warn"));
    setBusy(false);
}

BridgeWindow::MainAction BridgeWindow::mainActionFor(const ServiceView& v) const
{
    // An unknown start type is never the untick route, so it counts as ticked here as in prepareSave.
    const bool ticked = !m_autoStartKnown || m_editor.autoStart();
    if (v.queried && v.installation == InstallState::NotInstalled) {
        if (ticked) return MainAction::Install;
        return m_actions.savesWithoutService() ? MainAction::SaveOnly : MainAction::None;
    }
    return ticked ? MainAction::Apply : MainAction::Untick;
}

QString BridgeWindow::mainLabel(MainAction a, bool forButton)
{
    switch (a) {
    case MainAction::Install: return QStringLiteral("Install and apply");
    case MainAction::SaveOnly:
    case MainAction::None: return QStringLiteral("Save");
    case MainAction::Apply:
    case MainAction::Untick: break;
    }
    return forButton ? QStringLiteral("Save && apply") : QStringLiteral("Save & apply");
}

std::optional<BridgeWindow::Prepared> BridgeWindow::prepareSave(bool forInstall, std::optional<MainAction> expected)
{
    if (!m_actions.available() || busy() || !m_editor.loaded()) return std::nullopt;
    if (m_snapshot.folder == FolderView::Untrusted) {
        m_asker->notify(QStringLiteral("untrusted"),
                        QStringLiteral("The data folder is not trusted, so nothing can be saved there. Click Repair… first."));
        return std::nullopt;
    }
    const QStringList errors = validationErrors();
    if (!errors.isEmpty()) {
        m_asker->notify(QStringLiteral("invalid"), QStringLiteral("Fix these before saving:\n\n• ") + errors.join(QStringLiteral("\n• ")));
        return std::nullopt;
    }
    // Consent first, outside any lock (master §4.6 "Interactive flow"); a cancel changes nothing (INV-35).
    // A port is exposed when it binds every interface (by value, BindAddress.h) and nothing restricts its clients. On
    // Linux the allow-list is recorded, not enforced (D7), so it restricts nothing there (as the TUI asks).
    const bool allowListEnforced = m_actions.flavor() == Installation::Flavor::Windows;
    QStringList exposed;
    for (const PortMapping& p : m_editor.config().ports)
        if (p.enabled && bindsEveryInterface(p.bindAddress) && (!allowListEnforced || p.allowedClients.trimmed().isEmpty()))
            exposed << p.displayName().trimmed();
    if (!exposed.isEmpty()) {
        // The sentence names the count and the first two; the whole list is bounded and scrolls (WS-17 §2).
        Question q;
        q.id = QStringLiteral("exposure");
        q.text = (allowListEnforced
                      ? QStringLiteral("%1 will listen on every network interface with no client restriction. Anyone who can "
                                       "reach this computer could operate that device.")
                      : QStringLiteral("%1 will listen on every network interface. On Linux the allowed clients are recorded, "
                                       "not enforced: anyone who can reach this computer could operate that device unless a "
                                       "firewall rule stops them."))
                     .arg(namedCount(exposed));
        if (exposed.size() > 2) q.list = exposed;
        q.after = QStringLiteral("Save anyway?");
        q.choices = {Choice{QStringLiteral("cancel"), QStringLiteral("Cancel")}, Choice{QStringLiteral("save"), QStringLiteral("Save anyway")}};
        q.focus = QStringLiteral("cancel");
        if (!m_asker->ask(q).is("save")) return std::nullopt;
    }
    // Fresh evidence before the route: the installation, its state and the pending file are read again now.
    // An installation that cannot be read changes nothing; an unknown start type never takes the untick route.
    Prepared out;
    out.now = m_view.load();
    if (!out.now.service.queried) {
        m_asker->notify(QStringLiteral("serviceUnknown"),
                        QStringLiteral("Nothing was saved: the service's state could not be read (%1). Try again; if it "
                                       "persists, check the service from a terminal (aether-serial-bridge --status).")
                            .arg(out.now.service.error));
        return std::nullopt;
    }
    // The route the click takes is the one the label named (v1.1 fix after WS-17): an installation that changed since the
    // window last read it (installed or removed elsewhere) saves nothing; the window shows the new evidence and a label
    // for it, and the edits stay.
    if (expected && mainActionFor(out.now.service) != *expected) {
        refreshEvidence(out.now);
        setFooter(QStringLiteral("Nothing was saved: the installation changed since the window last read it, so the button "
                                 "now reads %1. Check what the window shows and click again.")
                      .arg(mainLabel(mainActionFor(m_service), false)),
                  QStringLiteral("warn"));
        return std::nullopt;
    }
    const bool untick = !forInstall && m_autoStartKnown && !m_editor.autoStart();
    if (untick && out.now.service.presence() == ServiceView::Presence::Present &&
        !m_asker->confirm(QStringLiteral("untick"),
                          m_actions.flavor() == Installation::Flavor::Windows
                              ? QStringLiteral("Turning off auto-start removes the service and its firewall rules: these ports "
                                               "stop being bridged until you turn it back on. Your settings are saved, not "
                                               "applied.\n\nContinue?")
                              : QStringLiteral("Turning off auto-start stops the bridge and keeps it from starting: these ports "
                                               "stop being bridged until you turn it back on. Your settings are saved, not "
                                               "applied.\n\nContinue?"),
                          QStringLiteral("Continue"), QStringLiteral("Cancel")))
        return std::nullopt;
    out.submission = m_editor.submission(!untick);
    // INV-102: a pending file someone else wrote since the load is replaced only when the user says so.
    const QByteArray pendingSha = out.now.pending.present ? sha256Hex(out.now.pending.bytes).toLatin1() : QByteArray();
    if (EditorModel::pendingConflicts(pendingSha, out.submission)) {
        if (!m_asker->confirm(QStringLiteral("supersede"),
                              QStringLiteral("The saved, not applied settings (config.pending.json) were changed since you "
                                             "loaded them, for example from the command line. Replace them with what is in "
                                             "this window?"),
                              QStringLiteral("Replace"), QStringLiteral("Cancel")))
            return std::nullopt;
        out.submission.supersedePending = true;
    }
    return out;
}

QString BridgeWindow::installedBuildPhrase(const ServiceView& v) const
{
    // "Installed: version 1.1.0 (program sha256 0123456789ab). This window: 1.2.0, program sha256 abcdef012345."
    const QString thisVersion = m_options.thisVersion.isEmpty()
                                    ? QString::fromLatin1(core::version().data(), qsizetype(core::version().size()))
                                    : m_options.thisVersion;
    QString what = core::describeInstalledVersion(v.installedVersion, thisVersion);
    if (what.isEmpty()) what = QStringLiteral("a different build");
    const QString digest = v.installedSha256.isEmpty() ? QString() : QStringLiteral(" (program sha256 %1)").arg(v.installedSha256.left(12));
    const QString mine = m_options.thisBuild.isEmpty() ? QStringLiteral("this build") : m_options.thisBuild;
    return QStringLiteral("Installed: %1%2. This window: %3.").arg(what, digest, mine);
}

QString BridgeWindow::buildQuestionText(const ServiceView& v) const
{
    return QStringLiteral("The installed bridge is a different build. %1 Update it to this build? The ports restart.")
        .arg(installedBuildPhrase(v));
}

void BridgeWindow::saveAndApply()
{
    // The label names the route (v1.1 fix after WS-17). Nothing installed and the box ticked: "Install and apply" is
    // Install the bridge service with the editor's ports, its question and checks included (it supersedes WS-17 call
    // 1, which asked nothing and, with the box unticked as it then loaded, saved and removed instead).
    const MainAction shown = mainActionFor(m_service);
    if (shown == MainAction::None) return;   // disabled: no route saves without installing here
    if (shown == MainAction::Install) {
        installFor(shown);
        return;
    }
    std::optional<Prepared> p = prepareSave(/*forInstall=*/false, shown);
    if (!p) return;
    const Submission& s = p->submission;
    const Snapshot& now = p->now;
    if (shown == MainAction::SaveOnly) {
        // "Save": the settings are saved, not applied, and nothing else changes (never save-and-remove, which on a
        // machine with nothing installed would still go to the firewall).
        m_inFlight = s;
        const SaveRequest req = s.request();
        submit([this, req](IOperationObserver& o) { return m_actions.saveOnly(req, o); }, QStringLiteral("Saving"),
               basisOf(now.service, &now));
        return;
    }
    const bool install = s.install;
    // A complete installation of another build is replaced only when the user says so (user report 2026-10-16: the
    // window installed its own program over the station's without asking). Three choices (D67): Update, No, keep it
    // (the focus: the settings go to the installed service and its program stays) and Cancel; Esc and the close button
    // are Cancel, which submits nothing (INV-131). An install that did not finish (a program missing, a lock file, the
    // service's configuration) is finished as before.
    bool runInstall = now.service.installation != InstallState::Current;
    bool keepInstalledBuild = false;
    if (install && now.service.installation == InstallState::IncompleteInstall && now.service.otherBuild) {
        Question q;
        q.id = QStringLiteral("build");
        q.text = buildQuestionText(now.service);
        q.after = QStringLiteral("No, keep it applies the settings to the installed program and keeps it. Cancel changes nothing.");
        q.choices = {Choice{QStringLiteral("cancel"), QStringLiteral("Cancel")},
                     Choice{QStringLiteral("keep"), QStringLiteral("No, keep it")},
                     Choice{QStringLiteral("update"), QStringLiteral("Update")}};
        q.focus = QStringLiteral("keep");
        const Answer a = m_asker->ask(q);
        if (!a.is("update") && !a.is("keep")) {
            setFooter(QStringLiteral("Cancelled; nothing was changed."), QStringLiteral("info"));
            return;
        }
        runInstall = a.is("update");
        keepInstalledBuild = !runInstall;
    }
    m_inFlight = s;
    SaveRequest req = s.request();
    req.keepInstalledBuild = keepInstalledBuild;
    const Basis basis = basisOf(now.service, &now);
    if (install)
        submit([this, req, runInstall](IOperationObserver& o) { return m_actions.run(req, runInstall, o); },
               QStringLiteral("Saving and applying"), basis);
    else
        submit([this, req](IOperationObserver& o) { return m_actions.untick(req, o); }, QStringLiteral("Saving"), basis);
}

void BridgeWindow::installService()
{
    installFor(std::nullopt);
}

void BridgeWindow::installFor(std::optional<MainAction> expected)
{
    if (!m_actions.available() || busy() || !m_editor.loaded()) return;
    if (const QString why = m_actions.lifecycleUnavailableReason(); !why.isEmpty()) {
        setFooter(why, QStringLiteral("warn"));
        return;
    }
    // Save & apply's checks and questions first (the exposure question before the install question, INV-35).
    std::optional<Prepared> p = prepareSave(/*forInstall=*/true, expected);
    if (!p) return;
    if (p->now.service.installation == InstallState::Current) {
        // Installed elsewhere since the window last read it: the window shows that installation, so the
        // Install button goes and the main button reads Save & apply on it; the edits stay.
        refreshEvidence(p->now);
        setFooter(QStringLiteral("The bridge service is already installed; nothing was changed."), QStringLiteral("info"));
        return;
    }
    const bool other = p->now.service.installation == InstallState::IncompleteInstall && p->now.service.otherBuild;
    Question q;
    q.id = other ? QStringLiteral("update") : QStringLiteral("install");
    if (other) {
        q.text = buildQuestionText(p->now.service);
        q.choices = {Choice{QStringLiteral("cancel"), QStringLiteral("Cancel")}, Choice{QStringLiteral("update"), QStringLiteral("Update")}};
    } else {
        const QString os = m_actions.flavor() == Installation::Flavor::Windows
                               ? QStringLiteral("It installs the program in C:\\Program Files\\AetherSerialBridge, a Windows "
                                                "service that starts at boot, a firewall rule per enabled port and an Apps & "
                                                "features entry.")
                               : QStringLiteral("polkit will ask for an administrator's password. Its prompt says it runs /bin/sh "
                                                "as the super user: that is this window copying the program inside it into "
                                                "/usr/local/bin and installing the service, nothing else.");
        q.text = QStringLiteral("Install the bridge service? %1 The ports in this window are applied and the bridge starts.").arg(os);
        q.choices = {Choice{QStringLiteral("cancel"), QStringLiteral("Cancel")}, Choice{QStringLiteral("install"), QStringLiteral("Install")}};
    }
    q.focus = QStringLiteral("cancel");
    const Answer a = m_asker->ask(q);
    if (!a.is("install") && !a.is("update")) {
        setFooter(QStringLiteral("Cancelled; nothing was changed."), QStringLiteral("info"));
        return;
    }
    m_inFlight = p->submission;
    SaveRequest req = p->submission.request();
    // The installation the questions were decided on: the transaction refuses under apply.lock if it is no longer this
    // one (another build installed while the install or update question was open). In process only.
    req.expectInstallation = SaveRequest::InstallationBasis{true, p->now.service.installation, p->now.service.otherBuild,
                                                            p->now.service.installedSha256};
    const Basis basis = basisOf(p->now.service, &p->now);
    if (other)
        submit([this, req](IOperationObserver& o) { return m_actions.update(req, o); }, QStringLiteral("Updating"), basis);
    else
        submit([this, req](IOperationObserver& o) { return m_actions.install(req, o); }, QStringLiteral("Installing"), basis);
}

void BridgeWindow::updateService()
{
    installService();   // the same pre-flight; another build gets the update question and the update operation
}

void BridgeWindow::uninstallService()
{
    if (!m_actions.available() || busy()) return;
    if (const QString why = m_actions.lifecycleUnavailableReason(); !why.isEmpty()) {
        setFooter(why, QStringLiteral("warn"));
        return;
    }
    // Fresh evidence for what goes down (WS-16's preview: the ports, the connected clients, the settings folder).
    const Snapshot now = m_view.load();
    if (!now.service.queried) {
        m_asker->notify(QStringLiteral("serviceUnknown"),
                        QStringLiteral("Nothing was changed: the service's state could not be read (%1).").arg(now.service.error));
        return;
    }
    // Not installed: nothing to do, unless an earlier uninstall kept the program, which this one
    // finishes, the --intent=uninstall of an elevated window included.
    const bool finishing = now.service.installation == InstallState::NotInstalled && now.service.programRemains;
    if (now.service.installation == InstallState::NotInstalled && !finishing) {
        setFooter(QStringLiteral("The bridge service is not installed; nothing was changed."), QStringLiteral("info"));
        return;
    }
    if (now.service.installation == InstallState::Foreign) {
        m_asker->notify(QStringLiteral("uninstallRefused"), now.service.installationDetail);
        return;
    }
    Installation inst;
    inst.state = now.service.installation;
    inst.otherBuild = now.service.otherBuild;
    inst.installedVersion = now.service.installedVersion;
    OpenedFolder folder;
    folder.usable = now.folder == FolderView::Trusted;
    folder.missing = now.folder == FolderView::Missing;
    const BridgeConfig committed = now.committed.config.value_or(BridgeConfig{});
    const core::UninstallPreview preview =
        core::uninstallPreview(committed, now.status, inst, folder, QDir::toNativeSeparators(m_view.dataFolder()));
    const bool windows = m_actions.flavor() == Installation::Flavor::Windows;
    Question q;
    q.id = QStringLiteral("uninstall");
    q.text = finishing ? QStringLiteral("Finish uninstalling? The bridge service was removed, but its program is still there.")
             : preview.portsGoingDown.isEmpty()
                 ? QStringLiteral("Uninstall the bridge service? No port is being bridged now.")
                 : QStringLiteral("Uninstall the bridge service? %1 stop%2 being bridged.")
                       .arg(namedCount(preview.portsGoingDown), preview.portsGoingDown.size() == 1 ? QStringLiteral("s") : QString());
    q.list = preview.lines;
    QStringList after;
    // Finishing, the service is already gone: the sentence names only what is left.
    const bool leftBehind = finishing;
    if (windows)
        after << (leftBehind ? QStringLiteral("It removes the program left behind and its folder %1 (and the Apps & "
                                              "features entry, if any).")
                             : QStringLiteral("It removes the service, the program and its folder %1, the firewall rules "
                                              "and the Apps & features entry."))
                     .arg(QStringLiteral("C:\\Program Files\\AetherSerialBridge"));
    else
        after << (leftBehind ? QStringLiteral("It removes the program left behind in /usr/local/bin, the polkit policy and "
                                              "/var/lib/aether-serial-bridge.")
                             : QStringLiteral("It removes the service, the program in /usr/local/bin, the polkit policy and "
                                              "/var/lib/aether-serial-bridge.")) +
                     QStringLiteral(" polkit will ask for an administrator's password.");
    const QString thisVersion = m_options.thisVersion.isEmpty()
                                    ? QString::fromLatin1(core::version().data(), qsizetype(core::version().size()))
                                    : m_options.thisVersion;
    if (preview.installedVersion && !preview.installedVersion->isEmpty()) {
        const QVersionNumber installed = QVersionNumber::fromString(*preview.installedVersion);
        const QVersionNumber mine = QVersionNumber::fromString(thisVersion);
        if (!installed.isNull() && !mine.isNull() && QVersionNumber::compare(installed, mine) > 0)
            after << QStringLiteral("The installed bridge is a newer build (%1) than this window (%2): uninstall it with that "
                                    "build's window, which knows everything it installed; this one may leave files behind.")
                         .arg(*preview.installedVersion, thisVersion);
    }
    if (!preview.settingsTrusted && now.folder != FolderView::Missing)
        after << QStringLiteral("Your settings folder is not trusted; it will not be touched.");
    else if (now.folder == FolderView::Missing)
        after << QStringLiteral("There are no settings to keep or remove.");
    q.after = after.join(QStringLiteral("\n\n"));
    if (preview.settingsTrusted)
        q.checkbox = QStringLiteral("Also remove my settings (the port list in %1)").arg(preview.settingsFolder);
    q.choices = {Choice{QStringLiteral("cancel"), QStringLiteral("Cancel")}, Choice{QStringLiteral("uninstall"), QStringLiteral("Uninstall")}};
    q.focus = QStringLiteral("cancel");
    q.destructive = QStringLiteral("uninstall");
    const Answer a = m_asker->ask(q);
    if (!a.is("uninstall")) {
        setFooter(QStringLiteral("Cancelled; nothing was changed."), QStringLiteral("info"));
        return;
    }
    // The editor is never changed by an uninstall: dirty edits stay dirty (no m_inFlight).
    UninstallOptions options;
    options.removeSettings = preview.settingsTrusted && a.checked;
    submit([this, options](IOperationObserver& o) { return m_actions.uninstall(options, o); }, QLatin1String(kUninstalling),
           basisOf(now.service));
}

QString BridgeWindow::lifecycleIntent() const
{
    if (!m_service.queried) return QStringLiteral("none");
    switch (m_service.installation) {
    case InstallState::NotInstalled: return QStringLiteral("install");
    case InstallState::IncompleteInstall: return m_service.otherBuild ? QStringLiteral("update") : QStringLiteral("install");
    default: return QStringLiteral("none");
    }
}

void BridgeWindow::restartAsAdmin()
{
    if (!m_options.restartAsAdmin || m_actions.available() || busy()) return;
    if (m_editor.dirty()) {
        Question q;
        q.id = QStringLiteral("restartAdmin");
        q.text = QStringLiteral("Your unsaved changes will be lost when the window restarts. Restart anyway?");
        q.choices = {Choice{QStringLiteral("cancel"), QStringLiteral("Cancel")}, Choice{QStringLiteral("restart"), QStringLiteral("Restart")}};
        q.focus = QStringLiteral("cancel");
        if (!m_asker->ask(q).is("restart")) {
            setFooter(QStringLiteral("Cancelled; nothing was changed."), QStringLiteral("info"));
            return;
        }
    }
    const BridgeWindowOptions::Relaunch r = m_options.restartAsAdmin(lifecycleIntent());
    if (r.started) {
        // The elevated window takes over; this one closes without asking again (the loss was agreed above).
        m_closeApproved = true;
        close();
        m_closeApproved = false;
        return;
    }
    if (r.cancelled) setFooter(QStringLiteral("Cancelled; nothing was changed."), QStringLiteral("info"));
    else setFooter(QStringLiteral("The window could not be restarted as administrator: %1.").arg(r.error), QStringLiteral("bad"));
}

void BridgeWindow::startService()
{
    submit([this](IOperationObserver& o) { return m_actions.start(o); }, QStringLiteral("Starting"));
}

void BridgeWindow::stopService()
{
    // Stop waits for a running apply (D13): "Waiting for the apply in progress…" is the interim.
    submit([this](IOperationObserver& o) { return m_actions.stop(o); }, QStringLiteral("Stopping"));
}

void BridgeWindow::repair()
{
    if (!m_actions.available() || busy()) return;
    if (!m_asker->confirm(QStringLiteral("repair"),
                          QStringLiteral("Move the untrusted folder %1 aside (to a new name beside it) and start with an empty "
                                         "one? Nothing is deleted; the old folder and its config.json stay there for you to "
                                         "review. The bridge is stopped and stays off until you Save & apply.")
                              .arg(m_view.dataFolder()),
                          QStringLiteral("Yes"), QStringLiteral("No")))
        return;
    submit([this](IOperationObserver& o) { return m_actions.repair(o); }, QStringLiteral("Repairing"));
}

void BridgeWindow::discardPending()
{
    if (!m_actions.available() || busy()) return;
    // The digest of the pending bytes this window loaded and shows (INV-127): the core discards only those.
    if (!m_editor.fromPending()) return;
    const QString shown = QString::fromLatin1(m_editor.source().sha256);
    const bool dirty = m_editor.dirty();
    if (!m_asker->confirm(QStringLiteral("discardPending"),
                          QStringLiteral("Discard the saved, not applied settings (config.pending.json)? The window then "
                                         "shows the applied settings.") +
                              (dirty ? QStringLiteral(" This also discards the unsaved changes in this window.") : QString()),
                          QStringLiteral("Discard"), QStringLiteral("Keep them")))
        return;
    if (dirty) m_discardConsentRevision = m_editor.revision();
    if (!submit([this, shown](IOperationObserver& o) { return m_actions.discardPending(shown, o); }, QLatin1String(kDiscarding)))
        m_discardConsentRevision.reset();
}

void BridgeWindow::interim(const Interim& i)
{
    // Never a result (D57): a banner while the operation goes on; the editor stays open and editable.
    setBanner(QStringLiteral("Still processing: %1").arg(i.message.isEmpty() ? i.phase : i.message), QStringLiteral("warn"));
}

QString BridgeWindow::outcomeTone(const Result& r) const
{
    switch (r.outcome) {
    case Outcome::Ok: return QStringLiteral("good");
    case Outcome::SavedNotApplied: return QStringLiteral("warn");
    case Outcome::NothingChanged: return QStringLiteral("warn");
    case Outcome::Uncommitted:
    case Outcome::CommittedNotRunning:
    case Outcome::StateUnknown: return QStringLiteral("bad");
    }
    return QStringLiteral("info");
}

void BridgeWindow::finished(const Result& r)
{
    bool newerEdits = false;
    if (m_inFlight) {
        const Submission s = *m_inFlight;
        m_inFlight.reset();
        const bool allSaved = m_editor.finish(r, s);   // INV-107: only persisted != None with the submitted digest
        newerEdits = EditorModel::persistedMatches(r, s) && !allSaved;
    }
    const QString what = m_operation;
    m_operation.clear();
    // After every operation (Save & apply, Start, Stop, Repair, Discard, a failed apply) the window shows what is on
    // disk and the real start type, unless edits are unsaved (INV-23); with unsaved edits only the service view is
    // refreshed.
    // Discard saved changes with unsaved edits in the window: the user agreed to lose the edits made up to the
    // question, so the window shows the applied settings; edits made while the discard ran are kept, and the working
    // copy no longer claims to be the (deleted) pending file.
    const bool discarded = what == QLatin1String(kDiscarding) && r.outcome == Outcome::Ok;
    const bool consentedLoss = discarded && m_discardConsentRevision && *m_discardConsentRevision == m_editor.revision();
    m_discardConsentRevision.reset();
    if ((!m_editor.dirty() || consentedLoss) && !what.isEmpty()) {
        applyLoad(m_view.load());
    } else {
        // Newer edits stay in the editor, but everything the window knows about the disk and the service is read again
        //: the committed bytes, the token's generation, the status and the service, so the "running
        // settings other than the saved config.json" note and the stale-rule check judge what was just committed.
        const Snapshot fresh = m_view.load();
        if (discarded) m_editor.detachFromPending(fresh.committed);
        refreshEvidence(fresh);
    }
    QString text = r.message;
    if (!r.fileChanges.isEmpty() && !text.contains(r.fileChanges.first()))
        text += QStringLiteral(" Also changed: %1.").arg(r.fileChanges.join(QStringLiteral("; ")));
    if (newerEdits) text += QStringLiteral(" There are newer changes: click Save & apply again.");
    else if (m_editor.dirty() && r.persisted == Persisted::None && !r.message.isEmpty())
        text += QStringLiteral(" Your edits are still open here, not saved.");
    setFooter(text, outcomeTone(r));
    // " From a terminal: <command>" (HelperClient, INV-124): the command runs as shown, so it can be copied.
    const qsizetype marker = r.message.indexOf(QLatin1String(" From a terminal: "));
    m_terminalCommand = marker >= 0 ? r.message.mid(marker + 18).trimmed() : QString();
    m_copyCommand->setVisible(!m_terminalCommand.isEmpty());
    if (m_log->isVisible()) refreshLog();
    emit operationFinished(r);
    autoCollapse();   // an edit-opened card folds again once everything is saved
    updateDetailsMarker();
    if (m_options.onFinished) m_options.onFinished(r);
}

// ---------------------------------------------------------------------------------------------------
// Log pane
// ---------------------------------------------------------------------------------------------------

void BridgeWindow::toggleLog()
{
    const bool show = !m_log->isVisible();
    m_log->setVisible(show);
    m_showLog->setText(show ? QStringLiteral("Hide log") : QStringLiteral("Show log"));
    if (show) {
        refreshLog();
        m_logTimer->start();
    } else {
        m_logTimer->stop();
    }
}

void BridgeWindow::refreshLog()
{
    QString why;
    const std::optional<QString> text = m_view.logTail(kLogTailBytes, &why);
    const QString shown = text ? *text : why;
    if (shown == m_log->toPlainText()) return;
    QScrollBar* bar = m_log->verticalScrollBar();
    const bool atEnd = bar->value() >= bar->maximum() - 2;
    m_log->setPlainText(shown);
    if (atEnd) bar->setValue(bar->maximum());
}

// ---------------------------------------------------------------------------------------------------
// Closing (INV-13, D57)
// ---------------------------------------------------------------------------------------------------

bool BridgeWindow::mayClose()
{
    // An operation still running refuses the close. One still waiting for the window's firewall check has not
    // started, so it does not: it is cancelled, but only once the close is approved (INV-129). A deferred
    // Save & apply always leaves the editor dirty, and "Keep editing" keeps the window AND the save it is waiting for.
    // A check that ends while the question is open starts nothing behind it (the question holds it, HoldingAsker):
    // Discard cancels the wait before the held start runs, Keep editing lets it run when the question closes.
    if (m_actions.busy()) {
        // Refused, not hidden: a hidden elevated process would hold locks and an unsaved revision nobody can see.
        m_asker->notify(QStringLiteral("busy"), QStringLiteral("An operation is still finishing; the window can close when it ends."));
        return false;
    }
    if (m_editor.dirty() &&
        !m_asker->confirm(QStringLiteral("discardEdits"),
                          m_waiting ? QStringLiteral("Discard unsaved changes? %1, waiting for the window's firewall check, "
                                                     "does not start.")
                                          .arg(m_deferredWhat)
                                    : QStringLiteral("Discard unsaved changes?"),
                          QStringLiteral("Discard"), QStringLiteral("Keep editing")))
        return false;
    cancelWaiting();
    return true;
}

void BridgeWindow::reject()
{
    // Escape and Ctrl+W come here without a close event (CanonWindow); QDialog::closeEvent calls here too.
    if (m_inCloseEvent) {
        CanonWindow::reject();
        return;
    }
    if (!mayClose()) return;
    m_closeApproved = true;
    close();
    m_closeApproved = false;
}

void BridgeWindow::closeEvent(QCloseEvent* event)
{
    if (!m_closeApproved && !mayClose()) {
        event->ignore();
        return;
    }
    m_inCloseEvent = true;
    CanonWindow::closeEvent(event);
    m_inCloseEvent = false;
}

bool BridgeWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
#ifdef _WIN32
    if (eventType == "windows_generic_MSG") {
        const MSG* msg = static_cast<const MSG*>(message);
        if (msg->message == WM_QUERYENDSESSION && busy()) {
            *result = FALSE;   // an operation is outstanding (D57); the shutdown-block reason says why
            return true;
        }
    }
#endif
    return CanonWindow::nativeEvent(eventType, message, result);
}

}  // namespace asb::gui
