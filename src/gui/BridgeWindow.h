#pragma once

// The bridge's window (master §5, WS-10): a Workspace CanonWindow built in AetherRX's order (AetherSDR
// AetherRxDialog.cpp:91-121), the composed body sheet (BridgeStyle.h, INV-69), and the editing and apply flows
// (§5.5):
//
//  - Load: ReadOnly through IViewSource. An untrusted folder shows "Config not loaded: the data folder isn't
//    trusted." and [Repair...] (INV-33). A pending file is the working copy, clean, with "Saved, not applied"
//    (INV-94). The auto-start box shows the real start type (INV-23).
//  - Save & apply: validate -> exposure confirm -> untick confirm -> pending-conflict confirm -> one operation on
//    the busy gate (IActions). Cancel at any question writes nothing and calls nothing (INV-35). Dirty is cleared
//    only for persisted != None with the submitted digest (INV-107, EditorModel); edits made meanwhile stay dirty
//    (INV-13). After every operation the window reloads from disk when nothing is unsaved (INV-23).
//  - Busy (D57, INV-118): Save & apply, Start, Stop, Repair, Discard and Reload are disabled; editing stays
//    allowed; an interim notification is a banner, never a result; closing is refused (closeEvent and reject(),
//    so Escape and Ctrl+W too); on Windows a shutdown-block reason is registered and WM_QUERYENDSESSION answered
//    FALSE.
//  - Not busy, dirty: close / Escape / Ctrl+W / Reload ask first (INV-13).
//  - The lifecycle (v1.1 WS-17): Install the bridge service / Finish installing / Update to this build (service row),
//    Uninstall… (footer), Restart as administrator (a standard user's window on Windows). Every question can be
//    cancelled with no effect (D67, INV-131); Save & apply's own questions come first (validation, exposure, the fresh
//    evidence, supersede), so INV-107's dirty rule covers an install as it does an apply.
//  - The window's own firewall check (Windows) is never waited for on this thread (INV-129): an operation submitted
//    while it runs waits in a visible "waiting" state with a Cancel, and starts when the check ends; Cancel or
//    closing the window meanwhile starts nothing.

#include "core/Interfaces.h"
#include "gui/EditorModel.h"
#include "gui/ViewSource.h"

#include "gui/CanonWindow.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

class QCheckBox;
class QFrame;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableView;
class QTimer;

namespace asb::gui {

class IActions;
class IAsker;
class PortCard;
class PortTableModel;

struct BridgeWindowOptions {
    // The 1 s status refresh. 0 = no timer (tests refresh explicitly).
    int refreshMs = 1000;
    // Linux words ("starts at boot", "not enforced") instead of Windows ones.
    bool linuxWords = false;
    // A local IPv4 list for the Listen combo (the product enumerates the interfaces).
    std::function<QStringList()> localAddresses;
    // Called with every final result (diagnostics / tests).
    std::function<void(const Result&)> onFinished;
    // "0.9.0, program sha256 0123456789ab": this build, named in the question before a different installed build is
    // replaced (user report 2026-10-16). Empty: "this build".
    QString thisBuild;
    // The details card's state when the window opens; unset: the product rule (gui.ini "DetailsExpanded", collapsed on
    // first run, and always collapsed in a read-only window). Tests set it.
    std::optional<bool> detailsExpanded;
    // Windows: re-runs the service's firewall comparator (D53, S20) for the committed config and the token's
    // generation while the installed bridge is not running; `done` on the GUI thread. Unset (Linux, tests that do
    // not ask): no check. Returns false when no check was started. The window counts a verdict only for the episode
    // it was started in (BridgeWindow::m_firewallEpisode).
    using FirewallCheck = std::function<bool(const BridgeConfig&, const FirewallGeneration&,
                                             std::function<void(IFirewallMonitor::Check, const QString& why)> done)>;
    FirewallCheck firewallCheck;
    // Whether none of the window's own firewall checks is running now (FirewallChecks::whenIdle): true = start at once;
    // false = `whenIdle` is posted to this thread when the last check ends. The window never starts, stops or
    // reconfigures the service while its own WMI query is in flight, and never blocks this thread
    // waiting for it (INV-129). Unset: nothing to wait for.
    std::function<bool(std::function<void()> whenIdle)> firewallIdle;
    // Windows, a standard user's window (D64): relaunch this program elevated with "--gui --intent=<intent>" and do
    // not wait (SelfElevate). Unset: no Restart as administrator.
    struct Relaunch {
        bool started = false;
        bool cancelled = false;   // UAC answered No (ERROR_CANCELLED): nothing ran
        QString error;
    };
    std::function<Relaunch(const QString& intent)> restartAsAdmin;
    // The elevated window's --intent (install | update | uninstall; empty or "none": nothing): that action's
    // confirmation opens once the window has loaded; the user still confirms (WS-17 §3).
    QString intent;
    // This build's version (core::version()), for the installed build's wording (WS-14). Empty: core::version().
    QString thisVersion;
    // Test builds only (tst_GuiNative's bca3030 row): also check while the service is starting, as bca3030 did.
    bool checkWhileStarting = false;
};

class BridgeWindow final : public AetherSDR::CanonWindow, private IOperationObserver {
    Q_OBJECT

public:
    using Options = BridgeWindowOptions;

    BridgeWindow(IViewSource& view, IActions& actions, IAsker* asker = nullptr, Options options = {}, QWidget* parent = nullptr);
    ~BridgeWindow() override;

    // Re-reads everything from disk (asks first when there are unsaved edits, unless `force`).
    void reload(bool force = false);
    // The 1 s refresh: service row, statuses, ACK (never the working copy).
    void refreshStatus();

    // The flows (the buttons call these; tests call them directly too).
    void saveAndApply();
    // What the main button (Save & apply) does now (v1.1 fix after WS-17). One predicate gives both the button's label
    // and the route its click takes, so the label always names the route:
    //  - Apply / Untick: an installation is there; "Save & apply" with the box ticked or unticked (unchanged);
    //  - Install: nothing installed, the box ticked; "Install and apply" runs Install the bridge service (its question
    //    included) with the editor's ports;
    //  - SaveOnly: nothing installed, the box unticked, and the window can save without a service; "Save" saves the
    //    settings as saved, not applied, and changes nothing else (never save-and-remove);
    //  - None: nothing installed, the box unticked, and no route can save without installing (the Linux window);
    //    "Save", disabled, with the reason.
    enum class MainAction { Apply, Untick, Install, SaveOnly, None };
    MainAction mainAction() const { return mainActionFor(m_service); }
    void startService();
    void stopService();
    void repair();
    void discardPending();
    // The lifecycle (WS-17): the service row's Install the bridge service / Finish installing (installService) or
    // Update to this build (updateService); Uninstall… (uninstallService, `removeSettings` only for tests that skip
    // the dialog's box: the box is what the user ticks); Restart as administrator (restartAsAdmin).
    void installService();
    void updateService();
    void uninstallService();
    void restartAsAdmin();
    // While an operation waits for the window's firewall check: Cancel (nothing is started).
    void cancelWaiting();
    bool waitingForFirewallCheck() const { return m_waiting; }
    void addPort();
    void removeSelectedPort();
    void toggleLog();

    // The port details card (owner request): folds up under the table when not editing.
    //  - setDetailsExpanded: the user's Details button; remembered in gui.ini ("DetailsExpanded").
    //  - expandForEditing: double-click or Enter on a row, + Add port: opens it, remembered as automatic.
    //  - autoCollapse: an automatically opened card folds again once nothing is unsaved and no operation runs; never
    //    while dirty or busy. Runs after every result and reload. Returns whether it folded.
    // The window's height shrinks by the card's and grows back to the height it had. Nothing in the card is cleared.
    bool detailsExpanded() const { return m_detailsExpanded; }
    void setDetailsExpanded(bool on);
    void expandForEditing();
    bool autoCollapse();
    bool detailsMarkerVisible() const;
    static constexpr char kDetailsKey[] = "DetailsExpanded";

    // State, for tests and the process self-test.
    EditorModel& editor() { return m_editor; }
    bool busy() const;
    QString footerText() const;
    // The footer's progress line now (tests call it at a chosen moment; a 500 ms timer does while busy).
    void refreshProgress() { updateProgress(); }
    QString bannerText() const;
    QString pendingNoteText() const;
    QString serviceText() const;
    bool firewallCheckRunning() const { return m_firewallChecking; }
    bool readOnly() const;
    PortTableModel* tableModel() const { return m_model; }
    QTableView* table() const { return m_table; }
    PortCard* card() const { return m_card; }
    QCheckBox* autoStartBox() const { return m_autoStart; }
    QPushButton* button(const QString& objectName) const;
    // Whether a session-end request would be refused now (Windows: WM_QUERYENDSESSION answered FALSE).
    bool refusesSessionEnd() const { return busy(); }

    void setAsker(IAsker* asker) { m_rawAsker = asker; }

signals:
    void operationFinished(const asb::Result& result);

protected:
    void closeEvent(QCloseEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

public slots:
    void reject() override;

private:
    // IOperationObserver (delivered on this thread by the OperationHost)
    void interim(const Interim&) override;
    void finished(const Result&) override;

    void build();
    void applyLoad(const Snapshot&);
    // The disk and service evidence without touching the editor (a completion with newer edits).
    void refreshEvidence(const Snapshot&);
    void fillTableAndCard();
    void updateServiceRow(const ServiceView&);
    void updateButtons();
    void updatePendingNote();
    void setFooter(const QString& text, const QString& tone);
    void setBanner(const QString& text, const QString& tone);
    void setBusy(bool on);
    void onEdited();
    bool mayClose();
    // What an operation's questions and request were decided on, compared again when a deferred operation starts
    //: the installation (its state, build and digest, a kept program) and, for the save routes, the
    // pending file. The service's run state is not in it (a Stop is not stale because the service stopped).
    struct Basis {
        bool queried = false;
        InstallState installation = InstallState::NotInstalled;
        bool otherBuild = false;
        QString installedSha256;
        bool programRemains = false;
        bool withPending = false;
        QByteArray pendingSha;   // empty: no pending file
        bool operator==(const Basis&) const = default;
    };
    static Basis basisOf(const ServiceView&, const Snapshot* pending = nullptr);
    // `basis`: what the caller decided on (a fresh load); unset = the window's current service view.
    bool submit(const std::function<bool(IOperationObserver&)>& op, const QString& what,
                const std::optional<Basis>& basis = std::nullopt);
    QStringList validationErrors() const;
    QStringList deviceChoices();
    bool installed() const;
    QString outcomeTone(const Result&) const;
    void refreshLog();
    void maybeCheckFirewall();
    // While busy: the footer says what the operation is doing and for how long (user report 2026-10-16).
    void updateProgress();
    // The questions and checks Save & apply and Install share; nullopt when the user cancelled or nothing may be saved.
    struct Prepared {
        Submission submission;
        Snapshot now;
    };
    // `expected`: the main button's action when it was clicked; a fresh read that gives another one (the installation
    // changed since the window last read it) saves nothing, so the click never takes a route its label did not name.
    std::optional<Prepared> prepareSave(bool forInstall, std::optional<MainAction> expected = std::nullopt);
    // installService() with the main button's `expected` (Install and apply): an installation that changed
    // since the window last read it gets call 27's refusal, as Save and Save & apply do.
    void installFor(std::optional<MainAction> expected);
    bool startOperation(const std::function<bool(IOperationObserver&)>& op, const QString& what);
    void startDeferred(quint64 epoch);
    // Every question the window opens holds the deferred start: a modal question runs a nested event
    // loop, which delivers the check's whenIdle, and an operation must not start behind a question still open (the
    // close question's Discard would then close the window while it writes). startDeferred() keeps the idle signal
    // while a question is open; releaseDeferred() posts it again when the last one closes, after the caller's answer.
    class HoldingAsker;
    void releaseDeferred();
    QString lifecycleIntent() const;
    MainAction mainActionFor(const ServiceView&) const;
    static QString mainLabel(MainAction, bool forButton);
    QString installedBuildPhrase(const ServiceView&) const;
    QString buildQuestionText(const ServiceView&) const;
    // New evidence or the end of an episode: an outstanding check's verdict is dropped, the shown one is cleared, and
    // a check for the current evidence may start.
    void newFirewallEpisode();
    void applyDetails(bool on);
    void updateDetailsMarker();

    IViewSource& m_view;
    IActions& m_actions;
    IAsker* m_rawAsker;                    // the product's CanonAsker, or the caller's (tests, the self-test)
    std::unique_ptr<IAsker> m_ownAsker;
    std::unique_ptr<IAsker> m_holding;     // a HoldingAsker over m_rawAsker
    IAsker* m_asker = nullptr;             // every question goes through it (= m_holding)
    Options m_options;
    EditorModel m_editor;
    Snapshot m_snapshot;
    ServiceView m_service;
    std::optional<Submission> m_inFlight;
    QString m_operation;          // what is running ("Save & apply", "Start", ...)
    bool m_closeApproved = false;
    bool m_inCloseEvent = false;
    bool m_busyShown = false;
    // Discard saved changes was confirmed with unsaved edits at this revision (the user agreed to lose them).
    std::optional<quint64> m_discardConsentRevision;
    bool m_autoStartKnown = true;     // the real start type was read (or there is no service)
    bool m_detailsExpanded = true;    // as built; build() then applies the initial state
    bool m_autoExpanded = false;      // opened by an edit action, not by the user's Details button
    int m_expandedHeight = 0;         // the window's height when the card was last open
    bool m_firewallChecking = false;   // the current episode's check is outstanding
    bool m_firewallChecked = false;    // checked in the current "installed, activated, not running" episode
    quint64 m_firewallEpisode = 0;     // a check's callback counts only while this is the id it was started under
    QString m_firewallNote;

    PortTableModel* m_model = nullptr;
    QTableView* m_table = nullptr;
    PortCard* m_card = nullptr;
    QLabel* m_banner = nullptr;
    QLabel* m_pendingNote = nullptr;
    QPushButton* m_discardPending = nullptr;
    QLabel* m_serviceDot = nullptr;
    QLabel* m_serviceText = nullptr;
    QPushButton* m_start = nullptr;
    QPushButton* m_stop = nullptr;
    QPushButton* m_showLog = nullptr;
    QPushButton* m_add = nullptr;
    QPushButton* m_remove = nullptr;
    QCheckBox* m_autoStart = nullptr;
    QPushButton* m_reload = nullptr;
    QPushButton* m_save = nullptr;
    QLabel* m_footer = nullptr;
    QPushButton* m_repair = nullptr;
    QPushButton* m_lifecycle = nullptr;      // Install the bridge service / Finish installing / Update to this build
    QPushButton* m_uninstall = nullptr;      // Uninstall…
    QPushButton* m_restartAdmin = nullptr;   // Restart as administrator (Windows, standard user)
    QPushButton* m_waitCancel = nullptr;     // Cancel while waiting for the firewall check
    QPushButton* m_copyCommand = nullptr;    // Copy the terminal command a refused Linux action named (INV-124, WS-17)
    QString m_terminalCommand;
    QPlainTextEdit* m_log = nullptr;
    QFrame* m_cardHost = nullptr;
    QPushButton* m_details = nullptr;
    QLabel* m_detailsMarker = nullptr;
    QTimer* m_timer = nullptr;
    QTimer* m_logTimer = nullptr;
    QTimer* m_progressTimer = nullptr;
    QElapsedTimer m_operationClock;   // since the operation was submitted
    // An operation waiting for the window's own firewall check (INV-129).
    bool m_waiting = false;
    quint64 m_waitEpoch = 0;          // a whenIdle callback counts only for the wait it was registered for
    std::function<bool(IOperationObserver&)> m_deferredOp;
    QString m_deferredWhat;
    Basis m_deferredBasis;            // read again before the deferred operation starts
    int m_deferHolds = 0;             // questions open now (nested ones count)
    std::optional<quint64> m_idleWhileHeld;   // the wait whose check ended while a question was open
    QElapsedTimer m_waitClock;
    bool m_intentDone = false;        // the --intent confirmation was opened (once)
};

}  // namespace asb::gui
