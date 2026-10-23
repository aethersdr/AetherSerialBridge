// The bridge window (WS-10; INV-13, 23, 33, 35, 69, 94, 107 owned; INV-118 and INV-102 integration rows; INV-29's
// window row). Offscreen. The window runs the real Applier on a real OperationHost over the WS-5 fakes (Rig): the
// operations, their deadlines (a fake clock) and the files on "disk" are the transaction core's own. The window's
// view reads the same fakes, only while no operation runs (the product gives the view its own Platform).

#include "core/AppSettings.h"
#include "core/ConfigJson.h"
#include "core/ConfigFiles.h"
#include "core/AllowedClients.h"
#include "core/FirewallKeywords.h"
#include "core/HelpText.h"
#include "core/OperationHost.h"
#include "core/ThemeManager.h"
#include "gui/Actions.h"
#include "gui/BridgeStyle.h"
#include "gui/BindAddress.h"
#include "gui/BridgeWindow.h"
#include "gui/CanonBody.h"
#include "gui/CanonIndicators.h"
#include "gui/CanonWindow.h"
#include "gui/Dialogs.h"
#include "gui/EditorModel.h"
#include "gui/FirewallChecks.h"
#include "gui/PortCard.h"
#include "gui/PortTableModel.h"
#include "gui/ViewSource.h"
#include "gui/WindowSetup.h"
#include "support/DemoFixture.h"
#include "support/Fakes.h"

#ifndef _WIN32
#include "cli/Helper.h"
#include "cli/HelperClient.h"
#include "gui/HelperActions.h"
#include "support/FakeCliHost.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QListWidget>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStyleOptionButton>
#include <QTableView>
#include <QTest>
#include <QAbstractButton>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QGroupBox>
#include <QTextDocumentFragment>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>

using namespace asb;
using namespace asb::gui;
using namespace asb::test;

namespace {

// Answers by question id; records what was asked.
struct ScriptedAsker final : IAsker {
    QMap<QString, bool> answers;     // missing id: false (cancel)
    QStringList asked;               // ids, in order
    QStringList texts;
    QStringList notices;
    // Runs when a question opens, before its answer (the world changing while the user reads it).
    std::function<void(const QString& id)> onQuestion;
    bool confirm(const QString& id, const QString& text, const QString&, const QString&) override
    {
        asked << id;
        texts << text;
        if (onQuestion) onQuestion(id);
        return answers.value(id, false);
    }
    void notify(const QString& id, const QString& text) override
    {
        notices << id;
        texts << text;
    }
    // The lifecycle questions (WS-17): `choices` by id ("keep", "update", "cancel", ...); else answers[id] true picks
    // the last (affirmative) choice; else Cancelled. `tick` ticks a question's checkbox.
    QMap<QString, QString> choices;
    bool tick = false;
    QList<Question> questions;
    Answer ask(const Question& q) override
    {
        asked << q.id;
        texts << q.text + QLatin1Char('\n') + q.list.join(QLatin1Char('\n')) + QLatin1Char('\n') + q.after;
        questions << q;
        if (onQuestion) onQuestion(q.id);
        Answer a;
        if (choices.contains(q.id)) a.choice = choices.value(q.id) == QLatin1String("cancel") ? QString() : choices.value(q.id);
        else if (answers.value(q.id, false) && !q.choices.isEmpty()) a.choice = q.choices.last().id;
        a.checked = tick && !q.checkbox.isEmpty();
        return a;
    }
};

// A native call that does not return until the test opens it (tst_OperationHost's pattern).
struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool entered = false, open = false;
    void enter()
    {
        std::unique_lock lk(m);
        entered = true;
        cv.notify_all();
        cv.wait(lk, [&] { return open; });
    }
    // Bounded: an operation that never reaches the blocked call fails the test instead of hanging it.
    [[nodiscard]] bool waitEntered()
    {
        std::unique_lock lk(m);
        return cv.wait_for(lk, std::chrono::seconds(20), [&] { return entered; });
    }
    void release()
    {
        std::lock_guard g(m);
        open = true;
        cv.notify_all();
    }
};

// The docs images hold the logo's spark still at its starting angle: on a platform that exposes the window (Windows'
// native one) it would otherwise tick with the wall clock, and the same state would render differently each run.
// Called before show(), so the timer never starts (SparkWidget::showEvent).
void holdSparksStill(QWidget* w)
{
    for (auto* s : w->findChildren<AetherSDR::SparkWidget*>()) s->setMotionPreference(Qt::MotionPreference::ReducedMotion);
}

// The window, its in-process Applier and its view over one fake world.
struct Harness {
    explicit Harness(Installation::Flavor f = Installation::Flavor::Windows) : flavor(f)
    {
        rig.world.checkLock = false;   // the view's reads are made without the apply lock, by design
        rig.service.installation.flavor = f;
        // configA listens on 0.0.0.0 with an allow-list: on Linux the list is not enforced, so it is exposed there.
        if (f == Installation::Flavor::Linux) asker.answers[QStringLiteral("exposure")] = true;
    }
    ~Harness()
    {
        if (gate) gate->release();
        window.reset();
    }

    void open()
    {
        host = std::make_unique<OperationHost>(*rig.platform, rig.world.clock, std::chrono::milliseconds(5));
        if (readOnly) actions = std::make_unique<ReadOnlyActions>(flavor, standardUserReason());   // a standard user
        else actions = std::make_unique<InProcessActions>(*host, flavor);
        PlatformViewSource::Hooks hooks;
        hooks.autoStart = [this]() -> std::optional<bool> {
            if (autoStartUnreadable) return std::nullopt;
            if (rig.service.installation.state == InstallState::NotInstalled) return false;
            return !rig.service.startsBlocked;
        };
        hooks.serialPorts = [this] { return serialPorts; };
        hooks.dataFolder = dataFolder;
        view = std::make_unique<PlatformViewSource>(*rig.platform, std::move(hooks), rig.world.clock);
        BridgeWindow::Options o;
        o.refreshMs = 0;
        o.linuxWords = flavor == Installation::Flavor::Linux;
        o.localAddresses = [this] { return localAddrs; };
        o.onFinished = [this](const Result& r) {
            finals << r;
            if (alsoOnFinished) alsoOnFinished(r);   // inside finished(), when the result is delivered
        };
        o.firewallCheck = firewallCheck;
        o.firewallIdle = firewallIdle;
        o.restartAsAdmin = restartAsAdmin;
        o.intent = intent;
        o.detailsExpanded = detailsExpanded;
        window = std::make_unique<BridgeWindow>(*view, *actions, realDialogs ? nullptr : &asker, o);
        if (stillSparks) holdSparksStill(window.get());
        window->show();
    }

    // Waits for the outstanding operation's final result to be delivered.
    bool waitIdle(int ms = 20000)
    {
        return QTest::qWaitFor([&] { return !window->busy(); }, ms);
    }

    // The editor's name field of port `row`, edited as the user types.
    void rename(int row, const QString& name)
    {
        window->table()->selectRow(row);
        auto* field = window->findChild<QLineEdit*>(QStringLiteral("asbCardName"));
        QVERIFY(field);
        field->selectAll();
        QTest::keyClicks(field, name);
    }

    void blockAt(const QString& method, std::chrono::seconds advance)
    {
        gate = std::make_unique<Gate>();
        rig.service.onCall = [this, method, advance](const QString& m) {
            if (m != method || !gate) return;
            rig.world.clock.advance(advance);
            Gate* g = gate.get();
            g->enter();
        };
    }

    QStringList mutations() const
    {
        // Every event the fakes logged that changes something (the view's reads are detect/query/inspect/read).
        QStringList out;
        for (const QString& e : rig.world.log.all()) {
            if (e.startsWith(QLatin1String("service:detect")) || e.startsWith(QLatin1String("service:query")) ||
                e.startsWith(QLatin1String("folder:inspect")) || e.startsWith(QLatin1String("folder:read")) ||
                e.startsWith(QLatin1String("io:openExisting")))
                continue;
            out << e;
        }
        return out;
    }

    Installation::Flavor flavor;
    Rig rig;
    ScriptedAsker asker;
    std::unique_ptr<OperationHost> host;
    std::unique_ptr<IActions> actions;
    std::unique_ptr<PlatformViewSource> view;
    std::unique_ptr<BridgeWindow> window;
    std::unique_ptr<Gate> gate;
    QList<Result> finals;
    BridgeWindowOptions::FirewallCheck firewallCheck;
    std::function<void(const Result&)> alsoOnFinished;
    std::function<bool(std::function<void()>)> firewallIdle;
    std::function<BridgeWindowOptions::Relaunch(const QString&)> restartAsAdmin;
    QString intent;
    bool realDialogs = false;   // the product's CanonAsker (Esc, the close button, Return on the real dialog)
    bool stillSparks = false;   // the docs images: the logo's spark held at its starting angle (holdSparksStill)
    bool readOnly = false;      // a standard user's window (ReadOnlyActions)
    bool autoStartUnreadable = false;
    std::optional<bool> detailsExpanded = true;   // most tests edit in the card; the details tests use the product rule
    // The machine the view sees (the docs images replace them with the demo fixture's, WS-19).
    QStringList serialPorts{QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5")};
    QStringList localAddrs{QStringLiteral("192.168.1.20"), QStringLiteral("127.0.0.1"), QStringLiteral("0.0.0.0")};
    QString dataFolder = QStringLiteral("C:/ProgramData/AetherSerialBridge");
};

// configA with port 1's listen address and allowed clients replaced.
QByteArray configWithBind(const QString& bind, const QString& allowed = {})
{
    BridgeConfig c = *fromJson(configA(), nullptr);
    c.ports[0].bindAddress = bind;
    c.ports[0].allowedClients = allowed;
    return toJson(c);
}

// configA with three ports (rows 0..2), for the tests that remove the selected row under the card.
QByteArray configThreePorts()
{
    BridgeConfig c = *fromJson(configA(), nullptr);
    c.ports[0].name = QStringLiteral("One");
    PortMapping two = c.ports[0];
    two.name = QStringLiteral("Two");
    two.comPort = QStringLiteral("COM4");
    two.tcpPort = 4002;
    PortMapping three = c.ports[0];
    three.name = QStringLiteral("Three");
    three.comPort = QStringLiteral("COM5");
    three.tcpPort = 4003;
    c.ports << two << three;
    return toJson(c);
}

// configA with no ports at all.
QByteArray configNoPorts()
{
    BridgeConfig c = *fromJson(configA(), nullptr);
    c.ports.clear();
    return toJson(c);
}

// A firewall check whose verdicts the test delivers, one by one, when it chooses (a slow WMI query).
struct HeldChecks {
    QList<std::function<void(IFirewallMonitor::Check, const QString&)>> done;
    BridgeWindowOptions::FirewallCheck hook()
    {
        return [this](const BridgeConfig&, const FirewallGeneration&, std::function<void(IFirewallMonitor::Check, const QString&)> d) {
            done << std::move(d);
            return true;
        };
    }
};

QByteArray committed(const Rig& r) { return r.file(QString::fromLatin1(ConfigFiles::kConfig)); }
QByteArray pending(const Rig& r) { return r.file(QString::fromLatin1(ConfigFiles::kPending)); }

// A colour within `tol` per channel (painted fills, no antialiasing at the sampled points).
bool near(const QColor& a, const QColor& b, int tol = 3)
{
    return qAbs(a.red() - b.red()) <= tol && qAbs(a.green() - b.green()) <= tol && qAbs(a.blue() - b.blue()) <= tol &&
           qAbs(a.alpha() - b.alpha()) <= tol;
}

QString describe(const QColor& c) { return c.name(QColor::HexArgb); }

// The window painted into a transparent image: no window background is drawn, so the corners outside the rounded
// rectangle stay alpha 0 unless something paints them.
QImage renderWindow(QWidget& w)
{
    QImage img(w.size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    w.render(&img, QPoint(), QRegion(), QWidget::DrawChildren);
    return img;
}

QColor pixelIn(const QImage& img, QWidget* window, QWidget* child, QPoint offsetInChild)
{
    const QPoint p = child->mapTo(window, offsetInChild);
    return img.pixelColor(p);
}


#ifndef _WIN32
// WS-9b's helper, run in this process over a FakeCliHost on the window's Rig (tst_HelperRoundTrip's in-process mode):
// the Linux window's route end to end, minus pkexec. The exit can be replaced (pkexec's own 126), the response
// rewritten, or the ending held back (a slow helper).
class InProcessHelper final : public cli::IHelperProcess {
public:
    explicit InProcessHelper(test::FakeCliHost& h) : m_h(h) {}
    std::optional<Exit> replaceWith;
    std::function<QByteArray(const QByteArray&)> rewriteOut;
    bool hold = false;
    int runs = 0;

    // The staged route (v1.1 WS-15) runs the same in-process helper here: the window's routing is tst_HelperActions'.
    void runStaged(const QByteArray&, const QByteArray&, const QByteArray& request, std::function<void(const Exit&)> done) override
    {
        ++stagedRuns;
        // The helper runs as the staged copy (INV-123: install and uninstall only there).
        const QString own = m_h.own;
        m_h.own = QStringLiteral("/usr/local/lib/aether-serial-bridge/staging.Test1234/aether-serial-bridge");
        run(request, std::move(done));
        m_h.own = own;
    }
    int stagedRuns = 0;

    void run(const QByteArray& request, std::function<void(const Exit&)> done) override
    {
        ++runs;
        Exit e;
        if (replaceWith) {
            e = *replaceWith;
        } else {
            const QString in = m_dir.filePath(QStringLiteral("in")), out = m_dir.filePath(QStringLiteral("out"));
            QFile f(in);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(request) != request.size()) qFatal("write");
            f.close();
            cli::HelperOptions o;
            o.inputFd = ::open(QFile::encodeName(in).constData(), O_RDONLY | O_CLOEXEC);
            o.outputFd = ::open(QFile::encodeName(out).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            o.syslog = false;
            test::CapturingConsole c;
            e.code = cli::runHelper(m_h, c, o);
            ::close(o.inputFd);
            ::close(o.outputFd);
            QFile r(out);
            if (r.open(QIODevice::ReadOnly)) e.out = r.readAll();
            e.err = c.errText().toUtf8();
            if (rewriteOut) e.out = rewriteOut(e.out);
        }
        if (hold) {
            m_held = std::move(done);
            m_heldExit = e;
            return;
        }
        QTimer::singleShot(0, [done = std::move(done), e] { done(e); });   // later, on this thread (the contract)
    }
    void release()
    {
        auto d = std::move(m_held);
        m_held = nullptr;
        if (d) d(m_heldExit);
    }

private:
    test::FakeCliHost& m_h;
    QTemporaryDir m_dir;
    std::function<void(const Exit&)> m_held;
    Exit m_heldExit;
};

// The Linux window: HelperActions over WS-9b's HelperClient over the in-process helper, the view on the same Rig.
// The window's own program for the staged route: these bytes, and the digest compiled into the window.
HelperActions::Staged stagedProgram(QString problem = {}, QString appImage = QStringLiteral("/tmp/AetherSerialBridge.AppImage"))
{
    HelperActions::Staged st;
    const QByteArray bytes = QByteArrayLiteral("the window's own program");
    st.expectedSha256 = sha256Hex(bytes).toLatin1();
    st.read = [bytes] {
        HelperActions::ProgramFile f;
        f.path = QStringLiteral("/tmp/.mount_asb/usr/libexec/aether-serial-bridge");
        f.bytes = bytes;
        return f;
    };
    st.appImage = appImage;
    if (!problem.isNull()) st.probe = [problem] { return problem; };
    return st;
}

struct HelperHarness {
    explicit HelperHarness(std::chrono::milliseconds waitDeadline = cli::HelperClient::kWaitDeadline,
                           HelperActions::Staged staged = {})
        : client(proc, waitDeadline), actions(client, std::move(staged))
    {
        rig.world.checkLock = false;
        rig.service.installation.flavor = Installation::Flavor::Linux;
        asker.answers[QStringLiteral("exposure")] = true;   // configA is exposed on Linux (allow-list not enforced)
    }
    void open()
    {
        PlatformViewSource::Hooks hooks;
        hooks.autoStart = [this]() -> std::optional<bool> {
            if (rig.service.installation.state == InstallState::NotInstalled) return false;
            return !rig.service.startsBlocked;
        };
        hooks.journalLog = true;
        if (serialPorts) hooks.serialPorts = [this] { return *serialPorts; };
        if (!dataFolder.isEmpty()) hooks.dataFolder = dataFolder;
        view = std::make_unique<PlatformViewSource>(*rig.platform, std::move(hooks), rig.world.clock);
        BridgeWindow::Options o;
        o.detailsExpanded = true;
        o.refreshMs = 0;
        o.linuxWords = true;
        o.localAddresses = [this] { return localAddrs; };
        o.onFinished = [this](const Result& r) { finals << r; };
        window = std::make_unique<BridgeWindow>(*view, actions, realDialogs ? nullptr : &asker, o);
        if (stillSparks) holdSparksStill(window.get());
        window->show();
    }
    bool waitIdle(int ms = 20000) { return QTest::qWaitFor([&] { return !window->busy(); }, ms); }
    void rename(const QString& name)
    {
        window->table()->selectRow(0);
        auto* field = window->findChild<QLineEdit*>(QStringLiteral("asbCardName"));
        field->selectAll();
        QTest::keyClicks(field, name);
    }

    Rig rig;
    test::FakeCliHost cli{rig};
    InProcessHelper proc{cli};
    cli::HelperClient client;
    HelperActions actions;
    ScriptedAsker asker;
    std::unique_ptr<PlatformViewSource> view;
    std::unique_ptr<BridgeWindow> window;
    QList<Result> finals;
    std::optional<QStringList> serialPorts;   // the device list (the docs images' demo devices, WS-19)
    QString dataFolder;                       // the view's data folder hook (the docs images', WS-19)
    QStringList localAddrs{QStringLiteral("127.0.0.1"), QStringLiteral("0.0.0.0")};
    bool realDialogs = false;                 // the product's CanonAsker
    bool stillSparks = false;   // the docs images: the logo's spark held at its starting angle (holdSparksStill)
};
#endif

}  // namespace

class tst_BridgeWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    // First (user report 2026-10-16): a lock an operation leaked would stall every later test that runs a second
    // operation on the same fakes, so the check that names it runs before them.
    void nothingHeldWhenFinishedFires_data();
    void nothingHeldWhenFinishedFires();

    // The application icon (user report 2026-10-21: the window had none).
    void applicationIcon();
    // INV-69
    void composedRendering();
    // INV-33
    void untrustedFolderFooter();
    // INV-35
    void cancelChangesNothing();
    // INV-94
    void untickedSaveReloadShowsEdits_data();
    void untickedSaveReloadShowsEdits();
    void untickedSavePendingWriteFailsNothingChanged();
    // INV-107
    void barrierDeadlineKeepsDirty();
    void stagedClearsDirty();
    void digestMismatchKeepsDirty();
    // INV-13
    void modelChangeDuringSaveStaysDirty();
    void escapeAndCtrlWAsk();
    void closeRefusedWhileBusy();
    // INV-23
    void autoStartIsRealStartType();
    void autoStartRefreshedAfterStart();
    void autoStartRefreshedAfterFailedApply();
    void autoStartNotRefreshedWhenDirty();
    void unknownStartTypeKeepsTheService();
    void refreshReadsTheStateOnly();
    // INV-118 (WS-5 integration rows)
    void preR4TimeoutKeepsEditorAndDirtyN();
    void secondSaveAfterEditDuringInterim();
    // INV-102 (WS-5 window rows)
    void saveThenReloadShowsCommittedNoPending();
    void cliEditAfterLoadThenSaveAsksToReplace();
    // INV-29 (WS-5 window row)
    void startGoesThroughApplier();
    // WS-9 deferred gate: "stale firewall rule detected" derived with the comparator (the GUI half)
    void staleFirewallRuleShownWhileNotRunning();
    void firewallNotCheckedWhileRunning();
    // The Linux route (WS-9b's deferred GUI gates): HelperClient over the helper, INV-107 / D57 on that route
    void linuxRouteStagedClearsDirty();
    void linuxRouteDigestMismatchKeepsDirty();
    void linuxRouteBarrierDeadlineKeepsDirty();
    void linuxRouteWaitingBannerAndCloseRefused();
    void linuxRouteCancelledChangesNothing();
    void linuxRouteUntickDeactivates();
    // Pre-emptive pass on WS-12's review classes (#18)
    void bindsEveryInterfaceByValue_data();
    void bindsEveryInterfaceByValue();
    void exposureAskedForEveryWildcardSpelling_data();
    void exposureAskedForEveryWildcardSpelling();
    void linuxExposureIgnoresTheAllowList();
    void discardWithUnsavedEditsSaysSoAndShowsApplied();
    void editsMadeDuringDiscardAreKept();
    void resultDuringAnOpenDialogKeepsTyping();
    void unacceptedCardEditOnACleanConfigAsks_data();
    void unacceptedCardEditOnACleanConfigAsks();
    void refreshNeverTouchesTheCard();
    void linuxCancelWhileTypingKeepsTheEdit();
    void dirtyRuleMatchesTheTuiRule_data();
    void dirtyRuleMatchesTheTuiRule();
    // Review round 1
    void installationUnreadableNeverRemovesTheService_data();
    void installationUnreadableNeverRemovesTheService();
    void stateUnreadableAtLoadKeepsTheService();
    void diagnosticsFollowACommitWithNewerEdits();
    void cardCursorSurvivesRefresh_data();
    void cardCursorSurvivesRefresh();
    void tableToggleKeepsTheCardCursor();
    void deviceKeepsTheTypedCase();
    void tcpPortZeroStaysAsTyped();
    void openDeviceListSurvivesRefreshAndTyping();
    // The details card (owner request)
    void detailsCollapsedOnFirstRun();
    void detailsStatePersists();
    void detailsAutoExpandOnEditAndAdd();
    void detailsNoAutoCollapseWhileDirty();
    void detailsNoAutoCollapseWhileBusy();
    void detailsMarkerWhenCollapsedAndDirty();
    void detailsHeightShrinksAndRestores();
    // User report 2026-10-16 (first use on a station): no silent program replacement; nothing held after an
    // operation; a stopped bridge says why
    void otherBuildAskedBeforeReplacing_data();
    void otherBuildAskedBeforeReplacing();
    void unfinishedInstallFinishedWithoutAsking();
    void startRunsAnInstalledOtherBuild();
    void ackTimeoutNamesWhyTheServiceStopped();
    void serviceExitDescriptions_data();
    void serviceExitDescriptions();
    void progressShownWhileBusy();
    void firewallNotCheckedWhileStarting();
    // The window's own check and a kept other build's diagnosis
    void operationWaitsForTheWindowsFirewallCheck_data();
    void operationWaitsForTheWindowsFirewallCheck();
    void firewallChecksAreOwnedBoundedAndWaitable();
    void staleRuleShownForAKeptOtherBuild();
    // The window's own check, round 2
    void startEnabledForAKeptOtherBuild();
    // Review round 3 follow-up: the card's row across a replaced list; firewall verdicts per episode
    void cardRowClampedWhenReloadRemovesTheSelectedRow();
    void cardRowClampedWhenDiscardRemovesTheSelectedRow();
    void portCardShowsNoRowPastTheList();
    // Owner request 2026-10-19: what Allowed means, with an example, visible on the card
    void allowedHintPerPlatform_data();
    void allowedHintPerPlatform();
    void obsoleteFirewallVerdictDroppedAcrossStartAndStop_data();
    void obsoleteFirewallVerdictDroppedAcrossStartAndStop();
    void obsoleteFirewallVerdictDroppedAcrossReload();
    void obsoleteFirewallVerdictDroppedAfterAnOperationWithNewerEdits();
    void obsoleteFirewallVerdictDroppedWhenTheBridgeRanMeanwhile();
    // Flows
    void savedAndAppliedClearsDirtyAndShowsOutcome();
    void readOnlyWindowEditsNothing();
    void pendingLoadedAsCleanWorkingCopy();
    void discardPendingAsksAndGoesThroughTheGate();
    void discardChecksDigest();
    void repairAsksYesNoThenRepairs();
    // v1.1 WS-17: the window's lifecycle (INV-128, 129, 131; D67)
    void notInstalledShowsInstall();
    void installRunsEditorPorts();
    void installAsksExposureFirst();
    void installCancelChangesNothing();
    void otherBuildShowsUpdate();
    void updateButtonRunsInstall();
    void buildQuestionThreeWay_data();
    void buildQuestionThreeWay();
    void uninstallDialogNamesPortsAndClients();
    void uninstallSettingsUncheckedEachTime();
    void uninstallCancelChangesNothing();
    void uninstallNewerBuildWarns();
    void uninstallKeepsDirtyEditor();
    void uninstallAgainFinishesAKeptProgram_data();
    void uninstallAgainFinishesAKeptProgram();
    void noUninstallOnACleanMachine();
    void afterUninstallShowsInstall();
    void restartAsAdminAsksWhenDirty();
    void restartAsAdminCancelled();
    void intentOpensConfirmationOnly();
    void actionDeferredWhileCheckRuns();
    void deferredActionCancelStartsNothing();
    void closeWhileWaitingStartsNothing();
    void escWhileWaitingKeepsTheSave_data();
    void escWhileWaitingKeepsTheSave();
    void deferredOperationOnChangedEvidenceStartsNothing_data();
    void deferredOperationOnChangedEvidenceStartsNothing();
    void checkEndsWhileTheCloseQuestionIsOpen_data();
    void checkEndsWhileTheCloseQuestionIsOpen();
    void exposureDialogBoundedWith40Ports();
    void lifecycleCancelChangesNothing_data();
    void lifecycleCancelChangesNothing();
    void readOnlyLifecycleRefused();
    // v1.1 fix after WS-17: the main button's label names the route its click takes.
    void mainButtonOnNothingInstalled_data();
    void mainButtonOnNothingInstalled();
    void mainButtonStandardUserIntent();
    void mainButtonRefusesWhenTheInstallationChanged();
    // Save stays save-only under the apply lock; Install and apply after an install elsewhere
    // shows that installation.
    void saveOnlyRefusedWhenInstalledDuringTheQuestion();
    void installAfterInstalledElsewhereShowsIt_data();
    void installAfterInstalledElsewhereShowsIt();
    // Another build installed while the install question is open is refused under the lock.
    void anotherBuildDuringTheInstallQuestionIsRefused_data();
    void anotherBuildDuringTheInstallQuestionIsRefused();
    void mainButtonLabelNamesTheRoute_data();
    void mainButtonLabelNamesTheRoute();
    // Linux only (moc reads no _WIN32: declared on both, skipped on Windows).
    void linuxNoAgentShowsTerminalCommand();
    void linuxProgramMismatchDisablesStagedActions();
    void linuxBuildQuestionUpdateIsStaged_data();
    void linuxBuildQuestionUpdateIsStaged();
    void linuxMainButtonOnNothingInstalled_data();
    void linuxMainButtonOnNothingInstalled();
    // v1.1 WS-19 (INV-135): the documentation images, from the demo fixture (ASB_DOCS_SHOTS_DIR; tools/docs_shots.py)
    void docsShots();
};

void tst_BridgeWindow::initTestCase()
{
    // gui.ini, the geometry key and the painted-indicator cache go to Qt's test locations, never a real profile.
    QStandardPaths::setTestModeEnabled(true);
    registerInterFonts();
    (void)AetherSDR::ThemeManager::instance();
    // The product's three levels (§5.2): the application sheet too, so composedRendering sees what the window wears.
    applyApplicationSheet(*qobject_cast<QApplication*>(QCoreApplication::instance()));
}

void tst_BridgeWindow::init()
{
    // Every test starts in Default Dark.
    QFile f(QStringLiteral(":/themes/default-dark.json"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QVERIFY(AetherSDR::ThemeManager::instance().activateValidatedTheme(QStringLiteral("Default Dark"), f.readAll()));
}

// INV-69 / B6: the composed body sheet keeps controls and containers apart. Kills: applying canonBodyStyleSheet()
// (its ID-selector transparency makes the fields, buttons, combos and the table transparent).
// Every size the qrc carries is a frame of its own (a missing alias or addFile leaves the title bar or taskbar to a
// reduction of another frame), and the desktop entry Wayland finds the icon through is named.
void tst_BridgeWindow::applicationIcon()
{
    applyApplicationIcon(*qApp);
    const QIcon icon = QApplication::windowIcon();
    QList<QSize> want;
    for (const int size : {16, 32, 48, 64, 128, 256})
        want.append(QSize(size, size));
    QCOMPARE(icon.availableSizes(), want);
    for (const QSize& size : want) {
        const QImage frame(QStringLiteral(":/appicon/asb-icon-%1.png").arg(size.width()));
        QCOMPARE(frame.size(), size);
        QVERIFY(frame.hasAlphaChannel());
    }
    QCOMPARE(QGuiApplication::desktopFileName(), QStringLiteral("aether-serial-bridge"));
}

void tst_BridgeWindow::composedRendering()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    BridgeWindow& w = *h.window;
    w.resize(1040, 760);
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    auto check = [&](const QString& theme) {
        QApplication::processEvents();
        const QImage img = renderWindow(w);
        auto& tm = AetherSDR::ThemeManager::instance();

        // Corners transparent (outside the 16 px rounding).
        QCOMPARE(img.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(img.pixelColor(img.width() - 1, img.height() - 1).alpha(), 0);

        // Containers: the painted ground shows through (equal to the ground painted alone).
        QImage ground(w.size(), QImage::Format_ARGB32_Premultiplied);
        ground.fill(Qt::transparent);
        {
            QPainter p(&ground);
            AetherSDR::paintCanonGround(p, &w, w.size(), AetherSDR::CanonWindow::kRadius);
        }
        for (const char* name : {"asbServiceRow", "asbPortsPanel", "asbHeader"}) {
            QWidget* c = w.findChild<QWidget*>(QString::fromLatin1(name));
            QVERIFY2(c, name);
            // A point inside the container but clear of its children: the right end of its top edge strip.
            QPoint at(c->width() - 2, 1);
            if (QString::fromLatin1(name) == QLatin1String("asbHeader")) at = QPoint(c->width() - 2, c->height() - 1);
            const QPoint pw = c->mapTo(&w, at);
            QVERIFY2(near(img.pixelColor(pw), ground.pixelColor(pw)),
                     qPrintable(QStringLiteral("%1 %2: %3 vs ground %4").arg(theme, QString::fromLatin1(name),
                                                                         describe(img.pixelColor(pw)), describe(ground.pixelColor(pw)))));
        }

        const QColor background0 = tm.color(&w, QStringLiteral("color.background.0"));
        auto expectFill = [&](QWidget* control, QPoint at, const QColor& fill, const char* what) {
            const QColor got = pixelIn(img, &w, control, at);
            QVERIFY2(got.alpha() == 255, qPrintable(QStringLiteral("%1 %2 is transparent (%3)").arg(theme, QString::fromLatin1(what), describe(got))));
            QVERIFY2(near(got, fill), qPrintable(QStringLiteral("%1 %2: %3, expected %4").arg(theme, QString::fromLatin1(what), describe(got), describe(fill))));
            QVERIFY2(!near(got, background0, 0) || near(fill, background0, 0),
                     qPrintable(QStringLiteral("%1 %2 shows color.background.0").arg(theme, QString::fromLatin1(what))));
        };

        // A QLineEdit (ModemChrome canon field fill: color.canon.control), sampled right of any text.
        auto* edit = w.findChild<QLineEdit*>(QStringLiteral("asbCardAllowed"));
        QVERIFY(edit);
        expectFill(edit, QPoint(edit->width() - 8, edit->height() / 2), tm.color(edit, QStringLiteral("color.canon.control")), "QLineEdit");
        // A QPushButton (canon control), just inside its border at mid height.
        QPushButton* button = w.button(QStringLiteral("asbReload"));
        QVERIFY(button);
        expectFill(button, QPoint(3, button->height() / 2), tm.color(button, QStringLiteral("color.canon.control")), "QPushButton");
        // A QComboBox (applyComboStyle: color.background.1 in the window's scope).
        auto* combo = w.findChild<QComboBox*>(QStringLiteral("asbCardFormat"));
        QVERIFY(combo);
        expectFill(combo, QPoint(3, combo->height() / 2), tm.color(combo, QStringLiteral("color.background.1")), "QComboBox");
        // The table viewport below its rows (bridgeTableRules: color.canon.control).
        QTableView* table = w.table();
        QWidget* viewport = table->viewport();
        expectFill(viewport, QPoint(viewport->width() / 2, viewport->height() - 3), tm.color(table, QStringLiteral("color.canon.control")),
                   "table viewport");
        // The table header (color.canon.raised).
        QHeaderView* header = table->horizontalHeader();
        expectFill(header, QPoint(header->width() - 4, 3), tm.color(table, QStringLiteral("color.canon.raised")), "table header");

        // The painted canon indicator on the auto-start box (unchecked: canon control fill inside a lineHi edge).
        QVERIFY(!AetherSDR::canonIndicatorRules().isEmpty());
        QCheckBox* box = w.autoStartBox();
        QStyleOptionButton opt;
        opt.initFrom(box);
        const QRect ind = box->style()->subElementRect(QStyle::SE_CheckBoxIndicator, &opt, box);
        QVERIFY(ind.isValid());
        const QColor centre = pixelIn(img, &w, box, ind.center());
        const QColor expected = box->isChecked() ? tm.color(box, QStringLiteral("color.canon.cyan"))
                                                 : tm.color(box, QStringLiteral("color.canon.control"));
        QVERIFY2(near(centre, expected, 8) || box->isChecked(),
                 qPrintable(QStringLiteral("%1 indicator centre %2, expected %3").arg(theme, describe(centre), describe(expected))));
    };

    check(QStringLiteral("Default Dark"));
    if (QTest::currentTestFailed()) return;

    // A live switch to Default Light: the sheets are re-resolved and the indicators rebuilt.
    QFile light(QStringLiteral(":/themes/default-light.json"));
    QVERIFY(light.open(QIODevice::ReadOnly));
    QVERIFY(AetherSDR::ThemeManager::instance().activateValidatedTheme(QStringLiteral("Default Light"), light.readAll()));
    QApplication::processEvents();
    check(QStringLiteral("Default Light (live)"));
    if (QTest::currentTestFailed()) return;

    // And back (live): nothing of Light survives.
    QFile dark(QStringLiteral(":/themes/default-dark.json"));
    QVERIFY(dark.open(QIODevice::ReadOnly));
    QVERIFY(AetherSDR::ThemeManager::instance().activateValidatedTheme(QStringLiteral("Default Dark"), dark.readAll()));
    check(QStringLiteral("Default Dark (live)"));
}

// INV-33: an untrusted folder is not read; the footer says so and offers Repair.
void tst_BridgeWindow::untrustedFolderFooter()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.folder.owner = QStringLiteral("attacker");
    h.rig.folder.reads.clear();
    h.open();
    QVERIFY2(h.window->footerText().startsWith(QStringLiteral("Config not loaded: the data folder isn't trusted.")),
             qPrintable(h.window->footerText()));
    QVERIFY(h.window->button(QStringLiteral("asbRepair"))->isVisibleTo(h.window.get()));
    QVERIFY(!h.window->button(QStringLiteral("asbSaveApply"))->isEnabled());
    QCOMPARE(h.window->tableModel()->rowCount(), 0);
    QVERIFY(!h.rig.folder.reads.contains(QStringLiteral("config.json")));
    QVERIFY(!h.rig.folder.reads.contains(QStringLiteral("config.pending.json")));
    QCOMPARE(h.rig.folder.mutations(), 0);
}

// INV-35: cancelling the exposure or the untick question writes nothing and calls no service.
void tst_BridgeWindow::cancelChangesNothing()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    const QByteArray before = committed(h.rig);
    // Expose port 1 to everyone: every interface, no client restriction.
    h.window->table()->selectRow(0);
    auto* allowed = h.window->findChild<QLineEdit*>(QStringLiteral("asbCardAllowed"));
    QVERIFY(allowed);
    allowed->selectAll();
    QTest::keyClick(allowed, Qt::Key_Backspace);
    QVERIFY(h.window->editor().dirty());
    QCOMPARE(h.window->editor().config().ports[0].allowedClients, QString());
    h.rig.world.log.clear();

    h.asker.answers[QStringLiteral("exposure")] = false;
    h.window->saveAndApply();
    QCOMPARE(h.asker.asked, QStringList{QStringLiteral("exposure")});
    QVERIFY(!h.window->busy());
    QCOMPARE(h.mutations(), QStringList());
    QCOMPARE(committed(h.rig), before);
    QVERIFY(!h.rig.has(QString::fromLatin1(ConfigFiles::kPending)));
    QVERIFY(h.window->editor().dirty());

    // Exposure accepted, untick refused.
    h.asker.asked.clear();
    h.asker.answers[QStringLiteral("exposure")] = true;
    h.asker.answers[QStringLiteral("untick")] = false;
    h.window->autoStartBox()->setChecked(false);
    h.window->saveAndApply();
    QCOMPARE(h.asker.asked, (QStringList{QStringLiteral("exposure"), QStringLiteral("untick")}));
    QVERIFY(!h.window->busy());
    QCOMPARE(h.mutations(), QStringList());
    QCOMPARE(committed(h.rig), before);
    QVERIFY(!h.rig.has(QString::fromLatin1(ConfigFiles::kPending)));
    QVERIFY(h.rig.service.engineRunning);
}

void tst_BridgeWindow::untickedSaveReloadShowsEdits_data()
{
    QTest::addColumn<int>("flavor");
    QTest::newRow("windows SaveAndRemove") << int(Installation::Flavor::Windows);
    QTest::newRow("linux Deactivate") << int(Installation::Flavor::Linux);
}

// INV-94: unticked Save & apply stages the edits (pending first) and says so; a reload shows them. Kills:
// Deactivate (or SaveAndRemove) ignoring N.
void tst_BridgeWindow::untickedSaveReloadShowsEdits()
{
    QFETCH(int, flavor);
    Harness h{Installation::Flavor(flavor)};
    h.rig.installedRunning(configA());
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    h.window->autoStartBox()->setChecked(false);
    QVERIFY(h.window->editor().dirty());
    const Submission expected = h.window->editor().submission(false);
    h.asker.answers[QStringLiteral("untick")] = true;
    h.window->saveAndApply();
    QCOMPARE(h.asker.asked.filter(QStringLiteral("untick")).size(), 1);
    QCOMPARE(h.asker.asked.contains(QStringLiteral("exposure")), Installation::Flavor(flavor) == Installation::Flavor::Linux);
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.size(), 1);
    QCOMPARE(h.finals[0].outcome, Outcome::SavedNotApplied);
    QCOMPARE(h.finals[0].persisted, Persisted::Staged);
    QVERIFY2(h.window->footerText().contains(QStringLiteral("saved but not applied")), qPrintable(h.window->footerText()));
    QVERIFY(!h.window->editor().dirty());
    // On disk: committed = A, pending = the submitted bytes.
    QCOMPARE(committed(h.rig), configA());
    QCOMPARE(pending(h.rig), expected.bytes);
    if (Installation::Flavor(flavor) == Installation::Flavor::Windows) {
        QCOMPARE(h.rig.service.installation.state, InstallState::NotInstalled);
        QVERIFY(h.rig.firewall.rules.isEmpty());
    } else {
        QCOMPARE(h.rig.service.installation.state, InstallState::Current);
        QVERIFY(h.rig.service.startsBlocked);
    }
    QVERIFY(!h.rig.service.engineRunning);

    // A reload from disk shows the edits: the pending file is the working copy, clean, with the note.
    h.window->reload(true);
    QVERIFY(h.window->editor().fromPending());
    QVERIFY(!h.window->editor().dirty());
    QCOMPARE(h.window->editor().config().ports[0].name, QStringLiteral("KAT500"));
    QVERIFY(h.window->pendingNoteText().contains(QStringLiteral("Saved, not applied")));
    if (Installation::Flavor(flavor) == Installation::Flavor::Windows) {
        // The service was removed: nothing is installed, so the box shows what an install sets and the main button
        // says it installs (v1.1 fix after WS-17).
        QVERIFY(h.window->autoStartBox()->isChecked());
        QCOMPARE(h.window->button(QStringLiteral("asbSaveApply"))->text(), QStringLiteral("Install and apply"));
    } else {
        QVERIFY(!h.window->autoStartBox()->isChecked());   // installed and disabled: the real start type
    }
}

// INV-94: a failed pending write changes nothing: dirty kept, the service untouched.
void tst_BridgeWindow::untickedSavePendingWriteFailsNothingChanged()
{
    Harness h(Installation::Flavor::Linux);
    h.rig.installedRunning(configA());
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    h.window->autoStartBox()->setChecked(false);
    h.asker.answers[QStringLiteral("untick")] = true;
    h.rig.world.files.failNext(FakeFileIo::Op::CreateTemp, QStringLiteral("disk full"));
    h.rig.world.log.clear();
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.size(), 1);
    QCOMPARE(h.finals[0].outcome, Outcome::NothingChanged);
    QCOMPARE(h.finals[0].persisted, Persisted::None);
    QVERIFY(h.window->editor().dirty());
    QCOMPARE(h.window->editor().config().ports[0].name, QStringLiteral("KAT500"));
    for (const QString& e : h.rig.world.log.all())
        QVERIFY2(!e.startsWith(QLatin1String("service:inhibitStarts")) && !e.startsWith(QLatin1String("service:requestStop")) &&
                     !e.startsWith(QLatin1String("service:uninstall")),
                 qPrintable(e));
    QVERIFY(h.rig.service.engineRunning);
    QCOMPARE(committed(h.rig), configA());
    QVERIFY(!h.rig.has(QString::fromLatin1(ConfigFiles::kPending)));
}

// INV-107: a barrier deadline before R4 (pre-commit StateUnknown, persisted None) keeps the edits dirty in the
// editor; a reload from disk shows the old committed config. Kills: clearing dirty on any StateUnknown.
void tst_BridgeWindow::barrierDeadlineKeepsDirty()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.service.neverReleases = true;   // the engine keeps engine.lock past the 30 s barrier
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.size(), 1);
    QCOMPARE(h.finals[0].outcome, Outcome::StateUnknown);
    QCOMPARE(h.finals[0].persisted, Persisted::None);
    QVERIFY(h.window->editor().dirty());
    QCOMPARE(h.window->editor().config().ports[0].name, QStringLiteral("KAT500"));
    QVERIFY2(h.window->footerText().contains(QStringLiteral("not saved")), qPrintable(h.window->footerText()));
    QCOMPARE(committed(h.rig), configA());
    h.asker.answers[QStringLiteral("discardEdits")] = true;
    h.window->reload(false);
    QCOMPARE(h.asker.asked.last(), QStringLiteral("discardEdits"));
    QCOMPARE(h.window->editor().config(), *fromJson(configA(), nullptr));
    QVERIFY(!h.window->editor().dirty());
}

// INV-107: a failure after R4 (Uncommitted, persisted Staged with the submitted digest) clears dirty, and the
// staged edits are what the window then shows.
void tst_BridgeWindow::stagedClearsDirty()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    const Submission s = h.window->editor().submission(true);
    h.rig.firewall.fail[QStringLiteral("sync")] = QStringLiteral("the provider refused the rule");
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.size(), 1);
    QCOMPARE(h.finals[0].outcome, Outcome::Uncommitted);
    QCOMPARE(h.finals[0].persisted, Persisted::Staged);
    QCOMPARE(h.finals[0].persistedSha256, s.sha256);
    QVERIFY(!h.window->editor().dirty());
    QVERIFY(h.window->editor().fromPending());
    QCOMPARE(pending(h.rig), s.bytes);
    QCOMPARE(h.window->editor().config().ports[0].name, QStringLiteral("KAT500"));
}

namespace {
// IActions whose operations "finish" with a scripted result, synchronously posted (digest cases).
class ScriptedActions final : public IActions {
public:
    Result result;
    std::optional<SaveRequest> submitted;
    bool isBusy = false;
    bool available() const override { return true; }
    Installation::Flavor flavor() const override { return fl; }
    bool busy() const override { return isBusy; }
    bool run(const SaveRequest& r, bool, IOperationObserver& o) override
    {
        routes << QStringLiteral("run");
        return finish(r, o);
    }
    bool untick(const SaveRequest& r, IOperationObserver& o) override
    {
        routes << QStringLiteral("untick");
        return finish(r, o);
    }
    bool savesWithoutService() const override { return canSaveOnly; }
    bool saveOnly(const SaveRequest& r, IOperationObserver& o) override
    {
        routes << QStringLiteral("saveOnly");
        return finish(r, o);
    }
    Installation::Flavor fl = Installation::Flavor::Windows;
    bool canSaveOnly = true;   // the in-process Applier; false models the Linux window's HelperActions
    QStringList routes;        // every save route taken (run, untick, saveOnly, install, update), in order
    bool start(IOperationObserver& o) override { return finish(std::nullopt, o); }
    bool stop(IOperationObserver& o) override { return finish(std::nullopt, o); }
    bool repair(IOperationObserver& o) override { return finish(std::nullopt, o); }
    bool discardPending(const QString& sha256, IOperationObserver& o) override
    {
        discardedDigest = sha256;
        return finish(std::nullopt, o);
    }
    QString discardedDigest;
    bool install(const std::optional<SaveRequest>& r, IOperationObserver& o) override
    {
        lifecycle << QStringLiteral("install");
        routes << QStringLiteral("install");
        return finish(r, o);
    }
    bool update(const std::optional<SaveRequest>& r, IOperationObserver& o) override
    {
        lifecycle << QStringLiteral("update");
        routes << QStringLiteral("update");
        return finish(r, o);
    }
    bool uninstall(UninstallOptions u, IOperationObserver& o) override
    {
        lifecycle << QStringLiteral("uninstall");
        uninstalled = u;
        return finish(std::nullopt, o);
    }
    QString lifecycleUnavailableReason() const override { return lifecycleWhy; }
    QStringList lifecycle;
    std::optional<UninstallOptions> uninstalled;
    QString lifecycleWhy;

private:
    bool finish(const std::optional<SaveRequest>& r, IOperationObserver& o)
    {
        submitted = r;
        isBusy = true;
        QMetaObject::invokeMethod(qApp, [this, &o] {
            isBusy = false;
            o.finished(result);
        }, Qt::QueuedConnection);
        return true;
    }
};
}  // namespace

// INV-107: persisted with a digest that is not the submitted revision's keeps dirty. Kills: clearing dirty without
// comparing persistedSha256.
void tst_BridgeWindow::digestMismatchKeepsDirty()
{
    Rig rig;
    rig.world.checkLock = false;
    rig.installedRunning(configA());
    PlatformViewSource::Hooks hooks;
    hooks.autoStart = [] { return std::optional<bool>(true); };
    PlatformViewSource view(*rig.platform, hooks, rig.world.clock);
    ScriptedActions actions;
    ScriptedAsker asker;
    BridgeWindow::Options o;
    o.detailsExpanded = true;
    o.refreshMs = 0;
    BridgeWindow w(view, actions, &asker, o);
    w.show();
    w.table()->selectRow(0);
    auto* field = w.findChild<QLineEdit*>(QStringLiteral("asbCardName"));
    field->selectAll();
    QTest::keyClicks(field, QStringLiteral("KAT500"));
    QVERIFY(w.editor().dirty());
    const Submission s = w.editor().submission(true);

    actions.result.outcome = Outcome::Ok;
    actions.result.persisted = Persisted::Committed;
    actions.result.persistedSha256 = QByteArray(64, 'a');   // not the submitted digest
    actions.result.message = QStringLiteral("Saved and applied.");
    w.saveAndApply();
    QVERIFY(actions.submitted.has_value());
    QCOMPARE(actions.submitted->config, s.bytes);
    QVERIFY(QTest::qWaitFor([&] { return !w.busy(); }));
    QApplication::processEvents();
    QVERIFY(w.editor().dirty());

    // The matching digest clears it.
    actions.result.persistedSha256 = s.sha256;
    w.saveAndApply();
    QVERIFY(QTest::qWaitFor([&] { return !w.busy(); }));
    QApplication::processEvents();
    QVERIFY(!w.editor().dirty());
}

// INV-13: an edit made while the save runs stays unsaved after the save succeeds.
void tst_BridgeWindow::modelChangeDuringSaveStaysDirty()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    h.blockAt(QStringLiteral("inhibitStarts"), std::chrono::seconds(0));
    h.window->saveAndApply();
    QVERIFY(h.window->busy());
    QVERIFY(h.gate->waitEntered());
    // Editing stays allowed while busy (D57).
    h.rename(0, QStringLiteral("KPA500"));
    QVERIFY(h.window->editor().dirty());
    h.gate->release();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.size(), 1);
    QCOMPARE(h.finals[0].outcome, Outcome::Ok);
    QVERIFY(h.window->editor().dirty());
    QCOMPARE(h.window->editor().config().ports[0].name, QStringLiteral("KPA500"));
    QVERIFY2(h.window->footerText().contains(QStringLiteral("newer changes")), qPrintable(h.window->footerText()));
    // Committed = the revision that was submitted.
    QCOMPARE(fromJson(committed(h.rig), nullptr)->ports[0].name, QStringLiteral("KAT500"));
}

// INV-13: Escape and Ctrl+W (reject() without a close event) ask when dirty; nothing asks when clean.
void tst_BridgeWindow::escapeAndCtrlWAsk()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    QVERIFY(QTest::qWaitForWindowExposed(h.window.get()));
    h.rename(0, QStringLiteral("KAT500"));
    h.asker.asked.clear();
    h.asker.answers[QStringLiteral("discardEdits")] = false;
    h.window->activateWindow();
    QTest::keyClick(h.window.get(), Qt::Key_Escape);
    QCOMPARE(h.asker.asked, QStringList{QStringLiteral("discardEdits")});
    QVERIFY(h.window->isVisible());
    QTest::keyClick(h.window.get(), Qt::Key_W, Qt::ControlModifier);
    QCOMPARE(h.asker.asked, (QStringList{QStringLiteral("discardEdits"), QStringLiteral("discardEdits")}));
    QVERIFY(h.window->isVisible());
    // The close button (a close event) asks too.
    QVERIFY(!h.window->close());
    QCOMPARE(h.asker.asked.size(), 3);
    QVERIFY(h.window->isVisible());
    // Yes: it closes, asked once.
    h.asker.answers[QStringLiteral("discardEdits")] = true;
    QTest::keyClick(h.window.get(), Qt::Key_Escape);
    QCOMPARE(h.asker.asked.size(), 4);
    QVERIFY(!h.window->isVisible());

    // Clean: Escape closes without a question.
    Harness clean;
    clean.rig.installedRunning(configA());
    clean.open();
    QVERIFY(QTest::qWaitForWindowExposed(clean.window.get()));
    QTest::keyClick(clean.window.get(), Qt::Key_Escape);
    QVERIFY(clean.asker.asked.isEmpty());
    QVERIFY(!clean.window->isVisible());
}

// INV-13 / D57: closing is refused while an operation is outstanding (close button, Escape, Ctrl+W), and a
// session end is refused too; once the final is delivered, it closes.
void tst_BridgeWindow::closeRefusedWhileBusy()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    QVERIFY(QTest::qWaitForWindowExposed(h.window.get()));
    h.blockAt(QStringLiteral("inhibitStarts"), std::chrono::seconds(0));
    h.window->saveAndApply();
    QVERIFY(h.gate->waitEntered());
    QVERIFY(h.window->busy());
    QVERIFY(h.window->refusesSessionEnd());
    QVERIFY(!h.window->close());
    QTest::keyClick(h.window.get(), Qt::Key_Escape);
    QTest::keyClick(h.window.get(), Qt::Key_W, Qt::ControlModifier);
    QVERIFY(h.window->isVisible());
    QCOMPARE(h.asker.notices, (QStringList{QStringLiteral("busy"), QStringLiteral("busy"), QStringLiteral("busy")}));
    QVERIFY(h.asker.asked.isEmpty());   // nothing offered that discards N
    h.gate->release();
    QVERIFY(h.waitIdle());
    QVERIFY(!h.window->refusesSessionEnd());
    QVERIFY(h.window->close());
}

// INV-23: the box shows the real start type at load, and the footer says when the bridge is disabled.
void tst_BridgeWindow::autoStartIsRealStartType()
{
    Harness on;
    on.rig.installedRunning(configA());
    on.open();
    QVERIFY(on.window->autoStartBox()->isChecked());
    QVERIFY(!on.window->editor().dirty());

    Harness off;
    off.rig.installedRunning(configA());
    off.rig.service.startsBlocked = true;
    off.open();
    QVERIFY(!off.window->autoStartBox()->isChecked());
    QVERIFY(!off.window->editor().dirty());
    QCOMPARE(off.window->footerText(), QStringLiteral("The bridge is disabled. Save & apply to start it."));

    // Nothing installed has no start type: the box shows the one an install sets (v1.1 fix after WS-17), without an edit.
    Harness none;
    none.open();
    QVERIFY(none.window->autoStartBox()->isChecked());
    QVERIFY(!none.window->editor().dirty());
}

// INV-23: Start allows starts (R9), and the box follows without an edit. Kills: no refresh after Start.
void tst_BridgeWindow::autoStartRefreshedAfterStart()
{
    Harness h;
    h.rig.installedRunning(configA());
    // A stopped, disabled bridge (as a failed apply leaves it).
    h.rig.service.requestStop({});
    h.rig.service.startsBlocked = true;
    h.open();
    QVERIFY(!h.window->autoStartBox()->isChecked());
    QVERIFY(h.window->button(QStringLiteral("asbStart"))->isEnabled());
    h.window->startService();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.size(), 1);
    QCOMPARE(h.finals[0].outcome, Outcome::Ok);
    QVERIFY(!h.rig.service.startsBlocked);
    QVERIFY(h.window->autoStartBox()->isChecked());
    QVERIFY(!h.window->editor().dirty());
}

// INV-23: a failed apply leaves starts blocked; the box shows it afterwards.
void tst_BridgeWindow::autoStartRefreshedAfterFailedApply()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    QVERIFY(h.window->autoStartBox()->isChecked());
    h.rig.firewall.fail[QStringLiteral("sync")] = QStringLiteral("the provider refused the rule");
    h.rename(0, QStringLiteral("KAT500"));
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals[0].outcome, Outcome::Uncommitted);
    QVERIFY(!h.window->editor().dirty());
    QVERIFY(h.rig.service.startsBlocked);
    QVERIFY(!h.window->autoStartBox()->isChecked());
}

// INV-23: with unsaved edits the box is not overwritten (the user's choice is an edit).
void tst_BridgeWindow::autoStartNotRefreshedWhenDirty()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.service.requestStop({});
    h.rig.service.startsBlocked = true;
    h.open();
    QVERIFY(!h.window->autoStartBox()->isChecked());
    h.window->autoStartBox()->setChecked(true);   // the user ticks it: an edit
    QVERIFY(h.window->editor().dirty());
    h.blockAt(QStringLiteral("start"), std::chrono::seconds(0));
    h.window->startService();
    QVERIFY(h.gate->waitEntered());
    h.window->autoStartBox()->setChecked(false);  // and unticks it again while Start runs
    h.gate->release();
    QVERIFY(h.waitIdle());
    QVERIFY(h.window->editor().dirty());
    QVERIFY(!h.window->autoStartBox()->isChecked());
    QVERIFY(!h.rig.service.startsBlocked);
}

// INV-23: a start type that cannot be read is not "off": the box is undetermined and disabled, and Save & apply keeps
// the installed service (Run), never the untick route that would remove it.
void tst_BridgeWindow::unknownStartTypeKeepsTheService()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.autoStartUnreadable = true;
    h.open();
    QCOMPARE(h.window->autoStartBox()->checkState(), Qt::PartiallyChecked);
    QVERIFY(!h.window->autoStartBox()->isEnabled());
    QVERIFY(!h.window->editor().dirty());
    h.rename(0, QStringLiteral("KAT500"));
    h.window->saveAndApply();
    QVERIFY(!h.asker.asked.contains(QStringLiteral("untick")));
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals[0].outcome, Outcome::Ok);
    QCOMPARE(h.rig.service.installation.state, InstallState::Current);
    QVERIFY(h.rig.service.engineRunning);
}

// The 1 s refresh asks the service manager for the state only: no installation evidence (on Windows detect reads the
// installed exe for its digest), and nothing at all of the installation while an operation runs.
void tst_BridgeWindow::refreshReadsTheStateOnly()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    h.rig.world.log.clear();
    h.window->refreshStatus();
    h.window->refreshStatus();
    QCOMPARE(h.rig.world.log.count(QStringLiteral("service:detect")), 0);
    QCOMPARE(h.rig.world.log.count(QStringLiteral("service:query")), 2);
    QVERIFY(h.window->serviceText().startsWith(QStringLiteral("Running")));
    h.rig.service.requestStop({});
    h.window->refreshStatus();
    QVERIFY2(h.window->serviceText().startsWith(QStringLiteral("Stopped")), qPrintable(h.window->serviceText()));

    // Not installed: detected again when idle (cheap: no service, nothing read).
    Harness none;
    none.open();
    none.rig.world.log.clear();
    none.window->refreshStatus();
    QCOMPARE(none.rig.world.log.count(QStringLiteral("service:detect")), 1);
}

// INV-118 (WS-5 deferred gate): a native call blocked before R4 past the barrier deadline -> interim banner, close
// refused; the call returns -> final StateUnknown; the window is alive, N dirty, and Save & apply works.
void tst_BridgeWindow::preR4TimeoutKeepsEditorAndDirtyN()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    QVERIFY(QTest::qWaitForWindowExposed(h.window.get()));
    h.rename(0, QStringLiteral("KAT500"));
    h.blockAt(QStringLiteral("inhibitStarts"), std::chrono::seconds(31));   // past the 30 s barrier, then blocked
    h.window->saveAndApply();
    QVERIFY(h.gate->waitEntered());
    QVERIFY(QTest::qWaitFor([&] { return !h.window->bannerText().isEmpty(); }, 10000));
    QVERIFY2(h.window->bannerText().startsWith(QStringLiteral("Still processing")), qPrintable(h.window->bannerText()));
    QVERIFY(h.finals.isEmpty());   // an interim is not a result
    QVERIFY(!h.window->close());
    QVERIFY(h.window->isVisible());
    h.gate->release();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.size(), 1);
    QCOMPARE(h.finals[0].outcome, Outcome::StateUnknown);
    QCOMPARE(h.finals[0].persisted, Persisted::None);
    QVERIFY(h.window->isVisible());
    QVERIFY(h.window->editor().dirty());
    QCOMPARE(h.window->editor().config().ports[0].name, QStringLiteral("KAT500"));
    QVERIFY(h.window->bannerText().isEmpty());
    QCOMPARE(committed(h.rig), configA());

    // Save & apply works again and commits N.
    h.rig.service.onCall = nullptr;
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.size(), 2);
    QCOMPARE(h.finals[1].outcome, Outcome::Ok);
    QVERIFY(!h.window->editor().dirty());
    QCOMPARE(fromJson(committed(h.rig), nullptr)->ports[0].name, QStringLiteral("KAT500"));
}

// INV-118: an edit made during the interim is what the next Save & apply submits.
void tst_BridgeWindow::secondSaveAfterEditDuringInterim()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    h.blockAt(QStringLiteral("inhibitStarts"), std::chrono::seconds(31));
    h.window->saveAndApply();
    QVERIFY(h.gate->waitEntered());
    QVERIFY(QTest::qWaitFor([&] { return !h.window->bannerText().isEmpty(); }, 10000));
    h.rename(0, QStringLiteral("KPA500"));   // during the interim
    h.gate->release();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals[0].outcome, Outcome::StateUnknown);
    QVERIFY(h.window->editor().dirty());
    h.rig.service.onCall = nullptr;
    const Submission next = h.window->editor().submission(true);
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals[1].outcome, Outcome::Ok);
    QCOMPARE(h.finals[1].persistedSha256, next.sha256);
    QCOMPARE(committed(h.rig), next.bytes);
    QVERIFY(!h.window->editor().dirty());
}

// INV-102: after a committed save the window shows the committed config and no pending note.
void tst_BridgeWindow::saveThenReloadShowsCommittedNoPending()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    const Submission s = h.window->editor().submission(true);
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals[0].outcome, Outcome::Ok);
    QCOMPARE(committed(h.rig), s.bytes);
    QVERIFY(!h.rig.has(QString::fromLatin1(ConfigFiles::kPending)));
    h.window->reload(true);
    QVERIFY(!h.window->editor().fromPending());
    QVERIFY(h.window->pendingNoteText().isEmpty());
    QCOMPARE(h.window->editor().source().kind, Source::Committed);
    QCOMPARE(h.window->editor().source().sha256, s.sha256);
}

// INV-102: a pending file written after the load (a CLI edit) is replaced only after the window asks.
void tst_BridgeWindow::cliEditAfterLoadThenSaveAsksToReplace()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    // Meanwhile, the CLI stages config C.
    h.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configC(), QStringLiteral("root"));
    h.asker.answers[QStringLiteral("supersede")] = false;
    h.rig.world.log.clear();
    h.window->saveAndApply();
    QCOMPARE(h.asker.asked, QStringList{QStringLiteral("supersede")});
    QVERIFY(!h.window->busy());
    QCOMPARE(h.mutations(), QStringList());
    QCOMPARE(pending(h.rig), configC());
    // Replace.
    h.asker.answers[QStringLiteral("supersede")] = true;
    const Submission s = h.window->editor().submission(true);
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals[0].outcome, Outcome::Ok);
    QCOMPARE(committed(h.rig), s.bytes);
    QVERIFY(!h.rig.has(QString::fromLatin1(ConfigFiles::kPending)));
}

// INV-29: the window's Start goes through the Applier: the firewall is synced from config.json before the start.
void tst_BridgeWindow::startGoesThroughApplier()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.service.requestStop({});
    h.open();
    h.rig.world.log.clear();
    h.window->startService();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals[0].outcome, Outcome::Ok);
    const int sync = h.rig.world.log.indexOf(QStringLiteral("firewall:sync"));
    const int start = h.rig.world.log.indexOf(QStringLiteral("service:start"));
    QVERIFY(sync >= 0);
    QVERIFY(start > sync);
    QCOMPARE(h.rig.world.log.indexOf(QStringLiteral("applyLock:acquired")) >= 0, true);
}

// D53/S20 (WS-9's deferred gate, GUI half): while the installed, activated bridge is not running the window re-runs the
// comparator for the committed config and the token's generation, once per episode; a mismatch is named, a failed
// check says nothing, and a running bridge is not checked (it checks itself).
void tst_BridgeWindow::staleFirewallRuleShownWhileNotRunning()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.service.requestStop({});   // exit 7 leaves it stopped (SCM recovery aside)
    int checks = 0;
    std::optional<FirewallGeneration> seen;
    std::optional<BridgeConfig> seenConfig;
    IFirewallMonitor::Check verdict = IFirewallMonitor::Check::Mismatch;
    h.firewallCheck = [&](const BridgeConfig& c, const FirewallGeneration& g,
                          std::function<void(IFirewallMonitor::Check, const QString&)> done) {
        ++checks;
        seen = g;
        seenConfig = c;
        QMetaObject::invokeMethod(qApp, [done, verdict] { done(verdict, QStringLiteral("extra rule")); }, Qt::QueuedConnection);
        return true;
    };
    h.open();
    QCOMPARE(checks, 1);
    QVERIFY(QTest::qWaitFor([&] { return !h.window->firewallCheckRunning(); }));
    QVERIFY2(h.window->serviceText().contains(QStringLiteral("stale firewall rule detected")), qPrintable(h.window->serviceText()));
    QVERIFY(seen.has_value());
    QCOMPARE(seen->id, QByteArrayLiteral("0123456789abcdef"));   // the generation start.token names
    QCOMPARE(*seenConfig, *fromJson(configA(), nullptr));
    // Once per episode: a refresh does not check again.
    h.window->refreshStatus();
    QCOMPARE(checks, 1);
    // Start re-syncs the rules and the bridge runs: the note goes, and nothing is checked while it runs.
    h.window->startService();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals[0].outcome, Outcome::Ok);
    h.window->refreshStatus();
    QVERIFY2(!h.window->serviceText().contains(QStringLiteral("stale")), qPrintable(h.window->serviceText()));
    QCOMPARE(checks, 1);

    // A failed check says nothing.
    Harness failed;
    failed.rig.installedRunning(configA());
    failed.rig.service.requestStop({});
    failed.firewallCheck = [&](const BridgeConfig&, const FirewallGeneration&,
                               std::function<void(IFirewallMonitor::Check, const QString&)> done) {
        done(IFirewallMonitor::Check::Failed, QStringLiteral("WMI unavailable"));
        return true;
    };
    failed.open();
    QVERIFY(!failed.window->serviceText().contains(QStringLiteral("stale")));
}

void tst_BridgeWindow::firewallNotCheckedWhileRunning()
{
    Harness h;
    h.rig.installedRunning(configA());
    int checks = 0;
    h.firewallCheck = [&](const BridgeConfig&, const FirewallGeneration&, std::function<void(IFirewallMonitor::Check, const QString&)>) {
        ++checks;
        return true;
    };
    h.open();
    h.window->refreshStatus();
    QCOMPARE(checks, 0);
}

#ifdef _WIN32
#define LINUX_ROUTE_ONLY() QSKIP("the pkexec helper route is Linux only")
#else
#define LINUX_ROUTE_ONLY() (void)0
#endif

// WS-9b deferred gate: a failure after R4 through the helper (Uncommitted, Staged, the submitted digest) clears dirty,
// exactly when HelperClient's own rule (clearsDirty) says so; the staged edits are what the window shows.
void tst_BridgeWindow::linuxRouteStagedClearsDirty()
{
    LINUX_ROUTE_ONLY();
#ifndef _WIN32
    HelperHarness h;
    h.rig.installedRunning(configA());
    h.open();
    h.rename(QStringLiteral("KAT500"));
    const Submission s = h.window->editor().submission(true);
    h.rig.firewall.fail[QStringLiteral("sync")] = QStringLiteral("the rule could not be written");
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.proc.runs, 1);
    QCOMPARE(h.finals.size(), 1);
    QCOMPARE(h.finals[0].outcome, Outcome::Uncommitted);
    QCOMPARE(h.finals[0].persisted, Persisted::Staged);
    QVERIFY(h.actions.lastOutcome());
    QCOMPARE(h.actions.lastOutcome()->kind, cli::HelperClient::Outcome::Kind::Done);
    QVERIFY(h.actions.lastOutcome()->clearsDirty);
    QCOMPARE(h.actions.lastOutcome()->submittedSha256, s.sha256);
    QVERIFY(!h.window->editor().dirty());
    QVERIFY(h.window->editor().fromPending());
    QCOMPARE(pending(h.rig), s.bytes);
#endif
}

// WS-9b deferred gate: a response whose persistedSha256 is not the digest of the bytes the window submitted keeps dirty.
void tst_BridgeWindow::linuxRouteDigestMismatchKeepsDirty()
{
    LINUX_ROUTE_ONLY();
#ifndef _WIN32
    HelperHarness h;
    h.rig.installedRunning(configA());
    h.open();
    h.rename(QStringLiteral("KAT500"));
    h.proc.rewriteOut = [](const QByteArray& out) {
        QJsonObject o = QJsonDocument::fromJson(out).object();
        o.insert(QStringLiteral("persistedSha256"), QString(64, QLatin1Char('a')));
        return QJsonDocument(o).toJson(QJsonDocument::Compact);
    };
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.size(), 1);
    QCOMPARE(h.finals[0].outcome, Outcome::Ok);
    QCOMPARE(h.finals[0].persisted, Persisted::Committed);
    QVERIFY(!h.actions.lastOutcome()->clearsDirty);
    QVERIFY(h.window->editor().dirty());
#endif
}

// WS-9b deferred gate: a barrier deadline in the helper (pre-commit StateUnknown, persisted None) keeps dirty.
void tst_BridgeWindow::linuxRouteBarrierDeadlineKeepsDirty()
{
    LINUX_ROUTE_ONLY();
#ifndef _WIN32
    HelperHarness h;
    h.rig.installedRunning(configA());
    h.rig.service.neverReleases = true;
    h.open();
    h.rename(QStringLiteral("KAT500"));
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.size(), 1);
    QCOMPARE(h.finals[0].outcome, Outcome::StateUnknown);
    QCOMPARE(h.finals[0].persisted, Persisted::None);
    QVERIFY(!h.actions.lastOutcome()->clearsDirty);
    QVERIFY(h.window->editor().dirty());
    QCOMPARE(h.window->editor().config().ports[0].name, QStringLiteral("KAT500"));
    QCOMPARE(committed(h.rig), configA());
#endif
}

// WS-9b deferred gate (D57): a slow helper gives the waiting notice as the interim banner; the window refuses to close
// while HelperClient is busy, and closes once the result arrived.
void tst_BridgeWindow::linuxRouteWaitingBannerAndCloseRefused()
{
    LINUX_ROUTE_ONLY();
#ifndef _WIN32
    HelperHarness h(std::chrono::milliseconds(50));
    h.rig.installedRunning(configA());
    h.open();
    QVERIFY(QTest::qWaitForWindowExposed(h.window.get()));
    h.proc.hold = true;
    h.window->startService();
    QVERIFY(h.client.busy());
    QVERIFY(h.window->busy());
    QVERIFY(QTest::qWaitFor([&] { return !h.window->bannerText().isEmpty(); }, 5000));
    QVERIFY2(h.window->bannerText().contains(QStringLiteral("Still working")), qPrintable(h.window->bannerText()));
    QVERIFY(h.finals.isEmpty());
    QVERIFY(!h.window->close());
    QTest::keyClick(h.window.get(), Qt::Key_Escape);
    QVERIFY(h.window->isVisible());
    QCOMPARE(h.asker.notices, (QStringList{QStringLiteral("busy"), QStringLiteral("busy")}));
    h.proc.release();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.size(), 1);
    QCOMPARE(h.finals[0].outcome, Outcome::Ok);
    QVERIFY(h.window->bannerText().isEmpty());
    QVERIFY(h.window->close());
#endif
}

// pkexec's dialog dismissed (126): "Cancelled; nothing was changed.", dirty kept, nothing touched.
void tst_BridgeWindow::linuxRouteCancelledChangesNothing()
{
    LINUX_ROUTE_ONLY();
#ifndef _WIN32
    HelperHarness h;
    h.rig.installedRunning(configA());
    h.open();
    h.rename(QStringLiteral("KAT500"));
    cli::IHelperProcess::Exit dismissed;
    dismissed.code = cli::HelperClient::kPkexecDismissed;
    h.proc.replaceWith = dismissed;
    h.rig.world.log.clear();
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.size(), 1);
    QCOMPARE(h.finals[0].outcome, Outcome::NothingChanged);
    QVERIFY2(h.window->footerText().startsWith(QStringLiteral("Cancelled; nothing was changed.")), qPrintable(h.window->footerText()));
    QVERIFY(h.window->editor().dirty());
    QCOMPARE(committed(h.rig), configA());
    QVERIFY(!h.rig.has(QString::fromLatin1(ConfigFiles::kPending)));
    QVERIFY(!h.rig.world.log.contains(QStringLiteral("applyLock:acquired")));
#endif
}

// Unticked Save & apply on Linux goes to the helper's deactivate with the edits (INV-94 on the helper route).
void tst_BridgeWindow::linuxRouteUntickDeactivates()
{
    LINUX_ROUTE_ONLY();
#ifndef _WIN32
    HelperHarness h;
    h.rig.installedRunning(configA());
    h.open();
    h.rename(QStringLiteral("KAT500"));
    h.window->autoStartBox()->setChecked(false);
    const Submission s = h.window->editor().submission(false);
    h.asker.answers[QStringLiteral("untick")] = true;
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.size(), 1);
    QCOMPARE(h.finals[0].outcome, Outcome::SavedNotApplied);
    QCOMPARE(h.actions.lastOutcome()->op, cli::helper::Op::Deactivate);
    QVERIFY(h.actions.lastOutcome()->clearsDirty);
    QVERIFY(!h.window->editor().dirty());
    QCOMPARE(pending(h.rig), s.bytes);
    QCOMPARE(committed(h.rig), configA());
    QCOMPARE(h.rig.service.installation.state, InstallState::Current);
    QVERIFY(h.rig.service.startsBlocked);
    QVERIFY(h.window->editor().fromPending());
#endif
}

// The TUI's bug class, checked here: "binds every interface" is a property of the parsed address.
void tst_BridgeWindow::bindsEveryInterfaceByValue_data()
{
    QTest::addColumn<QString>("bind");
    QTest::addColumn<bool>("every");
    for (const char* a : {"0.0.0.0", "000.000.000.000", "00.0.00.0", " 0.0.0.0 ", "::", "0:0:0:0:0:0:0:0",
                          "0000:0000:0000:0000:0000:0000:0000:0000", "0::0", "::0", "::ffff:0.0.0.0"})
        QTest::newRow(a) << QString::fromLatin1(a) << true;
    QTest::newRow("(blank)") << QString() << true;
    for (const char* a : {"127.0.0.1", "192.168.1.20", "0.0.0.1", "::1", "::ffff:127.0.0.1", "not-an-address"})
        QTest::newRow(a) << QString::fromLatin1(a) << false;
}
void tst_BridgeWindow::bindsEveryInterfaceByValue()
{
    QFETCH(QString, bind);
    QFETCH(bool, every);
    QCOMPARE(bindsEveryInterface(bind), every);
}

// Every spelling the validator accepts for a wildcard gets the exposure question; specific addresses do not.
void tst_BridgeWindow::exposureAskedForEveryWildcardSpelling_data()
{
    QTest::addColumn<QString>("bind");
    QTest::addColumn<bool>("asked");
    for (const char* a : {"0.0.0.0", "000.000.000.000", "::", "0:0:0:0:0:0:0:0", "0000:0000:0000:0000:0000:0000:0000:0000", "0::0"})
        QTest::newRow(a) << QString::fromLatin1(a) << true;
    for (const char* a : {"127.0.0.1", "::1"})
        QTest::newRow(a) << QString::fromLatin1(a) << false;
}
void tst_BridgeWindow::exposureAskedForEveryWildcardSpelling()
{
    QFETCH(QString, bind);
    QFETCH(bool, asked);
    const QByteArray cfg = configWithBind(bind);
    QVERIFY2(fromJson(cfg, nullptr)->validate().isEmpty(), qPrintable(fromJson(cfg, nullptr)->validate().join(QLatin1Char(' '))));
    Harness h;
    h.rig.installedRunning(cfg);
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    h.asker.answers[QStringLiteral("exposure")] = false;
    h.rig.world.log.clear();
    h.window->saveAndApply();
    QVERIFY2(h.asker.notices.isEmpty(), qPrintable(h.asker.texts.join(QLatin1Char('|'))));
    QCOMPARE(h.asker.asked.contains(QStringLiteral("exposure")), asked);
    if (asked) {
        QVERIFY(!h.window->busy());
        QCOMPARE(h.mutations(), QStringList());   // cancelled: nothing written, no service call (INV-35)
    } else {
        QVERIFY(h.waitIdle());
    }
}

// On Linux the allow-list is recorded, not enforced (D7): a wildcard port with an allow-list is still exposed there.
void tst_BridgeWindow::linuxExposureIgnoresTheAllowList()
{
    Harness h{Installation::Flavor::Linux};
    h.rig.installedRunning(configA());   // 0.0.0.0, allowed 10.0.0.5
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    h.asker.answers[QStringLiteral("exposure")] = false;
    h.window->saveAndApply();
    QCOMPARE(h.asker.asked, QStringList{QStringLiteral("exposure")});
    QVERIFY(h.asker.texts.last().contains(QStringLiteral("not enforced")));
    QVERIFY(!h.window->busy());

    Harness w;   // Windows: the firewall enforces the list, so the same port is not exposed
    w.rig.installedRunning(configA());
    w.open();
    w.rename(0, QStringLiteral("KAT500"));
    w.window->saveAndApply();
    QVERIFY(!w.asker.asked.contains(QStringLiteral("exposure")));
    QVERIFY(w.waitIdle());
}

// Discard saved changes with unsaved edits open: the question says the edits go too; then the applied settings show.
void tst_BridgeWindow::discardWithUnsavedEditsSaysSoAndShowsApplied()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configB(), QStringLiteral("root"));
    h.open();
    QVERIFY(h.window->editor().fromPending());
    h.rename(0, QStringLiteral("KAT500"));
    h.asker.answers[QStringLiteral("discardPending")] = false;
    h.window->discardPending();
    QVERIFY(h.asker.texts.last().contains(QStringLiteral("also discards the unsaved changes")));
    QVERIFY(h.window->editor().dirty());
    QVERIFY(h.rig.has(QString::fromLatin1(ConfigFiles::kPending)));
    h.asker.answers[QStringLiteral("discardPending")] = true;
    h.window->discardPending();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    QVERIFY(!h.window->editor().dirty());
    QVERIFY(!h.window->editor().fromPending());
    QVERIFY(h.window->pendingNoteText().isEmpty());
    QCOMPARE(h.window->editor().config(), *fromJson(configA(), nullptr));
}

// Edits made while the discard runs were never consented to: they stay, and the window no longer claims the pending
// file (it is gone).
void tst_BridgeWindow::editsMadeDuringDiscardAreKept()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configB(), QStringLiteral("root"));
    h.open();
    h.asker.answers[QStringLiteral("discardPending")] = true;
    h.blockAt(QStringLiteral("detect"), std::chrono::seconds(0));
    h.window->discardPending();
    QVERIFY(h.gate->waitEntered());
    h.rename(0, QStringLiteral("KAT500"));   // typed while the discard runs
    h.gate->release();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    QVERIFY(!h.rig.has(QString::fromLatin1(ConfigFiles::kPending)));
    QVERIFY(h.window->editor().dirty());
    QCOMPARE(h.window->editor().config().ports[0].name, QStringLiteral("KAT500"));
    QCOMPARE(h.window->findChild<QLineEdit*>(QStringLiteral("asbCardName"))->text(), QStringLiteral("KAT500"));
    QVERIFY(!h.window->editor().fromPending());
    QVERIFY(h.window->pendingNoteText().isEmpty());
    QCOMPARE(h.window->editor().source().kind, Source::Committed);
    QCOMPARE(h.window->editor().source().sha256, digest(configA()));
}

namespace {
// Answers like ScriptedAsker, and runs `whileOpen` while a notice is "open" (a modal dialog's nested event loop).
struct DialogAsker final : IAsker {
    QStringList notices;
    std::function<void()> whileOpen;
    bool confirm(const QString&, const QString&, const QString&, const QString&) override { return false; }
    void notify(const QString& id, const QString&) override
    {
        notices << id;
        if (whileOpen) whileOpen();
    }
};
}  // namespace

// An operation's result arriving while a dialog is open and a card field holds fresh typing: the window stays, the
// field keeps what was typed, the edit stays unsaved.
void tst_BridgeWindow::resultDuringAnOpenDialogKeepsTyping()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.service.requestStop({});
    h.open();
    QVERIFY(QTest::qWaitForWindowExposed(h.window.get()));
    DialogAsker dialogs;
    h.window->setAsker(&dialogs);
    h.blockAt(QStringLiteral("start"), std::chrono::seconds(0));
    h.window->startService();
    QVERIFY(h.gate->waitEntered());
    auto* allowed = h.window->findChild<QLineEdit*>(QStringLiteral("asbCardAllowed"));
    allowed->setFocus();
    allowed->end(false);
    QTest::keyClicks(allowed, QStringLiteral(", 10.0.0.6"));
    dialogs.whileOpen = [&] {   // the busy notice is open when the result arrives
        h.gate->release();
        QVERIFY(QTest::qWaitFor([&] { return !h.window->busy(); }, 20000));
    };
    QVERIFY(!h.window->close());
    QCOMPARE(dialogs.notices, QStringList{QStringLiteral("busy")});
    QCOMPARE(h.finals.size(), 1);
    QVERIFY(h.window->isVisible());
    QCOMPARE(allowed->text(), QStringLiteral("10.0.0.5, 10.0.0.6"));
    QCOMPARE(h.window->editor().config().ports[0].allowedClients, QStringLiteral("10.0.0.5, 10.0.0.6"));
    QVERIFY(h.window->editor().dirty());
    h.window->setAsker(&h.asker);
}

// A card edit not yet saved, on an otherwise clean config, is an edit: close, Escape and Ctrl+W ask. Fields that edit
// as you type (name, allowed, TCP port, an editable combo) are each a row.
void tst_BridgeWindow::unacceptedCardEditOnACleanConfigAsks_data()
{
    QTest::addColumn<QString>("field");
    QTest::addColumn<QString>("typed");
    QTest::newRow("name") << QStringLiteral("asbCardName") << QStringLiteral("X");
    QTest::newRow("allowed") << QStringLiteral("asbCardAllowed") << QStringLiteral("1");
    QTest::newRow("tcp port") << QStringLiteral("asbCardTcpPort") << QStringLiteral("2");
    QTest::newRow("device (editable combo)") << QStringLiteral("asbCardDevice") << QStringLiteral("9");
}
void tst_BridgeWindow::unacceptedCardEditOnACleanConfigAsks()
{
    QFETCH(QString, field);
    QFETCH(QString, typed);
    for (int how = 0; how < 3; ++how) {
        Harness h;
        h.rig.installedRunning(configA());
        h.open();
        QVERIFY(QTest::qWaitForWindowExposed(h.window.get()));
        QVERIFY(!h.window->editor().dirty());
        QWidget* w = h.window->findChild<QWidget*>(field);
        QVERIFY(w);
        QWidget* target = w;
        if (auto* combo = qobject_cast<QComboBox*>(w)) target = combo->lineEdit();
        target->setFocus();
        QTest::keyClicks(target, typed);
        QVERIFY2(h.window->editor().dirty(), qPrintable(field));
        h.asker.answers[QStringLiteral("discardEdits")] = false;
        if (how == 0) QVERIFY(!h.window->close());
        else if (how == 1) QTest::keyClick(h.window.get(), Qt::Key_Escape);
        else QTest::keyClick(h.window.get(), Qt::Key_W, Qt::ControlModifier);
        QCOMPARE(h.asker.asked, QStringList{QStringLiteral("discardEdits")});
        QVERIFY(h.window->isVisible());
    }
}

// The 1 s refresh (service row, statuses, traffic) never refills the table or the card: typing survives it.
void tst_BridgeWindow::refreshNeverTouchesTheCard()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    auto* name = h.window->findChild<QLineEdit*>(QStringLiteral("asbCardName"));
    name->setFocus();
    name->end(false);
    QTest::keyClicks(name, QStringLiteral("-2"));
    h.rig.service.requestStop({});
    h.window->refreshStatus();
    h.window->refreshStatus();
    QCOMPARE(name->text(), QStringLiteral("Amp-2"));
    QCOMPARE(name->cursorPosition(), 5);
    QCOMPARE(h.window->editor().config().ports[0].name, QStringLiteral("Amp-2"));
}

// The Linux helper's cancel (pkexec 126) arriving while the user types: the typing stays.
void tst_BridgeWindow::linuxCancelWhileTypingKeepsTheEdit()
{
    LINUX_ROUTE_ONLY();
#ifndef _WIN32
    HelperHarness h;
    h.rig.installedRunning(configA());
    h.open();
    cli::IHelperProcess::Exit dismissed;
    dismissed.code = cli::HelperClient::kPkexecDismissed;
    h.proc.replaceWith = dismissed;
    h.proc.hold = true;
    h.window->startService();
    QVERIFY(h.window->busy());
    h.rename(QStringLiteral("KAT500"));
    h.proc.release();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.last().outcome, Outcome::NothingChanged);
    QVERIFY(h.window->editor().dirty());
    QCOMPARE(h.window->findChild<QLineEdit*>(QStringLiteral("asbCardName"))->text(), QStringLiteral("KAT500"));
#endif
}

// WS-12's note: the window's dirty rule is TuiApp::clearsDirty's (persisted != None, a non-empty digest, equal to the
// digest of the submitted bytes) on every route, since both routes end in EditorModel::finish.
void tst_BridgeWindow::dirtyRuleMatchesTheTuiRule_data()
{
    QTest::addColumn<int>("persisted");
    QTest::addColumn<int>("digest");   // 0 the submitted bytes' digest, 1 another, 2 empty
    for (int p : {int(Persisted::None), int(Persisted::Staged), int(Persisted::Committed)})
        for (int d : {0, 1, 2})
            QTest::newRow(qPrintable(QStringLiteral("persisted %1, digest %2").arg(p).arg(d))) << p << d;
}
void tst_BridgeWindow::dirtyRuleMatchesTheTuiRule()
{
    QFETCH(int, persisted);
    QFETCH(int, digest);
    EditorModel m;
    Snapshot s;
    s.folder = FolderView::Trusted;
    s.committed.present = true;
    s.committed.bytes = configA();
    s.committed.config = fromJson(configA(), nullptr);
    QVERIFY(m.load(s));
    m.edit().ports[0].name = QStringLiteral("KAT500");
    const Submission sub = m.submission(true);
    Result r;
    r.persisted = Persisted(persisted);
    r.persistedSha256 = digest == 0 ? sub.sha256 : digest == 1 ? QByteArray(64, 'b') : QByteArray();
    // TuiApp::clearsDirty (origin/ws-12-tui src/tui/TuiApp.cpp), restated.
    const bool tui = r.persisted != Persisted::None && !r.persistedSha256.isEmpty() &&
                     r.persistedSha256 == sha256Hex(sub.bytes).toLatin1();
    QCOMPARE(EditorModel::persistedMatches(r, sub), tui);
    QCOMPARE(m.finish(r, sub), tui);
    QCOMPARE(m.dirty(), !tui);
}

// An installation that cannot be read is UNKNOWN, never "not installed": the box is undetermined and
// disabled, Save & apply reads the service again first and changes nothing while it is unknown, and after it recovers
// the save keeps the service (Run). Nothing ever routes to SaveAndRemove / Deactivate from it.
void tst_BridgeWindow::installationUnreadableNeverRemovesTheService_data()
{
    QTest::addColumn<int>("flavor");
    QTest::newRow("windows") << int(Installation::Flavor::Windows);
    QTest::newRow("linux") << int(Installation::Flavor::Linux);
}
void tst_BridgeWindow::installationUnreadableNeverRemovesTheService()
{
    QFETCH(int, flavor);
    Harness h{Installation::Flavor(flavor)};
    h.rig.installedRunning(configA());
    const int rules = int(h.rig.firewall.rules.size());
    h.rig.service.fail[QStringLiteral("detect")] = QStringLiteral("the service manager did not answer");
    h.open();
    QCOMPARE(h.window->autoStartBox()->checkState(), Qt::PartiallyChecked);
    QVERIFY(!h.window->autoStartBox()->isEnabled());
    QVERIFY2(h.window->footerText().contains(QStringLiteral("could not be read")), qPrintable(h.window->footerText()));
    h.rename(0, QStringLiteral("KAT500"));
    h.rig.world.log.clear();
    h.window->saveAndApply();
    QCOMPARE(h.asker.notices, QStringList{QStringLiteral("serviceUnknown")});
    QVERIFY(!h.asker.asked.contains(QStringLiteral("untick")));
    QVERIFY(!h.window->busy());
    QCOMPARE(h.mutations(), QStringList());
    QVERIFY(h.window->editor().dirty());

    // The service manager answers again: the save keeps the service.
    h.rig.service.fail.remove(QStringLiteral("detect"));
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.size(), 1);
    QCOMPARE(h.finals[0].outcome, Outcome::Ok);
    QVERIFY(!h.asker.asked.contains(QStringLiteral("untick")));
    QCOMPARE(h.rig.service.installation.state, InstallState::Current);
    QVERIFY(!h.rig.service.startsBlocked);
    QVERIFY(h.rig.service.engineRunning);
    QCOMPARE(int(h.rig.firewall.rules.size()), rules);
    QVERIFY(!h.rig.has(QString::fromLatin1(ConfigFiles::kPending)));
}

// The exact trigger: detect says Current, the state query throws. The installation stays known, the state
// is shown unknown, and Save & apply keeps the service.
void tst_BridgeWindow::stateUnreadableAtLoadKeepsTheService()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.service.fail[QStringLiteral("query")] = QStringLiteral("D-Bus timeout");
    h.open();
    QVERIFY2(h.window->serviceText().startsWith(QStringLiteral("State unknown")), qPrintable(h.window->serviceText()));
    QVERIFY(!h.window->button(QStringLiteral("asbStart"))->isEnabled());
    QVERIFY(!h.window->button(QStringLiteral("asbStop"))->isEnabled());
    QVERIFY(h.window->autoStartBox()->isChecked());   // the start type itself was read
    h.rig.service.fail.remove(QStringLiteral("query"));
    h.rename(0, QStringLiteral("KAT500"));
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals[0].outcome, Outcome::Ok);
    QVERIFY(!h.asker.asked.contains(QStringLiteral("untick")));
    QCOMPARE(h.rig.service.installation.state, InstallState::Current);
}

// A save that commits while newer edits exist refreshes what the window knows of the disk (committed
// bytes, the token's generation, the status) without touching the editor: no false "running settings other than the
// saved config.json", and the stale-rule check compares against what was just committed.
void tst_BridgeWindow::diagnosticsFollowACommitWithNewerEdits()
{
    Harness h;
    h.rig.installedRunning(configA());
    std::optional<BridgeConfig> checkedConfig;
    std::optional<FirewallGeneration> checkedGeneration;
    h.firewallCheck = [&](const BridgeConfig& c, const FirewallGeneration& g,
                          std::function<void(IFirewallMonitor::Check, const QString&)> done) {
        checkedConfig = c;
        checkedGeneration = g;
        done(IFirewallMonitor::Check::Match, {});
        return true;
    };
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    const Submission s = h.window->editor().submission(true);
    h.blockAt(QStringLiteral("inhibitStarts"), std::chrono::seconds(0));
    h.window->saveAndApply();
    QVERIFY(h.gate->waitEntered());
    h.rename(0, QStringLiteral("KPA500"));   // newer edits
    h.gate->release();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals[0].outcome, Outcome::Ok);
    QVERIFY(h.window->editor().dirty());
    QCOMPARE(committed(h.rig), s.bytes);
    QVERIFY2(!h.window->serviceText().contains(QStringLiteral("other than")), qPrintable(h.window->serviceText()));
    h.window->refreshStatus();
    QVERIFY2(!h.window->serviceText().contains(QStringLiteral("other than")), qPrintable(h.window->serviceText()));
    // The bridge stops (exit 7): the check gets the committed config and the generation the new token names.
    h.rig.service.requestStop({});
    h.window->refreshStatus();
    QVERIFY(checkedConfig.has_value());
    QCOMPARE(*checkedConfig, *fromJson(s.bytes, nullptr));
    QString why;
    const std::optional<IStartToken::Token> token = h.rig.token->read(&why);
    QVERIFY2(token.has_value(), qPrintable(why));
    QCOMPARE(*checkedGeneration, token->generation);
    QCOMPARE(h.window->editor().config().ports[0].name, QStringLiteral("KPA500"));
}

// Typing mid-text in the card's editable combos, then the 1 s refresh and another field's edit: the cursor
// and the text stay where the user left them.
void tst_BridgeWindow::cardCursorSurvivesRefresh_data()
{
    QTest::addColumn<QString>("combo");
    QTest::addColumn<int>("at");
    QTest::addColumn<QString>("typed");
    QTest::addColumn<QString>("expected");
    QTest::newRow("listen") << QStringLiteral("asbCardListen") << 2 << QStringLiteral("1") << QStringLiteral("0.10.0.0");
    QTest::newRow("device") << QStringLiteral("asbCardDevice") << 3 << QStringLiteral("1") << QStringLiteral("COM13");
    QTest::newRow("baud") << QStringLiteral("asbCardBaud") << 2 << QStringLiteral("5") << QStringLiteral("385400");
}
void tst_BridgeWindow::cardCursorSurvivesRefresh()
{
    QFETCH(QString, combo);
    QFETCH(int, at);
    QFETCH(QString, typed);
    QFETCH(QString, expected);
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    QVERIFY(QTest::qWaitForWindowExposed(h.window.get()));
    auto* c = h.window->findChild<QComboBox*>(combo);
    QVERIFY(c && c->lineEdit());
    c->lineEdit()->setFocus();
    c->lineEdit()->setCursorPosition(at);
    QTest::keyClicks(c->lineEdit(), typed);
    QCOMPARE(c->currentText(), expected);
    QCOMPARE(c->lineEdit()->cursorPosition(), at + typed.size());
    h.window->refreshStatus();
    h.window->refreshStatus();
    QCOMPARE(c->currentText(), expected);
    QCOMPARE(c->lineEdit()->cursorPosition(), at + typed.size());
    QTest::keyClicks(c->lineEdit(), typed);   // a second keystroke after the refresh lands where the cursor is
    expected.insert(at + typed.size(), typed);
    QCOMPARE(c->currentText(), expected);
    QCOMPARE(c->lineEdit()->cursorPosition(), at + 2 * typed.size());
    QVERIFY(h.window->editor().dirty());
}

// A refill of the card from the model (the table's On box for the same row) leaves a combo that already shows the
// model's value alone.
void tst_BridgeWindow::tableToggleKeepsTheCardCursor()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    auto* listen = h.window->findChild<QComboBox*>(QStringLiteral("asbCardListen"));
    listen->lineEdit()->setFocus();
    listen->lineEdit()->setCursorPosition(2);
    QTest::keyClicks(listen->lineEdit(), QStringLiteral("1"));
    QCOMPARE(listen->lineEdit()->cursorPosition(), 3);
    QAbstractItemModel* m = h.window->tableModel();
    QVERIFY(m->setData(m->index(0, PortTableModel::On), Qt::Unchecked, Qt::CheckStateRole));
    QCOMPARE(listen->currentText(), QStringLiteral("0.10.0.0"));
    QCOMPARE(listen->lineEdit()->cursorPosition(), 3);
    QVERIFY(!h.window->findChild<QCheckBox*>(QStringLiteral("asbCardEnabled"))->isChecked());
}

// The list holds COM5; the user types com5. The card shows what the model holds, whatever refills it.
void tst_BridgeWindow::deviceKeepsTheTypedCase()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    auto* device = h.window->findChild<QComboBox*>(QStringLiteral("asbCardDevice"));
    device->lineEdit()->selectAll();
    QTest::keyClicks(device->lineEdit(), QStringLiteral("com5"));
    QCOMPARE(h.window->editor().config().ports[0].comPort, QStringLiteral("com5"));
    h.window->refreshStatus();
    h.window->card()->setRow(0);
    h.window->card()->setChoices({QStringLiteral("COM3"), QStringLiteral("COM5"), QStringLiteral("COM9")},
                                 {QStringLiteral("127.0.0.1"), QStringLiteral("0.0.0.0")});
    QCOMPARE(device->currentText(), QStringLiteral("com5"));
    QCOMPARE(h.window->editor().config().ports[0].comPort, QStringLiteral("com5"));
}

// Select-all and type 0 in TCP port: the model holds 0 (shown blank on a fresh load), the field keeps "0"
// through the refresh, another field's edit and a refill.
void tst_BridgeWindow::tcpPortZeroStaysAsTyped()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    auto* tcp = h.window->findChild<QLineEdit*>(QStringLiteral("asbCardTcpPort"));
    tcp->selectAll();
    QTest::keyClicks(tcp, QStringLiteral("0"));
    QCOMPARE(h.window->editor().config().ports[0].tcpPort, 0);
    QCOMPARE(tcp->text(), QStringLiteral("0"));
    h.window->refreshStatus();
    h.rename(0, QStringLiteral("KAT500"));
    h.window->card()->setRow(0);
    QCOMPARE(tcp->text(), QStringLiteral("0"));
}

// An open device list is not cleared by the 1 s refresh or by typing elsewhere.
void tst_BridgeWindow::openDeviceListSurvivesRefreshAndTyping()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    QVERIFY(QTest::qWaitForWindowExposed(h.window.get()));
    auto* device = h.window->findChild<QComboBox*>(QStringLiteral("asbCardDevice"));
    const int items = device->count();
    QVERIFY(items >= 3);
    device->setCurrentIndex(1);   // a choice, then the list opened on it
    const QString chosen = device->currentText();
    device->showPopup();
    QVERIFY(QTest::qWaitFor([&] { return device->view()->isVisible(); }, 5000));
    h.window->refreshStatus();
    h.window->refreshStatus();
    QVERIFY(device->view()->isVisible());
    QCOMPARE(device->count(), items);
    QCOMPARE(device->view()->currentIndex().row(), 1);
    QCOMPARE(device->currentText(), chosen);
    device->hidePopup();
}

namespace {
void clearDetailsKey()
{
    auto& settings = AetherSDR::AppSettings::instance();
    settings.remove(QString::fromLatin1(BridgeWindow::kDetailsKey));
    settings.save();
}
}  // namespace

// First run: the details are folded, the window is the shorter height; a read-only window opens folded whatever was
// saved (readOnlyWindowEditsNothing).
void tst_BridgeWindow::detailsCollapsedOnFirstRun()
{
    clearDetailsKey();
    Harness h;
    h.detailsExpanded.reset();
    h.rig.installedRunning(configA());
    h.open();
    QVERIFY(!h.window->detailsExpanded());
    QVERIFY(h.window->card()->isHidden() || !h.window->card()->isVisibleTo(h.window.get()));
    QPushButton* toggle = h.window->button(QStringLiteral("asbDetailsToggle"));
    QVERIFY(toggle);
    QCOMPARE(toggle->accessibleName(), QStringLiteral("Port details"));
    QCOMPARE(toggle->focusPolicy(), Qt::StrongFocus);
    // The table, the selection and the statuses are the same folded.
    QCOMPARE(h.window->tableModel()->rowCount(), 1);
    QCOMPARE(h.window->card()->row(), 0);
}

// The user's choice is remembered in gui.ini and restored by the next window.
void tst_BridgeWindow::detailsStatePersists()
{
    clearDetailsKey();
    {
        Harness h;
        h.detailsExpanded.reset();
        h.rig.installedRunning(configA());
        h.open();
        QVERIFY(!h.window->detailsExpanded());
        QTest::mouseClick(h.window->button(QStringLiteral("asbDetailsToggle")), Qt::LeftButton);
        QVERIFY(h.window->detailsExpanded());
    }
    QCOMPARE(AetherSDR::AppSettings::instance().value(QString::fromLatin1(BridgeWindow::kDetailsKey)).toBool(), true);
    {
        Harness h;
        h.detailsExpanded.reset();
        h.rig.installedRunning(configA());
        h.open();
        QVERIFY(h.window->detailsExpanded());
        QTest::mouseClick(h.window->button(QStringLiteral("asbDetailsToggle")), Qt::LeftButton);
        QVERIFY(!h.window->detailsExpanded());
    }
    {
        Harness h;
        h.detailsExpanded.reset();
        h.rig.installedRunning(configA());
        h.open();
        QVERIFY(!h.window->detailsExpanded());
    }
    clearDetailsKey();
}

// Starting to edit opens the details: double-click a row, Enter on a row, + Add port.
void tst_BridgeWindow::detailsAutoExpandOnEditAndAdd()
{
    for (int how = 0; how < 3; ++how) {
        Harness h;
        h.detailsExpanded = false;
        h.rig.installedRunning(configA());
        h.open();
        QVERIFY(QTest::qWaitForWindowExposed(h.window.get()));
        QVERIFY(!h.window->detailsExpanded());
        QTableView* t = h.window->table();
        const QRect cell = t->visualRect(h.window->tableModel()->index(0, PortTableModel::Name));
        if (how == 0) {
            // A real double-click: press, release, then the double-click press (Qt emits doubleClicked for it).
            QTest::mouseClick(t->viewport(), Qt::LeftButton, Qt::NoModifier, cell.center());
            QTest::mouseDClick(t->viewport(), Qt::LeftButton, Qt::NoModifier, cell.center());
        } else if (how == 1) {
            t->setFocus();
            t->setCurrentIndex(h.window->tableModel()->index(0, PortTableModel::Name));
            QTest::keyClick(t, Qt::Key_Return);
        } else {
            h.window->addPort();
            QCOMPARE(h.window->tableModel()->rowCount(), 2);
        }
        QVERIFY2(h.window->detailsExpanded(), qPrintable(QString::number(how)));
        QVERIFY(h.window->card()->isVisibleTo(h.window.get()));
    }
}

// An edit-opened card never folds while something is unsaved; once the save is done it folds again.
void tst_BridgeWindow::detailsNoAutoCollapseWhileDirty()
{
    Harness h;
    h.detailsExpanded = false;
    h.rig.installedRunning(configA());
    h.open();
    h.window->addPort();   // auto-expanded, and an edit
    QVERIFY(h.window->detailsExpanded());
    QVERIFY(h.window->editor().dirty());
    QVERIFY(!h.window->autoCollapse());
    QVERIFY(h.window->detailsExpanded());
    h.window->startService();   // an operation's result while dirty: still open
    QVERIFY(h.waitIdle());
    QVERIFY(h.window->detailsExpanded());
    // Remove the new port again and save: clean afterwards, so it folds.
    h.window->table()->selectRow(1);
    h.window->removeSelectedPort();
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QVERIFY(!h.window->editor().dirty());
    QVERIFY(!h.window->detailsExpanded());
}

// Never folds while an operation runs, even with nothing unsaved.
void tst_BridgeWindow::detailsNoAutoCollapseWhileBusy()
{
    Harness h;
    h.detailsExpanded = false;
    h.rig.installedRunning(configA());
    h.rig.service.requestStop({});
    h.open();
    h.window->expandForEditing();   // as a double-click does: open, nothing edited
    QVERIFY(h.window->detailsExpanded());
    QVERIFY(!h.window->editor().dirty());
    h.blockAt(QStringLiteral("start"), std::chrono::seconds(0));
    h.window->startService();
    QVERIFY(h.gate->waitEntered());
    QVERIFY(h.window->busy());
    QVERIFY(!h.window->autoCollapse());
    QVERIFY(h.window->detailsExpanded());
    h.gate->release();
    QVERIFY(h.waitIdle());
    QVERIFY(!h.window->detailsExpanded());   // idle and clean: folded after the result
}

// The user may fold the card with unsaved edits: they are kept, and the folded bar says so.
void tst_BridgeWindow::detailsMarkerWhenCollapsedAndDirty()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    QVERIFY(h.window->detailsExpanded());
    QVERIFY(!h.window->detailsMarkerVisible());
    h.rename(0, QStringLiteral("KAT500"));
    QVERIFY(!h.window->detailsMarkerVisible());   // open: the card itself shows the edit
    QTest::mouseClick(h.window->button(QStringLiteral("asbDetailsToggle")), Qt::LeftButton);
    QVERIFY(!h.window->detailsExpanded());
    QVERIFY(h.window->detailsMarkerVisible());
    QVERIFY(h.window->editor().dirty());
    h.window->refreshStatus();   // the refresh and a result work the same folded
    QVERIFY(h.window->detailsMarkerVisible());
    QTest::mouseClick(h.window->button(QStringLiteral("asbDetailsToggle")), Qt::LeftButton);
    QVERIFY(!h.window->detailsMarkerVisible());
    QCOMPARE(h.window->findChild<QLineEdit*>(QStringLiteral("asbCardName"))->text(), QStringLiteral("KAT500"));
    QCOMPARE(h.window->editor().config().ports[0].name, QStringLiteral("KAT500"));
    // Saved while folded: the marker goes.
    QTest::mouseClick(h.window->button(QStringLiteral("asbDetailsToggle")), Qt::LeftButton);
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QVERIFY(!h.window->editor().dirty());
    QVERIFY(!h.window->detailsMarkerVisible());
}

// Folding shrinks the window by the card; unfolding gives back the height it had.
void tst_BridgeWindow::detailsHeightShrinksAndRestores()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    QVERIFY(QTest::qWaitForWindowExposed(h.window.get()));
    h.window->resize(1040, 760);
    QApplication::processEvents();
    const int open = h.window->height();
    const int card = h.window->card()->parentWidget()->height();
    QVERIFY(card > 100);
    QTableView* t = h.window->table();
    const int tableHeight = t->height();
    QTest::mouseClick(h.window->button(QStringLiteral("asbDetailsToggle")), Qt::LeftButton);
    QApplication::processEvents();
    QVERIFY2(h.window->height() <= open - card + 2 && h.window->height() >= open - card - 12,
             qPrintable(QStringLiteral("%1 -> %2 (card %3)").arg(open).arg(h.window->height()).arg(card)));
    QVERIFY(qAbs(t->height() - tableHeight) <= 2);   // the table keeps its size: no layout jump
    QTest::mouseClick(h.window->button(QStringLiteral("asbDetailsToggle")), Qt::LeftButton);
    QApplication::processEvents();
    QCOMPARE(h.window->height(), open);
}

void tst_BridgeWindow::savedAndAppliedClearsDirtyAndShowsOutcome()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    QVERIFY(!h.window->editor().dirty());
    h.rename(0, QStringLiteral("KAT500"));
    QCOMPARE(h.window->footerText(), QStringLiteral("Unsaved changes. Click Save & apply."));
    h.window->saveAndApply();
    QVERIFY(h.window->busy());
    QVERIFY(!h.window->button(QStringLiteral("asbSaveApply"))->isEnabled());
    QVERIFY(!h.window->button(QStringLiteral("asbStop"))->isEnabled());
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals[0].outcome, Outcome::Ok);
    QCOMPARE(h.finals[0].persisted, Persisted::Committed);
    QVERIFY(!h.window->editor().dirty());
    QVERIFY(h.window->footerText().startsWith(h.finals[0].message.left(20)));
    QVERIFY(h.window->button(QStringLiteral("asbSaveApply"))->isEnabled());
    QVERIFY(h.window->autoStartBox()->isChecked());
    QVERIFY(h.rig.service.engineRunning);
}

// A window without actions (a standard user on Windows) edits nothing and offers nothing that changes anything.
void tst_BridgeWindow::readOnlyWindowEditsNothing()
{
    Rig rig;
    rig.world.checkLock = false;
    rig.installedRunning(configA());
    PlatformViewSource view(*rig.platform, {}, rig.world.clock);
    ReadOnlyActions actions(Installation::Flavor::Windows, QStringLiteral("Read-only: run as administrator to change ports."));
    ScriptedAsker asker;
    BridgeWindow::Options o;
    o.refreshMs = 0;
    BridgeWindow w(view, actions, &asker, o);
    w.show();
    QVERIFY(w.readOnly());
    QVERIFY(!w.detailsExpanded());   // a read-only window opens with the details folded
    QCOMPARE(w.bannerText(), QStringLiteral("Read-only: run as administrator to change ports."));
    for (const char* b : {"asbSaveApply", "asbStart", "asbStop", "asbAddPort", "asbRemovePort", "asbRepair", "asbDiscardPending"})
        QVERIFY2(!w.button(QString::fromLatin1(b))->isEnabled(), b);
    QVERIFY(!w.autoStartBox()->isEnabled());
    QVERIFY(!w.findChild<QLineEdit*>(QStringLiteral("asbCardName"))->isEnabled());
    QCOMPARE(w.tableModel()->rowCount(), 1);
    QVERIFY(!(w.tableModel()->flags(w.tableModel()->index(0, PortTableModel::On)) & Qt::ItemIsUserCheckable));
    rig.world.log.clear();
    w.saveAndApply();
    w.startService();
    w.stopService();
    w.repair();
    w.discardPending();
    w.addPort();
    QCOMPARE(w.tableModel()->rowCount(), 1);
    QVERIFY(asker.asked.isEmpty());
    for (const QString& e : rig.world.log.all())
        QVERIFY2(e.startsWith(QLatin1String("service:detect")) || e.startsWith(QLatin1String("service:query")) ||
                     e.startsWith(QLatin1String("folder:")) || e.startsWith(QLatin1String("io:openExisting")),
                 qPrintable(e));
    QCOMPARE(rig.folder.mutations(), 0);
}

// INV-94 / D48: a pending file is the clean working copy with the note; one equal to config.json is ignored.
void tst_BridgeWindow::pendingLoadedAsCleanWorkingCopy()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configB(), QStringLiteral("root"));
    h.open();
    QVERIFY(h.window->editor().fromPending());
    QVERIFY(!h.window->editor().dirty());
    QCOMPARE(h.window->editor().config(), *fromJson(configB(), nullptr));
    QCOMPARE(h.window->pendingNoteText(), QStringLiteral("▲ Saved, not applied (config.pending.json)."));
    QCOMPARE(h.window->editor().source().kind, Source::Pending);

    Harness equal;
    equal.rig.installedRunning(configA());
    equal.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configA(), QStringLiteral("root"));
    equal.open();
    QVERIFY(!equal.window->editor().fromPending());
    QVERIFY(equal.window->pendingNoteText().isEmpty());
}

void tst_BridgeWindow::discardPendingAsksAndGoesThroughTheGate()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configB(), QStringLiteral("root"));
    h.open();
    h.asker.answers[QStringLiteral("discardPending")] = false;
    h.window->discardPending();
    QVERIFY(!h.window->busy());
    QVERIFY(h.rig.has(QString::fromLatin1(ConfigFiles::kPending)));
    h.asker.answers[QStringLiteral("discardPending")] = true;
    h.window->discardPending();
    QVERIFY(h.window->busy());
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals[0].outcome, Outcome::Ok);
    QVERIFY(!h.rig.has(QString::fromLatin1(ConfigFiles::kPending)));
    QVERIFY(!h.window->editor().fromPending());
    QCOMPARE(h.window->editor().config(), *fromJson(configA(), nullptr));
}

// INV-127: the window passes the digest of the pending bytes it loaded; a file replaced on disk since is kept and the
// result says so. Kills inv127-discard-ignores-digest (with the Applier row) and a window that sends no digest.
void tst_BridgeWindow::discardChecksDigest()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configB(), QStringLiteral("root"));
    h.open();
    QVERIFY(h.window->editor().fromPending());
    QCOMPARE(h.window->editor().source().sha256, digest(configB()));
    h.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configC(), QStringLiteral("root"));   // replaced
    h.asker.answers[QStringLiteral("discardPending")] = true;
    h.window->discardPending();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.last().outcome, Outcome::NothingChanged);
    QVERIFY2(h.finals.last().message.contains(QStringLiteral("were replaced since this window read them")),
             qPrintable(h.finals.last().message));
    QCOMPARE(h.rig.file(QString::fromLatin1(ConfigFiles::kPending)), configC());
}

void tst_BridgeWindow::repairAsksYesNoThenRepairs()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.folder.owner = QStringLiteral("attacker");
    h.open();
    // Nothing can be typed before Repair: an untrusted folder loads no working copy, and the card and the table are
    // not editable (so Repair never runs over pending typing).
    QVERIFY(!h.window->findChild<QLineEdit*>(QStringLiteral("asbCardName"))->isEnabled());
    QVERIFY(!h.window->button(QStringLiteral("asbAddPort"))->isEnabled());
    QVERIFY(!h.window->autoStartBox()->isEnabled());
    h.asker.answers[QStringLiteral("repair")] = false;
    h.window->repair();
    QCOMPARE(h.asker.asked, QStringList{QStringLiteral("repair")});
    QVERIFY(!h.window->busy());
    QCOMPARE(h.rig.folder.mutations(), 0);
    QVERIFY(h.asker.texts.last().contains(QStringLiteral("Nothing is deleted")));
    h.asker.answers[QStringLiteral("repair")] = true;
    h.window->repair();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.rig.folder.renames, 1);
    QVERIFY(!h.window->footerText().startsWith(QStringLiteral("Config not loaded")));
}

// User report 2026-10-16: Save & apply found the installed program was another build and installed this build's over it
// without a word. Now it asks, the default (No, Escape, an unscripted asker) keeps the installed program and applies the
// settings to it, and Update installs.
void tst_BridgeWindow::otherBuildAskedBeforeReplacing_data()
{
    QTest::addColumn<bool>("update");
    QTest::newRow("no-keeps-the-installed-program") << false;
    QTest::newRow("update-installs-this-build") << true;
}

void tst_BridgeWindow::otherBuildAskedBeforeReplacing()
{
    QFETCH(bool, update);
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.service.installation.state = InstallState::IncompleteInstall;
    h.rig.service.installation.otherBuild = true;
    h.rig.service.installation.installedSha256 = QStringLiteral("117eb29a0b1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f708192a3b4c5d6e7f8091a");
    h.rig.service.installation.detail = QStringLiteral("the installed program is a different build");
    h.open();
    QVERIFY2(h.window->serviceText().contains(QStringLiteral("different build")), qPrintable(h.window->serviceText()));
    h.rename(0, QStringLiteral("KAT500"));
    const Submission s = h.window->editor().submission(true);
    h.asker.choices[QStringLiteral("build")] = update ? QStringLiteral("update") : QStringLiteral("keep");
    h.rig.world.log.clear();
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.asker.asked.count(QStringLiteral("build")), 1);
    const int at = h.asker.asked.indexOf(QStringLiteral("build"));
    QVERIFY(at >= 0);
    QString question;
    for (const QString& t : h.asker.texts)
        if (t.contains(QStringLiteral("different build"))) question = t;
    QVERIFY2(question.contains(QStringLiteral("program sha256 117eb29a0b1c")), qPrintable(question));
    QVERIFY2(question.contains(QStringLiteral("The ports restart")), qPrintable(question));
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    QCOMPARE(committed(h.rig), s.bytes);
    QCOMPARE(h.rig.world.log.indexOf(QStringLiteral("service:installOrConfigure")) >= 0, update);
    QCOMPARE(h.rig.service.installation.otherBuild, !update);
    QVERIFY(h.rig.service.engineRunning);
}

// An install that did not finish for another reason (a lock file, the service's configuration, a missing program) is
// finished by Save & apply as before: there is no installed program to keep, so nothing is asked.
void tst_BridgeWindow::unfinishedInstallFinishedWithoutAsking()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.service.installation.state = InstallState::IncompleteInstall;
    h.rig.service.installation.detail = QStringLiteral("a lock file is missing");
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    h.rig.world.log.clear();
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QVERIFY(!h.asker.asked.contains(QStringLiteral("build")));
    QVERIFY(h.rig.world.log.indexOf(QStringLiteral("service:installOrConfigure")) >= 0);
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
}

// The window's Start never replaces the program: an installed other build is started as it is.
void tst_BridgeWindow::startRunsAnInstalledOtherBuild()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.service.requestStop({});
    h.rig.service.installation.state = InstallState::IncompleteInstall;
    h.rig.service.installation.otherBuild = true;
    h.open();
    h.rig.world.log.clear();
    h.window->startService();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    QVERIFY(h.rig.world.log.indexOf(QStringLiteral("service:installOrConfigure")) < 0);
    QVERIFY(h.rig.service.engineRunning);
    QVERIFY(!h.asker.asked.contains(QStringLiteral("build")));
}

// User report 2026-10-16 (the service could not start while the window was open): when an operation's `finished` is
// delivered, whatever its outcome, the window's process holds no apply.lock, no engine.lock and no firewall provider
// session, and the bridge can be started again at once.
void tst_BridgeWindow::nothingHeldWhenFinishedFires_data()
{
    QTest::addColumn<QString>("op");
    QTest::newRow("save-and-apply-ok") << QStringLiteral("save");
    QTest::newRow("start-ok") << QStringLiteral("start");
    QTest::newRow("stop-ok") << QStringLiteral("stop");
    QTest::newRow("ack-timeout") << QStringLiteral("ack-timeout");
    QTest::newRow("barrier-deadline-worker-inside") << QStringLiteral("late");
}

void tst_BridgeWindow::nothingHeldWhenFinishedFires()
{
    QFETCH(QString, op);
    Harness h;
    h.rig.installedRunning(configA());
    if (op == QLatin1String("start")) h.rig.service.requestStop({});
    struct Held {
        bool apply = true, engine = true, session = true, directory = true, folder = true;
    };
    QList<Held> seen;
    h.open();
    QVERIFY2(!h.rig.folder.directoryOpen, "the data folder's handle is still open after the window loaded");
    // The window's own completion hook runs inside finished(), after the window has handled the result.
    h.alsoOnFinished = [&](const Result&) {
        seen << Held{h.rig.world.applyLock.held(), h.rig.engineLock.appliersHold(), h.rig.firewall.connected,
                     h.rig.engineLock.directoryOpen, h.rig.folder.directoryOpen};
    };
    if (op == QLatin1String("ack-timeout")) h.rig.service.ack = AckMode::Never;
    if (op == QLatin1String("late")) h.blockAt(QStringLiteral("requestStop"), std::chrono::seconds(120));
    if (op == QLatin1String("save") || op == QLatin1String("ack-timeout") || op == QLatin1String("late")) {
        h.rename(0, QStringLiteral("KAT500"));
        h.window->saveAndApply();
    } else if (op == QLatin1String("start")) {
        h.window->startService();
    } else {
        h.window->stopService();
    }
    if (op == QLatin1String("late")) {
        QVERIFY(h.gate->waitEntered());
        h.host->pollForInterim();
        QTest::qWait(50);
        QVERIFY(h.window->busy());   // the worker is still inside its native call: no result yet
        h.gate->release();
    }
    QVERIFY(h.waitIdle());
    QCOMPARE(seen.size(), 1);
    QVERIFY2(!seen[0].apply, "apply.lock still held when finished fired");
    QVERIFY2(!seen[0].engine, "engine.lock still held by the window's process when finished fired");
    QVERIFY2(!seen[0].session, "the firewall provider session is still open when finished fired");
    // v1.1 INV-128: nor the lock directory's handle (the operation used dir() / a hold; closeIdle closed it).
    QVERIFY2(!seen[0].directory, "the lock directory's handle is still open when finished fired");
    // Nor the data folder's (the Applier's inspection; the window's reload closes its own again).
    QVERIFY2(!seen[0].folder, "the data folder's handle is still open when finished fired");
    if (op == QLatin1String("ack-timeout")) QCOMPARE(h.finals.last().outcome, Outcome::StateUnknown);
    else if (op == QLatin1String("late")) QVERIFY(h.finals.last().outcome != Outcome::Ok);
    else QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    // The bridge can start now, with the window still open (SCM's restart, or the user's Start).
    h.rig.service.ack = AckMode::Correct;
    h.rig.service.neverReleases = false;
    h.rig.service.requestStop({});
    h.rig.service.onCall = {};
    h.finals.clear();
    h.alsoOnFinished = {};
    h.window->startService();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    QVERIFY(h.rig.service.engineRunning);
    QVERIFY(!h.rig.world.applyLock.held());
    QVERIFY(!h.rig.engineLock.appliersHold());
}

// User report 2026-10-16: "did not confirm loading them within 15 s" now says why when the bridge is not running.
void tst_BridgeWindow::ackTimeoutNamesWhyTheServiceStopped()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.service.staleFirewallAtStart = true;   // the engine's own firewall check ends it (exit 7, no report)
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    const Result& r = h.finals.last();
    QCOMPARE(r.outcome, Outcome::StateUnknown);
    QVERIFY2(r.message.contains(QStringLiteral("did not confirm")), qPrintable(r.message));
    QVERIFY2(r.message.contains(QStringLiteral("The service is not running: the service ended without reporting why")),
             qPrintable(r.message));
    QVERIFY2(r.message.contains(QStringLiteral("exit 7")), qPrintable(r.message));
    QVERIFY2(r.message.contains(QStringLiteral("for example")), qPrintable(r.message));   // a candidate, not a verdict
    QVERIFY2(h.window->footerText().contains(QStringLiteral("not running")), qPrintable(h.window->footerText()));
}

void tst_BridgeWindow::serviceExitDescriptions_data()
{
    QTest::addColumn<int>("state");
    QTest::addColumn<int>("code");   // -1: none reported
    QTest::addColumn<bool>("unreported");
    QTest::addColumn<QString>("says");
    QTest::newRow("running") << int(ServiceQuery::Running) << 7 << true << QString();
    QTest::newRow("starting") << int(ServiceQuery::StartPending) << -1 << true << QString();
    // v1.1 station fix: Windows reports not activated with no code, so a stop without one names it.
    QTest::newRow("stopped-no-code") << int(ServiceQuery::Stopped) << -1 << false << QStringLiteral("no valid start token");
    QTest::newRow("stop-pending") << int(ServiceQuery::StopPending) << -1 << false << QString();
    QTest::newRow("stopped-clean") << int(ServiceQuery::Stopped) << 0 << false << QString();
    QTest::newRow("1067") << int(ServiceQuery::Stopped) << -1 << true << QStringLiteral("Windows error 1067). At start that can be, for example");
    QTest::newRow("lock") << int(ServiceQuery::Stopped) << 4 << false << QStringLiteral("engine.lock");
    QTest::newRow("token") << int(ServiceQuery::Stopped) << 6 << false << QStringLiteral("start token");
    QTest::newRow("folder") << int(ServiceQuery::Stopped) << 3 << false << QStringLiteral("data folder");
    QTest::newRow("firewall") << int(ServiceQuery::Stopped) << 7 << false << QStringLiteral("firewall rules");
    QTest::newRow("other") << int(ServiceQuery::Stopped) << 42 << false << QStringLiteral("exit code 42");
}

void tst_BridgeWindow::serviceExitDescriptions()
{
    QFETCH(int, state);
    QFETCH(int, code);
    QFETCH(bool, unreported);
    QFETCH(QString, says);
    ServiceQuery q;
    q.state = ServiceQuery::State(state);
    if (code >= 0) q.exitCode = code;
    q.endedUnreported = unreported;
    const QString text = describeServiceExit(q);
    if (says.isEmpty()) QVERIFY2(text.isEmpty(), qPrintable(text));
    else QVERIFY2(text.contains(says), qPrintable(text));
}

// V1.1 INV-129: an operation never starts while the window's own firewall check runs, and the
// window never waits for it on its thread: the operation is deferred (busy, "Waiting for the window's firewall
// check", a Cancel) and starts when the check ends.
void tst_BridgeWindow::operationWaitsForTheWindowsFirewallCheck_data()
{
    QTest::addColumn<bool>("idle");
    QTest::newRow("check-finished-in-time") << true;
    QTest::newRow("check-still-running") << false;
}

void tst_BridgeWindow::operationWaitsForTheWindowsFirewallCheck()
{
    QFETCH(bool, idle);
    for (const QString& op : {QStringLiteral("save"), QStringLiteral("start"), QStringLiteral("stop")}) {
        Harness h;
        h.rig.installedRunning(configA());
        if (op == QLatin1String("start")) h.rig.service.requestStop({});
        QList<std::function<void()>> deferred;
        h.firewallIdle = [&](std::function<void()> whenIdle) {
            if (!idle) deferred << whenIdle;
            return idle;
        };
        h.open();
        h.rename(0, QStringLiteral("KAT500"));
        h.rig.world.log.clear();
        if (op == QLatin1String("save")) h.window->saveAndApply();
        else if (op == QLatin1String("start")) h.window->startService();
        else h.window->stopService();
        QVERIFY(h.window->busy());
        if (!idle) {
            QCOMPARE(deferred.size(), 1);
            QVERIFY(h.window->waitingForFirewallCheck());
            QTest::qWait(50);
            QVERIFY(h.finals.isEmpty());
            QVERIFY2(!h.rig.world.log.contains(QStringLiteral("applyLock:acquired")),
                     qPrintable(h.rig.world.log.all().join(QLatin1Char(','))));   // no operation ran
            QVERIFY2(h.window->footerText().contains(QStringLiteral("Waiting for the window's firewall check")),
                     qPrintable(h.window->footerText()));
            QVERIFY(h.window->button(QStringLiteral("asbWaitCancel"))->isVisible());
            deferred.first()();   // the check ended (FirewallChecks posts this on the GUI thread)
            QVERIFY(!h.window->waitingForFirewallCheck());
        }
        QVERIFY(h.waitIdle());
        QCOMPARE(h.finals.size(), 1);
        QVERIFY(h.rig.world.log.contains(QStringLiteral("applyLock:acquired")) || op != QLatin1String("save"));
    }
}

// The window's checks run on threads FirewallChecks owns, with a deadline; waitIdle is
// bounded; a verdict is delivered on the owner's thread; the owner's destruction waits for a running check (bounded),
// joins it, and no verdict arrives after it.
void tst_BridgeWindow::firewallChecksAreOwnedBoundedAndWaitable()
{
    std::mutex m;
    std::condition_variable cv;
    bool release = false;
    std::optional<Deadline> seenDeadline;
    std::atomic<std::thread::id> workThread{};
    auto work = [&](const BridgeConfig&, const FirewallGeneration&, Deadline d) {
        workThread = std::this_thread::get_id();
        {
            std::unique_lock lk(m);
            seenDeadline = d;
            cv.wait(lk, [&] { return release; });
        }
        return FirewallChecks::Verdict{IFirewallMonitor::Check::Mismatch, QStringLiteral("extra rule")};
    };
    const BridgeConfig config = *fromJson(configA(), nullptr);
    const FirewallGeneration g{QByteArrayLiteral("0123456789abcdef")};
    int verdicts = 0;
    std::thread::id verdictThread;
    {
        FirewallChecks checks(work, std::chrono::milliseconds(3000));
        const Deadline before = std::chrono::steady_clock::now();
        QVERIFY(checks.start(config, g, [&](IFirewallMonitor::Check c, const QString&) {
            ++verdicts;
            verdictThread = std::this_thread::get_id();
            QCOMPARE(c, IFirewallMonitor::Check::Mismatch);
        }));
        QVERIFY(QTest::qWaitFor([&] { return checks.running() == 1 && workThread.load() != std::thread::id(); }));
        QVERIFY(!checks.waitIdle(std::chrono::milliseconds(50)));   // bounded: returns while the check runs
        {
            std::lock_guard lk(m);
            QVERIFY(seenDeadline.has_value());
            QVERIFY(*seenDeadline >= before + std::chrono::milliseconds(3000));
            QVERIFY(*seenDeadline <= std::chrono::steady_clock::now() + std::chrono::milliseconds(3000));
            release = true;
        }
        cv.notify_all();
        QVERIFY(checks.waitIdle(std::chrono::milliseconds(5000)));
        QVERIFY(QTest::qWaitFor([&] { return verdicts == 1; }));
        QCOMPARE(verdictThread, std::this_thread::get_id());   // on the owner's (GUI) thread
        QVERIFY(workThread.load() != std::this_thread::get_id());

        // A second check still running when its owner goes: the destructor waits for it (bounded) and joins it.
        release = false;
        QVERIFY(checks.start(config, g, [&](IFirewallMonitor::Check, const QString&) { ++verdicts; }));
        QVERIFY(QTest::qWaitFor([&] { return checks.running() == 1; }));
        std::thread releaser([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            {
                std::lock_guard lk(m);
                release = true;
            }
            cv.notify_all();
        });
        releaser.detach();
    }   // ~FirewallChecks: waits for the second check, then joins it
    QTest::qWait(100);
    QCOMPARE(verdicts, 1);   // the second verdict was not posted after its owner went
}

// An installed other build the user kept is still diagnosed: when it stops with a stale rule, the
// comparator's mismatch is shown.
void tst_BridgeWindow::staleRuleShownForAKeptOtherBuild()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.service.installation.state = InstallState::IncompleteInstall;
    h.rig.service.installation.otherBuild = true;
    int checks = 0;
    h.firewallCheck = [&](const BridgeConfig&, const FirewallGeneration&, std::function<void(IFirewallMonitor::Check, const QString&)> done) {
        ++checks;
        done(IFirewallMonitor::Check::Mismatch, QStringLiteral("extra rule"));
        return true;
    };
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    h.asker.choices[QStringLiteral("build")] = QStringLiteral("keep");   // No, keep it (v1.1: no answer is Cancel)
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    QVERIFY(h.rig.service.installation.otherBuild);
    QCOMPARE(checks, 0);   // running: nothing to check
    h.rig.service.requestStop({});   // it stops itself (exit 7)
    h.window->refreshStatus();
    QCOMPARE(checks, 1);
    QVERIFY2(h.window->serviceText().contains(QStringLiteral("stale firewall rule detected")), qPrintable(h.window->serviceText()));
}

// After "No, keep it" and a Stop, the Start button is enabled for the kept build and starts it as
// it is (call 22); an install that did not finish for another reason still has no Start.
void tst_BridgeWindow::startEnabledForAKeptOtherBuild()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.service.installation.state = InstallState::IncompleteInstall;
    h.rig.service.installation.otherBuild = true;
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    h.asker.choices[QStringLiteral("build")] = QStringLiteral("keep");   // No, keep it (v1.1: no answer is Cancel)
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    h.window->stopService();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    QVERIFY(!h.rig.service.engineRunning);
    auto* start = h.window->findChild<QPushButton*>(QStringLiteral("asbStart"));
    QVERIFY(start);
    QVERIFY(start->isEnabled());
    h.rig.world.log.clear();
    QTest::mouseClick(start, Qt::LeftButton);
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    QVERIFY(h.rig.service.engineRunning);
    QVERIFY(h.rig.world.log.indexOf(QStringLiteral("service:installOrConfigure")) < 0);   // started as it is

    Harness unfinished;
    unfinished.rig.installedRunning(configA());
    unfinished.rig.service.requestStop({});
    unfinished.rig.service.installation.state = InstallState::IncompleteInstall;
    unfinished.rig.service.installation.detail = QStringLiteral("a lock file is missing");
    unfinished.open();
    QVERIFY(!unfinished.window->findChild<QPushButton*>(QStringLiteral("asbStart"))->isEnabled());
}

// User report 2026-10-16: while the service is starting it runs its own firewall check; the window does not query the
// firewall provider beside it, and checks once the service has stopped.
void tst_BridgeWindow::firewallNotCheckedWhileStarting()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.service.requestStop({});
    h.rig.service.stateOverride = ServiceQuery::StartPending;
    int checks = 0;
    h.firewallCheck = [&](const BridgeConfig&, const FirewallGeneration&, std::function<void(IFirewallMonitor::Check, const QString&)> done) {
        ++checks;
        done(IFirewallMonitor::Check::Match, {});
        return true;
    };
    h.open();
    h.window->refreshStatus();
    QCOMPARE(checks, 0);
    h.rig.service.stateOverride.reset();   // it stopped
    h.window->refreshStatus();
    QCOMPARE(checks, 1);
}

// User report 2026-10-16: a long operation does not look hung: the footer names the step and the seconds so far.
void tst_BridgeWindow::progressShownWhileBusy()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    h.blockAt(QStringLiteral("requestStop"), std::chrono::seconds(0));
    h.window->saveAndApply();
    QVERIFY(h.gate->waitEntered());
    h.window->refreshProgress();
    QVERIFY2(h.window->footerText().startsWith(QStringLiteral("Saving and applying… stopping the bridge · ")),
             qPrintable(h.window->footerText()));
    QVERIFY2(h.window->footerText().endsWith(QStringLiteral(" s")), qPrintable(h.window->footerText()));
    h.gate->release();
    QVERIFY(h.waitIdle());
    h.window->refreshProgress();   // idle: the result's footer stays
    QVERIFY2(!h.window->footerText().contains(QStringLiteral(" · ")) ||
                 !h.window->footerText().startsWith(QStringLiteral("Saving and applying…")),
             qPrintable(h.window->footerText()));
}

// Reload to a config with fewer ports while a later row is selected. The card's row is clamped to the
// replacement list before anything refills the card (the defence in PortCard::fill, which warns, is never reached);
// down to zero ports the card shows no row.
void tst_BridgeWindow::cardRowClampedWhenReloadRemovesTheSelectedRow()
{
    QTest::failOnWarning(QRegularExpression(QStringLiteral("PortCard: row")));
    Harness h;
    h.rig.installedRunning(configThreePorts());
    h.open();
    h.window->table()->selectRow(2);
    QCOMPARE(h.window->card()->row(), 2);
    auto* name = h.window->findChild<QLineEdit*>(QStringLiteral("asbCardName"));
    QVERIFY(name);
    QCOMPARE(name->text(), QStringLiteral("Three"));
    h.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kConfig), configA(), QStringLiteral("root"));
    h.window->reload(true);
    QCOMPARE(h.window->editor().config().ports.size(), 1);
    QCOMPARE(h.window->card()->row(), 0);
    QCOMPARE(h.window->table()->currentIndex().row(), 0);
    QCOMPARE(name->text(), h.window->editor().config().ports[0].name);

    h.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kConfig), configNoPorts(), QStringLiteral("root"));
    h.window->reload(true);
    QVERIFY(h.window->editor().config().ports.isEmpty());
    QCOMPARE(h.window->card()->row(), -1);
    QVERIFY(!h.window->table()->currentIndex().isValid());
    QCOMPARE(h.window->findChild<QLabel*>(QStringLiteral("asbCardTitle"))->text(), QStringLiteral("No port selected"));
    QVERIFY(name->text().isEmpty());
    QVERIFY(!name->isEnabled());
    // The window still works: a refresh and an added port.
    h.window->refreshStatus();
    h.window->addPort();
    QCOMPARE(h.window->card()->row(), 0);
}

// The Discard route: discarding a three-port pending file while row 2 is selected shows the applied
// settings (one port, then none) with the card on a row that exists.
void tst_BridgeWindow::cardRowClampedWhenDiscardRemovesTheSelectedRow()
{
    QTest::failOnWarning(QRegularExpression(QStringLiteral("PortCard: row")));
    for (const bool toZero : {false, true}) {
        Harness h;
        h.rig.installedRunning(configA());
        if (toZero) h.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kConfig), configNoPorts(), QStringLiteral("root"));
        h.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configThreePorts(), QStringLiteral("root"));
        h.open();
        QVERIFY(h.window->editor().fromPending());
        h.window->table()->selectRow(2);
        QCOMPARE(h.window->card()->row(), 2);
        h.asker.answers[QStringLiteral("discardPending")] = true;
        h.window->discardPending();
        QVERIFY(h.waitIdle());
        QCOMPARE(h.finals.last().outcome, Outcome::Ok);
        QVERIFY(!h.window->editor().fromPending());
        auto* name = h.window->findChild<QLineEdit*>(QStringLiteral("asbCardName"));
        if (toZero) {
            QVERIFY(h.window->editor().config().ports.isEmpty());
            QCOMPARE(h.window->card()->row(), -1);
            QVERIFY(!h.window->table()->currentIndex().isValid());
            QVERIFY(name->text().isEmpty());
        } else {
            QCOMPARE(h.window->editor().config().ports.size(), 1);
            QCOMPARE(h.window->card()->row(), 0);
            QCOMPARE(h.window->table()->currentIndex().row(), 0);
            QCOMPARE(name->text(), h.window->editor().config().ports[0].name);
        }
    }
}

// The card's own bound (defence): a row past a working copy replaced under it is no row, and says so; it never indexes
// past the list.
void tst_BridgeWindow::portCardShowsNoRowPastTheList()
{
    Harness h;
    h.rig.installedRunning(configThreePorts());
    h.open();
    EditorModel editor;
    QVERIFY(editor.load(h.view->load()));
    PortCard card(editor, /*windows=*/false);
    card.setChoices({QStringLiteral("COM3")}, {QStringLiteral("0.0.0.0")});
    card.setRow(2);
    QCOMPARE(card.row(), 2);
    QCOMPARE(card.findChild<QLineEdit*>(QStringLiteral("asbCardName"))->text(), QStringLiteral("Three"));
    h.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kConfig), configA(), QStringLiteral("root"));
    QVERIFY(editor.load(h.view->load()));
    QCOMPARE(editor.config().ports.size(), 1);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("PortCard: row 2 is past the 1 ports")));
    card.setChoices({QStringLiteral("COM3"), QStringLiteral("COM4")}, {QStringLiteral("0.0.0.0")});
    QCOMPARE(card.row(), -1);
    QCOMPARE(card.findChild<QLabel*>(QStringLiteral("asbCardTitle"))->text(), QStringLiteral("No port selected"));
    QVERIFY(card.findChild<QLineEdit*>(QStringLiteral("asbCardName"))->text().isEmpty());
}

// Owner request 2026-10-19 ("some more help about what allowed means like perhaps an entry example"): under the
// Allowed field the card shows, in view and not only on hover, an example the validator accepts, what blank means, and
// what this OS does with the list. It is whole at the launch size (1040 x 760, the docs images' size). At the minimum
// size (760 x 560) the body needs more than the window has before this change too (every card row is squeezed), so
// there it is only required to be on the card, in its place.
void tst_BridgeWindow::allowedHintPerPlatform_data()
{
    QTest::addColumn<int>("flavor");
    QTest::newRow("windows") << int(Installation::Flavor::Windows);
    QTest::newRow("linux") << int(Installation::Flavor::Linux);
}

void tst_BridgeWindow::allowedHintPerPlatform()
{
    QFETCH(int, flavor);
    const bool windows = Installation::Flavor(flavor) == Installation::Flavor::Windows;
    Harness h{Installation::Flavor(flavor)};
    h.rig.installedRunning(configA());
    h.open();
    QVERIFY(QTest::qWaitForWindowExposed(h.window.get()));
    h.window->resize(1040, 760);
    QApplication::processEvents();
    QLabel* hint = h.window->card()->allowedHint();
    QVERIFY(hint);
    QCOMPARE(hint->text(), PortCard::allowedHintText(windows));
    // The example is one the field accepts, and blank is explained.
    QVERIFY(hint->text().contains(QString::fromLatin1(help::kAllowedExample)));
    QVERIFY(AllowedClients::check(QString::fromLatin1(help::kAllowedExample)).isEmpty());
    QVERIFY(hint->text().contains(QStringLiteral("blank = any address")));
    if (windows) {
        // The keyword named is one the firewall accepts; the rule admits only the listed addresses.
        QVERIFY(hint->text().contains(QString::fromLatin1(help::kAllowedKeywordExample)));
        QVERIFY(std::find(kAcceptedFirewallKeywords.begin(), kAcceptedFirewallKeywords.end(),
                          std::string_view(help::kAllowedKeywordExample)) != kAcceptedFirewallKeywords.end());
        QVERIFY(hint->text().contains(QStringLiteral("While Windows Firewall is on, this port's rule lets in only these addresses")));
        QVERIFY(!hint->text().contains(QStringLiteral("enforce"), Qt::CaseInsensitive));
    } else {
        // Linux names no keyword (nothing there understands one) and says the list is not enforced.
        QVERIFY(!hint->text().contains(QString::fromLatin1(help::kAllowedKeywordExample)));
        QVERIFY(hint->text().contains(QStringLiteral("does not enforce this list")));
        QVERIFY(hint->text().contains(QStringLiteral("use the system firewall to restrict who can connect")));
    }
    QVERIFY(!hint->text().contains(QStringLiteral("recorded, not enforced")));
    // In view, whole: wrapped into the height it has, inside the card, below the Allowed field, not over Lines.
    QVERIFY(hint->isVisible());
    QVERIFY(hint->wordWrap());
    QVERIFY2(hint->height() >= hint->heightForWidth(hint->width()),
             qPrintable(QStringLiteral("%1 px for %2 px of text").arg(hint->height()).arg(hint->heightForWidth(hint->width()))));
    QWidget* card = h.window->card();
    const QRect inCard(hint->mapTo(card, QPoint(0, 0)), hint->size());
    QVERIFY2(card->rect().contains(inCard), qPrintable(QStringLiteral("hint %1,%2 %3x%4 in card %5x%6")
                                                              .arg(inCard.x()).arg(inCard.y()).arg(inCard.width())
                                                              .arg(inCard.height()).arg(card->width()).arg(card->height())));
    auto* allowed = card->findChild<QLineEdit*>(QStringLiteral("asbCardAllowed"));
    auto* dtr = card->findChild<QCheckBox*>(QStringLiteral("asbCardDtr"));
    QVERIFY(allowed->mapTo(card, QPoint(0, allowed->height())).y() <= inCard.top());
    QVERIFY(inCard.bottom() < dtr->mapTo(card, QPoint(0, 0)).y());
    // The table above keeps room for its rows.
    QVERIFY2(h.window->table()->viewport()->height() >= 3 * h.window->table()->verticalHeader()->defaultSectionSize(),
             qPrintable(QString::number(h.window->table()->viewport()->height())));
    // At the minimum size: still shown, on the card, between Allowed and Lines.
    h.window->resize(h.window->minimumSize());
    QApplication::processEvents();
    QCOMPARE(h.window->size(), QSize(760, 560));
    QVERIFY(hint->isVisible() && hint->height() > 0);
    const QRect small(hint->mapTo(card, QPoint(0, 0)), hint->size());
    QVERIFY(card->rect().contains(small));
    QVERIFY(allowed->mapTo(card, QPoint(0, allowed->height())).y() <= small.top());
    QVERIFY(small.bottom() < dtr->mapTo(card, QPoint(0, 0)).y());
}

// A check started while the bridge was stopped is still outstanding across Start (or Save & apply) and
// the bridge stopping again (exit 7). Its verdict judged rules the operation rewrote: it is dropped, the stopped episode
// after the stop gets its own check, and that check's verdict is the one shown.
void tst_BridgeWindow::obsoleteFirewallVerdictDroppedAcrossStartAndStop_data()
{
    QTest::addColumn<bool>("save");
    QTest::newRow("start") << false;
    QTest::newRow("save-and-apply") << true;
}

void tst_BridgeWindow::obsoleteFirewallVerdictDroppedAcrossStartAndStop()
{
    QFETCH(bool, save);
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.service.requestStop({});
    HeldChecks held;
    h.firewallCheck = held.hook();
    h.open();
    QCOMPARE(held.done.size(), 1);
    QVERIFY(h.window->firewallCheckRunning());
    if (save) {
        h.rename(0, QStringLiteral("KAT500"));
        h.window->saveAndApply();
    } else {
        h.window->startService();
    }
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    QVERIFY2(h.window->serviceText().contains(QStringLiteral("Running")), qPrintable(h.window->serviceText()));
    h.rig.service.requestStop({});   // the bridge stops (exit 7) before the first check answers
    h.window->refreshStatus();
    QCOMPARE(held.done.size(), 2);   // the current episode is not blocked by the outstanding one
    held.done[0](IFirewallMonitor::Check::Mismatch, QStringLiteral("extra rule"));
    QVERIFY2(!h.window->serviceText().contains(QStringLiteral("stale")), qPrintable(h.window->serviceText()));
    QVERIFY(h.window->firewallCheckRunning());   // still waiting for the current one
    h.window->refreshStatus();
    QCOMPARE(held.done.size(), 2);
    held.done[1](IFirewallMonitor::Check::Mismatch, QStringLiteral("extra rule"));
    QVERIFY(!h.window->firewallCheckRunning());
    QVERIFY2(h.window->serviceText().contains(QStringLiteral("stale firewall rule detected")), qPrintable(h.window->serviceText()));
    h.window->refreshStatus();
    QCOMPARE(held.done.size(), 2);   // once per episode
}

// A Reload reads the evidence again: a check outstanding from before it is dropped and the reload checks again; a
// verdict already shown goes with the evidence it judged.
void tst_BridgeWindow::obsoleteFirewallVerdictDroppedAcrossReload()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.service.requestStop({});
    HeldChecks held;
    h.firewallCheck = held.hook();
    h.open();
    QCOMPARE(held.done.size(), 1);
    h.window->reload(true);
    QCOMPARE(held.done.size(), 2);
    held.done[0](IFirewallMonitor::Check::Mismatch, QStringLiteral("extra rule"));
    QVERIFY2(!h.window->serviceText().contains(QStringLiteral("stale")), qPrintable(h.window->serviceText()));
    QVERIFY(h.window->firewallCheckRunning());
    held.done[1](IFirewallMonitor::Check::Mismatch, QStringLiteral("extra rule"));
    QVERIFY(!h.window->firewallCheckRunning());
    QVERIFY2(h.window->serviceText().contains(QStringLiteral("stale firewall rule detected")), qPrintable(h.window->serviceText()));
    // The shown verdict belongs to its episode: the next reload clears it at once and checks again.
    h.window->reload(true);
    QCOMPARE(held.done.size(), 3);
    QVERIFY2(!h.window->serviceText().contains(QStringLiteral("stale")), qPrintable(h.window->serviceText()));
    held.done[2](IFirewallMonitor::Check::Match, {});
    QVERIFY2(!h.window->serviceText().contains(QStringLiteral("stale")), qPrintable(h.window->serviceText()));
}

// An operation that ends with newer edits in the window refreshes only the evidence (refreshEvidence): that is new
// evidence too, so a check outstanding from before the operation is dropped and the stopped bridge is checked again.
// (Discard saved changes leaves a stopped, activated bridge as it was; the edit typed while it runs is kept.)
void tst_BridgeWindow::obsoleteFirewallVerdictDroppedAfterAnOperationWithNewerEdits()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.service.requestStop({});
    h.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configB(), QStringLiteral("root"));
    HeldChecks held;
    h.firewallCheck = held.hook();
    h.open();
    QCOMPARE(held.done.size(), 1);
    h.asker.answers[QStringLiteral("discardPending")] = true;
    h.blockAt(QStringLiteral("detect"), std::chrono::seconds(0));
    h.window->discardPending();
    QVERIFY(h.gate->waitEntered());
    h.rename(0, QStringLiteral("KAT500"));   // typed while the discard runs: its end does not reload the editor
    h.gate->release();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    QVERIFY(h.window->editor().dirty());
    QCOMPARE(held.done.size(), 2);
    held.done[0](IFirewallMonitor::Check::Mismatch, QStringLiteral("extra rule"));
    QVERIFY2(!h.window->serviceText().contains(QStringLiteral("stale")), qPrintable(h.window->serviceText()));
    QVERIFY(h.window->firewallCheckRunning());
    held.done[1](IFirewallMonitor::Check::Match, {});
    QVERIFY(!h.window->firewallCheckRunning());
}

// No operation of the window's: the bridge is started elsewhere (SCM, systemd) and stops again (exit 7) while the
// first check is outstanding. Leaving the episode drops that check; the new stopped episode is checked.
void tst_BridgeWindow::obsoleteFirewallVerdictDroppedWhenTheBridgeRanMeanwhile()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.rig.service.requestStop({});
    HeldChecks held;
    h.firewallCheck = held.hook();
    h.open();
    QCOMPARE(held.done.size(), 1);
    h.rig.service.launchEngine();
    h.window->refreshStatus();
    QVERIFY2(h.window->serviceText().contains(QStringLiteral("Running")), qPrintable(h.window->serviceText()));
    h.rig.service.requestStop({});
    h.window->refreshStatus();
    QCOMPARE(held.done.size(), 2);
    held.done[0](IFirewallMonitor::Check::Mismatch, QStringLiteral("extra rule"));
    QVERIFY2(!h.window->serviceText().contains(QStringLiteral("stale")), qPrintable(h.window->serviceText()));
    held.done[1](IFirewallMonitor::Check::Mismatch, QStringLiteral("extra rule"));
    QVERIFY2(h.window->serviceText().contains(QStringLiteral("stale firewall rule detected")), qPrintable(h.window->serviceText()));
}

// ---------------------------------------------------------------------------------------------------
// v1.1 WS-17: the window's lifecycle
// ---------------------------------------------------------------------------------------------------

namespace {

// Acts on the next question the product's CanonAsker opens (a modal CanonWindow named asbDialog_<id>), from inside the
// event loop its exec() runs. `seen` gets its object name.
void onNextDialog(std::function<void(QDialog*)> act, QString* seen = nullptr)
{
    auto* t = new QTimer(qApp);
    t->setInterval(20);
    QObject::connect(t, &QTimer::timeout, qApp, [t, act, seen] {
        auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!d || !d->objectName().startsWith(QLatin1String("asbDialog_")) || !d->isVisible()) return;
        t->stop();
        t->deleteLater();
        if (seen) *seen = d->objectName();
        act(d);
    });
    t->start();
}

QString dialogText(QDialog* d)
{
    QStringList parts;
    for (const char* n : {"asbDialogText", "asbDialogAfter"})
        if (auto* l = d->findChild<QLabel*>(QLatin1String(n))) parts << l->text();
    return parts.join(QLatin1Char('\n'));
}

// The installed program is another build (WS-14: Installation::otherBuild).
void otherBuild(Rig& rig)
{
    rig.service.installation.state = InstallState::IncompleteInstall;
    rig.service.installation.otherBuild = true;
    rig.service.installation.installedSha256 = QStringLiteral("117eb29a0b1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f708192a3b4c5d6e7f8091a");
    rig.service.installation.detail = QStringLiteral("the installed program is a different build");
}

}  // namespace

// WS-17 §1: nothing installed -> Install the bridge service in the service row, Save & apply reads Install and apply,
// no Uninstall….
void tst_BridgeWindow::notInstalledShowsInstall()
{
    Harness h;
    h.open();
    QCOMPARE(h.window->serviceText(), QStringLiteral("The bridge service is not installed."));
    QPushButton* life = h.window->button(QStringLiteral("asbLifecycle"));
    QVERIFY(life && life->isVisible());
    QCOMPARE(life->text(), QStringLiteral("Install the bridge service"));
    QCOMPARE(h.window->button(QStringLiteral("asbSaveApply"))->text(), QStringLiteral("Install and apply"));
    QVERIFY(h.window->autoStartBox()->isChecked());   // an install starts automatically (v1.1 fix after WS-17)
    QVERIFY(h.window->autoStartBox()->isEnabled());   // and may be unticked: the button then reads Save
    QVERIFY(!h.window->button(QStringLiteral("asbUninstall"))->isVisible());
    QVERIFY(!h.window->button(QStringLiteral("asbRestartAdmin"))->isVisible());
}

// WS-17 §1–2: Install applies the editor's ports (the same submission, so INV-107 clears dirty) and starts the bridge.
void tst_BridgeWindow::installRunsEditorPorts()
{
    Harness h;
    h.open();
    h.window->addPort();
    QVERIFY(h.window->editor().dirty());
    const Submission s = h.window->editor().submission(true);
    h.asker.answers[QStringLiteral("install")] = true;
    QTest::mouseClick(h.window->button(QStringLiteral("asbLifecycle")), Qt::LeftButton);
    QCOMPARE(h.asker.asked, QStringList{QStringLiteral("install")});
    QVERIFY2(h.asker.questions.last().text.contains(QStringLiteral("Apps & features entry")), qPrintable(h.asker.questions.last().text));
    QCOMPARE(h.asker.questions.last().focus, QStringLiteral("cancel"));
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    QCOMPARE(committed(h.rig), s.bytes);
    QCOMPARE(h.rig.service.installation.state, InstallState::Current);
    QVERIFY(h.rig.service.engineRunning);
    QVERIFY(!h.window->editor().dirty());
    QVERIFY(!h.window->button(QStringLiteral("asbLifecycle"))->isVisible());
    QVERIFY(h.window->button(QStringLiteral("asbUninstall"))->isVisible());
}

// INV-35 before the lifecycle question: a port on every interface is asked about first.
void tst_BridgeWindow::installAsksExposureFirst()
{
    Harness h;
    h.open();
    h.window->addPort();
    h.window->editor().edit().ports[0].bindAddress = QStringLiteral("0.0.0.0");
    h.window->editor().edit().ports[0].allowedClients.clear();
    h.asker.answers[QStringLiteral("exposure")] = true;
    h.window->installService();
    QCOMPARE(h.asker.asked, (QStringList{QStringLiteral("exposure"), QStringLiteral("install")}));
    QVERIFY(!h.window->busy());   // Install itself was not answered: nothing ran
    h.asker.asked.clear();
    h.asker.answers[QStringLiteral("exposure")] = false;
    h.window->installService();
    QCOMPARE(h.asker.asked, QStringList{QStringLiteral("exposure")});   // cancelled there: no install question
}

void tst_BridgeWindow::installCancelChangesNothing()
{
    Harness h;
    h.open();
    h.window->addPort();
    h.rig.world.log.clear();
    h.window->installService();   // unanswered: Cancelled
    QCOMPARE(h.asker.asked, QStringList{QStringLiteral("install")});
    QVERIFY(!h.window->busy());
    QCOMPARE(h.mutations(), QStringList());
    QCOMPARE(h.rig.service.installation.state, InstallState::NotInstalled);
    QVERIFY(h.window->editor().dirty());
    QVERIFY2(h.window->footerText().contains(QStringLiteral("Cancelled; nothing was changed.")), qPrintable(h.window->footerText()));
}

// WS-17 §1 with WS-14's wording: another build installed -> Update to this build, and the row names both builds.
void tst_BridgeWindow::otherBuildShowsUpdate()
{
    Harness h;
    h.rig.installedRunning(configA());
    otherBuild(h.rig);
    h.rig.service.installation.installedVersion = QString();   // read, no stamp: v1.0
    h.open();
    QPushButton* life = h.window->button(QStringLiteral("asbLifecycle"));
    QVERIFY(life->isVisible());
    QCOMPARE(life->text(), QStringLiteral("Update to this build"));
    const QString row = h.window->serviceText();
    QVERIFY2(row.contains(QStringLiteral("Installed: an earlier build, before 1.1 (program sha256 117eb29a0b1c)")), qPrintable(row));
    QVERIFY2(row.contains(QStringLiteral("This window:")), qPrintable(row));
    QVERIFY(h.window->button(QStringLiteral("asbUninstall"))->isVisible());
    QVERIFY(h.window->button(QStringLiteral("asbUninstall"))->isEnabled());
}

// Update to this build: the update question (Update / Cancel), then this build's program replaces the installed one.
void tst_BridgeWindow::updateButtonRunsInstall()
{
    Harness h;
    h.rig.installedRunning(configA());
    otherBuild(h.rig);
    h.open();
    h.asker.answers[QStringLiteral("update")] = true;
    h.rig.world.log.clear();
    QTest::mouseClick(h.window->button(QStringLiteral("asbLifecycle")), Qt::LeftButton);
    QCOMPARE(h.asker.asked, QStringList{QStringLiteral("update")});
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    QVERIFY(h.rig.world.log.indexOf(QStringLiteral("service:installOrConfigure")) >= 0);
    QVERIFY(!h.rig.service.installation.otherBuild);
    QVERIFY(!h.window->button(QStringLiteral("asbLifecycle"))->isVisible());
}

// D67, INV-131: the build question has three answers; Esc and the close button are Cancel and submit nothing (v1.0:
// they meant "No, keep it" and applied the settings).
void tst_BridgeWindow::buildQuestionThreeWay_data()
{
    QTest::addColumn<QString>("answer");
    QTest::newRow("update") << QStringLiteral("update");
    QTest::newRow("keep") << QStringLiteral("keep");
    QTest::newRow("cancel-esc") << QStringLiteral("esc");
    QTest::newRow("cancel-close") << QStringLiteral("close");
}

void tst_BridgeWindow::buildQuestionThreeWay()
{
    QFETCH(QString, answer);
    const bool real = answer == QLatin1String("esc") || answer == QLatin1String("close");
    Harness h;
    h.realDialogs = real;
    h.rig.installedRunning(configA());
    otherBuild(h.rig);
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    h.rig.world.log.clear();
    QString seen, focus;
    QStringList buttons;
    if (real) {
        onNextDialog([&](QDialog* d) {
            for (QPushButton* b : d->findChildren<QPushButton*>())
                if (b->objectName().startsWith(QLatin1String("asbDialogChoice_"))) {
                    buttons << b->objectName().mid(16);
                    if (b->isDefault()) focus = b->objectName().mid(16);
                }
            if (answer == QLatin1String("esc")) QTest::keyClick(d, Qt::Key_Escape);
            else d->close();
        }, &seen);
    } else {
        h.asker.choices[QStringLiteral("build")] = answer;
    }
    h.window->saveAndApply();
    if (real) {
        QCOMPARE(seen, QStringLiteral("asbDialog_build"));
        QCOMPARE(buttons, (QStringList{QStringLiteral("cancel"), QStringLiteral("keep"), QStringLiteral("update")}));
        QCOMPARE(focus, QStringLiteral("keep"));   // Return on an unread question keeps the installed program
        QVERIFY(!h.window->busy());
        QTest::qWait(50);
        QVERIFY(h.finals.isEmpty());
        QCOMPARE(h.mutations(), QStringList());
        QCOMPARE(committed(h.rig), configA());
        QVERIFY(h.window->editor().dirty());
        QVERIFY(h.rig.service.installation.otherBuild);
        return;
    }
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    QVERIFY(!h.window->editor().dirty());
    QCOMPARE(h.rig.world.log.indexOf(QStringLiteral("service:installOrConfigure")) >= 0, answer == QLatin1String("update"));
    QCOMPARE(h.rig.service.installation.otherBuild, answer == QLatin1String("keep"));
}

// WS-17 §2 with WS-16's preview: the ports that go down, the connected client, what is removed, the settings box.
void tst_BridgeWindow::uninstallDialogNamesPortsAndClients()
{
    Harness h;
    h.rig.installedRunning(configA());
    StatusReport report;
    report.updated = QDateTime::currentDateTimeUtc();
    PortStatus port;
    port.tcpPort = 4001;
    report.ports << port;
    h.rig.service.status = report;
    h.rig.service.status->ports[0].state = BridgeState::Connected;
    h.rig.service.status->ports[0].client = QStringLiteral("192.0.2.10:50000");
    h.rig.service.status->updated = QDateTime::currentDateTimeUtc();
    h.open();
    h.window->uninstallService();
    QCOMPARE(h.asker.asked, QStringList{QStringLiteral("uninstall")});
    const Question q = h.asker.questions.last();
    QVERIFY2(q.text.contains(QStringLiteral("port 4001")), qPrintable(q.text));
    QVERIFY2(q.list.join(QLatin1Char('\n')).contains(QStringLiteral("192.0.2.10:50000 is connected and will be disconnected")),
             qPrintable(q.list.join(QLatin1Char('\n'))));
    QVERIFY2(q.after.contains(QStringLiteral("Apps & features entry")), qPrintable(q.after));
    QVERIFY2(q.checkbox.startsWith(QStringLiteral("Also remove my settings (the port list in ")), qPrintable(q.checkbox));
    QCOMPARE(q.focus, QStringLiteral("cancel"));
    QCOMPARE(q.destructive, QStringLiteral("uninstall"));
}

// "Also remove my settings" starts unticked every time the dialog opens, whatever it was the last time.
void tst_BridgeWindow::uninstallSettingsUncheckedEachTime()
{
    Harness h;
    h.realDialogs = true;
    h.rig.installedRunning(configA());
    h.open();
    QList<bool> uncheckedAtOpen;
    for (int round = 0; round < 2; ++round) {
        QString seen;
        onNextDialog([&](QDialog* d) {
            auto* box = d->findChild<QCheckBox*>(QStringLiteral("asbDialogCheck"));
            uncheckedAtOpen << (box && !box->isChecked());
            if (box) box->setChecked(true);   // ticked, then the dialog is cancelled
            QTest::mouseClick(d->findChild<QPushButton*>(QStringLiteral("asbDialogChoice_cancel")), Qt::LeftButton);
        }, &seen);
        h.window->uninstallService();
        QCOMPARE(seen, QStringLiteral("asbDialog_uninstall"));
    }
    QCOMPARE(uncheckedAtOpen, (QList<bool>{true, true}));
    QCOMPARE(h.rig.service.installation.state, InstallState::Current);
}

void tst_BridgeWindow::uninstallCancelChangesNothing()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    h.rig.world.log.clear();
    h.asker.tick = true;   // even with the box ticked
    h.window->uninstallService();   // unanswered: Cancelled
    QVERIFY(!h.window->busy());
    QCOMPARE(h.mutations(), QStringList());
    QCOMPARE(h.rig.service.installation.state, InstallState::Current);
    QCOMPARE(committed(h.rig), configA());
}

// R26: uninstalling a newer installed build with an older window says so.
void tst_BridgeWindow::uninstallNewerBuildWarns()
{
    Harness h;
    h.rig.installedRunning(configA());
    otherBuild(h.rig);
    h.rig.service.installation.installedVersion = QStringLiteral("99.0.0");
    h.open();
    h.window->uninstallService();
    const Question q = h.asker.questions.last();
    QVERIFY2(q.after.contains(QStringLiteral("newer build (99.0.0)")), qPrintable(q.after));
}

// The editor is never changed by an uninstall: dirty edits stay dirty.
void tst_BridgeWindow::uninstallKeepsDirtyEditor()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    h.asker.answers[QStringLiteral("uninstall")] = true;
    h.window->uninstallService();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    QCOMPARE(h.rig.service.installation.state, InstallState::NotInstalled);
    QVERIFY(h.window->editor().dirty());
    QCOMPARE(h.window->editor().config().ports[0].name, QStringLiteral("KAT500"));
    QVERIFY(h.window->button(QStringLiteral("asbLifecycle"))->isVisible());   // the service row was read again
}

// An uninstall that removed the service and kept the program ("uninstall again to finish") leaves the
// window's Uninstall… there, enabled; the retry asks to finish, removes the program and the button goes. Windows: the
// program's removal fails (D69, the service is gone first); Linux: the unit goes, then the program's removal fails.
// The elevated window's --intent=uninstall in that state opens the same question.
void tst_BridgeWindow::uninstallAgainFinishesAKeptProgram_data()
{
    QTest::addColumn<bool>("linux");
    QTest::addColumn<bool>("viaIntent");
    QTest::newRow("windows") << false << false;
    QTest::newRow("linux") << true << false;
    QTest::newRow("windows, --intent=uninstall") << false << true;
}

void tst_BridgeWindow::uninstallAgainFinishesAKeptProgram()
{
    QFETCH(bool, linux);
    QFETCH(bool, viaIntent);
    Harness h(linux ? Installation::Flavor::Linux : Installation::Flavor::Windows);
    Rig& rig = h.rig;
    rig.service.separateProgram = !linux;
    rig.installedRunning(configA());
    const QString why = QStringLiteral("the file is in use");
    rig.service.fail[linux ? QStringLiteral("uninstall!") : QStringLiteral("removeProgram!")] = why;
    h.open();
    h.asker.answers[QStringLiteral("uninstall")] = true;
    h.window->uninstallService();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.size(), 1);
    QVERIFY2(h.finals.last().outcome != Outcome::Ok, qPrintable(h.finals.last().message));
    QCOMPARE(rig.service.installation.state, InstallState::NotInstalled);
    QVERIFY(rig.service.programPresent);
    QPushButton* uninstall = h.window->button(QStringLiteral("asbUninstall"));
    QVERIFY2(uninstall->isVisible() && uninstall->isEnabled(), "the window keeps the way to finish");
    QVERIFY2(h.window->serviceText().contains(QStringLiteral("An uninstall did not finish")), qPrintable(h.window->serviceText()));
    rig.service.fail.clear();

    h.asker.asked.clear();
    h.asker.questions.clear();
    if (viaIntent) {
        // A new (elevated) window over the same machine, started with --intent=uninstall.
        h.window.reset();
        h.intent = QStringLiteral("uninstall");
        h.open();
        QTRY_COMPARE(h.asker.asked, QStringList{QStringLiteral("uninstall")});
    } else {
        QTest::mouseClick(uninstall, Qt::LeftButton);
        QCOMPARE(h.asker.asked, QStringList{QStringLiteral("uninstall")});
    }
    QVERIFY2(h.asker.questions.last().text.startsWith(QStringLiteral("Finish uninstalling?")),
             qPrintable(h.asker.questions.last().text));
    // The service is already gone: the second paragraph names only what is left.
    const QString after = h.asker.questions.last().after;
    QVERIFY2(!after.contains(QStringLiteral("removes the service")), qPrintable(after));
    QVERIFY2(after.startsWith(linux ? QStringLiteral("It removes the program left behind in /usr/local/bin, the polkit policy "
                                                     "and /var/lib/aether-serial-bridge. polkit will ask for an "
                                                     "administrator's password.")
                                    : QStringLiteral("It removes the program left behind and its folder C:\\Program "
                                                     "Files\\AetherSerialBridge (and the Apps & features entry, if any).")),
             qPrintable(after));
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    QVERIFY(!rig.service.programPresent);
    QVERIFY(!h.window->button(QStringLiteral("asbUninstall"))->isVisible());
    QCOMPARE(h.window->serviceText(), QStringLiteral("The bridge service is not installed."));
}

// The review's control: nothing installed and no program left: no Uninstall…, and the action refuses as before.
void tst_BridgeWindow::noUninstallOnACleanMachine()
{
    for (const bool linux : {false, true}) {
        Harness h(linux ? Installation::Flavor::Linux : Installation::Flavor::Windows);
        h.rig.service.separateProgram = !linux;
        h.open();
        QVERIFY(!h.window->button(QStringLiteral("asbUninstall"))->isVisible());
        h.rig.world.log.clear();
        h.window->uninstallService();
        QVERIFY(h.asker.asked.isEmpty());
        QVERIFY(!h.window->busy());
        QCOMPARE(h.window->footerText(), QStringLiteral("The bridge service is not installed; nothing was changed."));
        QCOMPARE(h.mutations(), QStringList());
    }
}

// After an uninstall (box unticked) the window shows Install again, with the kept settings in the editor.
void tst_BridgeWindow::afterUninstallShowsInstall()
{
    Harness h;
    h.rig.installedRunning(configA());
    h.open();
    h.asker.answers[QStringLiteral("uninstall")] = true;
    h.window->uninstallService();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    QCOMPARE(h.window->serviceText(), QStringLiteral("The bridge service is not installed."));
    QVERIFY(h.window->button(QStringLiteral("asbLifecycle"))->isVisible());
    QVERIFY(!h.window->button(QStringLiteral("asbUninstall"))->isVisible());
    QCOMPARE(h.window->editor().config().ports.size(), 1);   // the settings were kept
    QCOMPARE(committed(h.rig), configA());
}

// D64: Restart as administrator asks first when edits would be lost; Cancel relaunches nothing.
void tst_BridgeWindow::restartAsAdminAsksWhenDirty()
{
    Harness h;
    h.readOnly = true;
    QStringList relaunched;
    h.restartAsAdmin = [&](const QString& intent) {
        relaunched << intent;
        BridgeWindowOptions::Relaunch r;
        r.started = true;
        return r;
    };
    h.open();
    QPushButton* restart = h.window->button(QStringLiteral("asbRestartAdmin"));
    QVERIFY(restart && restart->isVisible() && restart->isEnabled());
    QCOMPARE(h.window->bannerText(), standardUserReason());
    h.window->editor().edit().ports.append(PortMapping{});   // unsaved (a read-only window cannot edit; the model can)
    QVERIFY(h.window->editor().dirty());
    h.window->restartAsAdmin();
    QCOMPARE(h.asker.asked, QStringList{QStringLiteral("restartAdmin")});
    QVERIFY(relaunched.isEmpty());
    QVERIFY(h.window->isVisible());
    h.asker.answers[QStringLiteral("restartAdmin")] = true;
    h.window->restartAsAdmin();
    QCOMPARE(relaunched, QStringList{QStringLiteral("install")});   // nothing installed: the elevated window offers Install
    QVERIFY(!h.window->isVisible());   // the elevated window takes over
}

// UAC answered No: "Cancelled; nothing was changed.", the window stays.
void tst_BridgeWindow::restartAsAdminCancelled()
{
    Harness h;
    h.readOnly = true;
    h.rig.installedRunning(configA());
    int calls = 0;
    h.restartAsAdmin = [&](const QString& intent) {
        ++calls;
        [&] { QCOMPARE(intent, QStringLiteral("none")); }();
        BridgeWindowOptions::Relaunch r;
        r.cancelled = true;
        return r;
    };
    h.open();
    QTest::mouseClick(h.window->button(QStringLiteral("asbRestartAdmin")), Qt::LeftButton);
    QCOMPARE(calls, 1);
    QVERIFY(h.asker.asked.isEmpty());   // nothing unsaved: no question
    QVERIFY(h.window->isVisible());
    QCOMPARE(h.window->footerText(), QStringLiteral("Cancelled; nothing was changed."));
}

// WS-17 §3: the elevated window's --intent opens that action's confirmation once loaded, once, and the user still
// confirms; a standard user's window ignores it.
void tst_BridgeWindow::intentOpensConfirmationOnly()
{
    {
        Harness h;
        h.intent = QStringLiteral("uninstall");
        h.rig.installedRunning(configA());
        h.open();
        QTRY_COMPARE(h.asker.asked, QStringList{QStringLiteral("uninstall")});
        QVERIFY(!h.window->busy());   // unanswered: nothing ran
        QCOMPARE(h.rig.service.installation.state, InstallState::Current);
        h.window->reload(true);
        QTest::qWait(50);
        QCOMPARE(h.asker.asked.size(), 1);   // once
    }
    {
        Harness h;
        h.intent = QStringLiteral("update");
        h.rig.installedRunning(configA());
        otherBuild(h.rig);
        h.open();
        QTRY_COMPARE(h.asker.asked, QStringList{QStringLiteral("update")});
    }
    {
        Harness h;
        h.readOnly = true;
        h.intent = QStringLiteral("uninstall");
        h.rig.installedRunning(configA());
        h.open();
        QTest::qWait(50);
        QVERIFY(h.asker.asked.isEmpty());
    }
}

// INV-129: an operation submitted while the window's own firewall check runs waits for it WITHOUT blocking the GUI
// thread: the click returns at once, the window says it waits, the event loop keeps turning (here it is the GUI thread
// that ends the check), and the operation starts when the check has ended.
void tst_BridgeWindow::actionDeferredWhileCheckRuns()
{
    std::mutex m;
    std::condition_variable cv;
    bool open = false;
    auto work = [&](const BridgeConfig&, const FirewallGeneration&, Deadline) {
        std::unique_lock lk(m);
        cv.wait_for(lk, std::chrono::milliseconds(1500), [&] { return open; });   // bounded either way
        return FirewallChecks::Verdict{IFirewallMonitor::Check::Match, QString()};
    };
    FirewallChecks checks(work, std::chrono::milliseconds(3000));
    Harness h;
    h.rig.installedRunning(configA());
    h.firewallIdle = [&](std::function<void()> cb) { return checks.whenIdle(std::move(cb)); };
    h.open();
    QVERIFY(checks.start(*fromJson(configA(), nullptr), FirewallGeneration{QByteArrayLiteral("0123456789abcdef")},
                         [](IFirewallMonitor::Check, const QString&) {}));
    QVERIFY(QTest::qWaitFor([&] { return checks.running() == 1; }));
    QElapsedTimer clock;
    clock.start();
    QList<qint64> beats;
    QTimer heartbeat;
    heartbeat.setInterval(20);
    connect(&heartbeat, &QTimer::timeout, this, [&] { beats << clock.elapsed(); });
    heartbeat.start();
    qint64 openedAt = -1;
    QTimer::singleShot(300, this, [&] {   // the GUI thread ends the check: only possible if it is not blocked
        openedAt = clock.elapsed();
        {
            std::lock_guard lk(m);
            open = true;
        }
        cv.notify_all();
    });
    h.rig.world.log.clear();
    h.window->stopService();
    const qint64 returnedAt = clock.elapsed();
    QVERIFY2(returnedAt < 250, qPrintable(QStringLiteral("the click returned after %1 ms").arg(returnedAt)));
    QVERIFY(h.window->waitingForFirewallCheck());
    QVERIFY(h.window->busy());
    QVERIFY2(h.window->footerText().startsWith(QStringLiteral("Waiting for the window's firewall check")), qPrintable(h.window->footerText()));
    QVERIFY(!h.rig.world.log.contains(QStringLiteral("applyLock:acquired")));
    QVERIFY(h.waitIdle());
    heartbeat.stop();
    QVERIFY(openedAt >= 0 && openedAt < 1000);
    QCOMPARE(h.finals.size(), 1);
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    qint64 maxGap = 0;
    for (qsizetype i = 1; i < beats.size(); ++i) maxGap = std::max(maxGap, beats[i] - beats[i - 1]);
    QVERIFY2(maxGap < 250, qPrintable(QStringLiteral("the GUI thread stood still for %1 ms").arg(maxGap)));
}

// INV-129: Cancel while waiting starts nothing, also when the check ends afterwards.
void tst_BridgeWindow::deferredActionCancelStartsNothing()
{
    Harness h;
    h.rig.installedRunning(configA());
    std::function<void()> whenIdle;
    h.firewallIdle = [&](std::function<void()> cb) {
        whenIdle = std::move(cb);
        return false;
    };
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    h.rig.world.log.clear();
    h.window->saveAndApply();
    QVERIFY(h.window->waitingForFirewallCheck());
    QPushButton* cancel = h.window->button(QStringLiteral("asbWaitCancel"));
    QVERIFY(cancel && cancel->isVisible());
    QTest::mouseClick(cancel, Qt::LeftButton);
    QVERIFY(!h.window->busy());
    QVERIFY(whenIdle);
    whenIdle();   // the check ends now
    QTest::qWait(50);
    QVERIFY(h.finals.isEmpty());
    QCOMPARE(h.mutations(), QStringList());
    QVERIFY(h.window->editor().dirty());
    QVERIFY2(h.window->footerText().contains(QStringLiteral("cancelled; nothing was started")), qPrintable(h.window->footerText()));
}

// INV-129: closing the window while an operation waits for the check starts nothing.
void tst_BridgeWindow::closeWhileWaitingStartsNothing()
{
    Harness h;
    h.rig.installedRunning(configA());
    std::function<void()> whenIdle;
    h.firewallIdle = [&](std::function<void()> cb) {
        whenIdle = std::move(cb);
        return false;
    };
    h.open();
    h.rig.world.log.clear();
    h.window->stopService();
    QVERIFY(h.window->waitingForFirewallCheck());
    QVERIFY(h.window->close());   // not refused: nothing has started
    QVERIFY(!h.window->isVisible());
    whenIdle();
    QTest::qWait(50);
    QVERIFY(h.finals.isEmpty());
    QCOMPARE(h.mutations(), QStringList());
    QVERIFY(h.rig.service.engineRunning);
}

// INV-129: Esc while a Save & apply waits for the window's firewall check asks the dirty question first.
// "Keep editing" keeps the window and the save it waits for: it runs when the check ends. "Discard" closes the window
// and cancels the wait, so nothing runs.
void tst_BridgeWindow::escWhileWaitingKeepsTheSave_data()
{
    QTest::addColumn<bool>("discard");
    QTest::newRow("keep editing") << false;
    QTest::newRow("discard") << true;
}

void tst_BridgeWindow::escWhileWaitingKeepsTheSave()
{
    QFETCH(bool, discard);
    Harness h;
    h.rig.installedRunning(configA());
    std::function<void()> whenIdle;
    h.firewallIdle = [&](std::function<void()> cb) {
        whenIdle = std::move(cb);
        return false;
    };
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    h.rig.world.log.clear();
    h.window->saveAndApply();
    QVERIFY(h.window->waitingForFirewallCheck());
    h.asker.answers[QStringLiteral("discardEdits")] = discard;
    h.window->reject();   // what Escape and Ctrl+W do (CanonWindow)
    QCOMPARE(h.asker.asked.last(), QStringLiteral("discardEdits"));
    QVERIFY2(h.asker.texts.last().contains(QStringLiteral("Saving and applying, waiting for the window's firewall check")),
             qPrintable(h.asker.texts.last()));
    QCOMPARE(h.window->isVisible(), !discard);
    QCOMPARE(h.window->waitingForFirewallCheck(), !discard);
    QVERIFY(whenIdle);
    whenIdle();   // the check ends now
    if (discard) {
        QTest::qWait(50);
        QVERIFY(h.finals.isEmpty());
        QCOMPARE(h.mutations(), QStringList());
        return;
    }
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.size(), 1);
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    QVERIFY(!h.window->editor().dirty());
    QCOMPARE(fromJson(committed(h.rig), nullptr)->ports[0].name, QStringLiteral("KAT500"));
}

// INV-129: a deferred operation starts on the evidence its questions were answered on, or not at all.
// When the check ends, the window reads the installation (and, for a save, the pending file) again; a change since the
// click starts nothing, says what changed, keeps the edits and shows the new evidence. "unchanged" is the control.
void tst_BridgeWindow::deferredOperationOnChangedEvidenceStartsNothing_data()
{
    QTest::addColumn<QString>("op");
    QTest::addColumn<QString>("change");
    QTest::newRow("save, unchanged") << QStringLiteral("save") << QStringLiteral("none");
    QTest::newRow("save, uninstalled elsewhere") << QStringLiteral("save") << QStringLiteral("uninstalled");
    QTest::newRow("save, another build installed") << QStringLiteral("save") << QStringLiteral("otherBuild");
    QTest::newRow("save, a pending file appeared") << QStringLiteral("save") << QStringLiteral("pending");
    QTest::newRow("uninstall, unchanged") << QStringLiteral("uninstall") << QStringLiteral("none");
    QTest::newRow("uninstall, another build installed") << QStringLiteral("uninstall") << QStringLiteral("otherBuild");
    QTest::newRow("stop, uninstalled elsewhere") << QStringLiteral("stop") << QStringLiteral("uninstalled");
}

void tst_BridgeWindow::deferredOperationOnChangedEvidenceStartsNothing()
{
    QFETCH(QString, op);
    QFETCH(QString, change);
    Harness h;
    h.rig.installedRunning(configA());
    std::function<void()> whenIdle;
    h.firewallIdle = [&](std::function<void()> cb) {
        whenIdle = std::move(cb);
        return false;
    };
    h.open();
    if (op == QLatin1String("save")) {
        h.rename(0, QStringLiteral("KAT500"));
        h.window->saveAndApply();
    } else if (op == QLatin1String("uninstall")) {
        h.asker.answers[QStringLiteral("uninstall")] = true;
        h.window->uninstallService();
    } else {
        h.window->stopService();
    }
    QVERIFY(h.window->waitingForFirewallCheck());
    // Meanwhile, elsewhere (the CLI, another window):
    if (change == QLatin1String("uninstalled")) {
        h.rig.service.installation.state = InstallState::NotInstalled;
        h.rig.service.engineRunning = false;
        h.rig.service.programPresent = false;
    } else if (change == QLatin1String("otherBuild")) {
        otherBuild(h.rig);
    } else if (change == QLatin1String("pending")) {
        h.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configC(), QStringLiteral("root"));
    }
    h.rig.world.log.clear();
    QVERIFY(whenIdle);
    whenIdle();   // the check ends now
    if (change == QLatin1String("none")) {
        QVERIFY(h.waitIdle());
        QCOMPARE(h.finals.size(), 1);
        QCOMPARE(h.finals.last().outcome, Outcome::Ok);
        return;
    }
    QTest::qWait(50);
    QVERIFY(!h.window->busy());
    QVERIFY(h.finals.isEmpty());
    QCOMPARE(h.mutations(), QStringList());
    const QString what = change == QLatin1String("pending")
                             ? QStringLiteral("the saved, not applied settings (config.pending.json) changed")
                             : QStringLiteral("the installation changed");
    QVERIFY2(h.window->footerText().contains(QStringLiteral("was not started: %1").arg(what)), qPrintable(h.window->footerText()));
    if (op == QLatin1String("save")) QVERIFY(h.window->editor().dirty());
    if (change == QLatin1String("uninstalled"))
        QCOMPARE(h.window->serviceText(), QStringLiteral("The bridge service is not installed."));   // the new evidence
}

// INV-129: the check ends while the close question is open. The product's dialog runs a nested event
// loop, and the check's whenIdle is delivered inside it (here: from inside the real dialog). Nothing starts behind the
// question, in every row. Discard cancels the wait: nothing starts or writes, and the window closes. Keep editing (the
// button, or Esc on the question) resumes the save once the question has closed, on the evidence read again then
//: an installation changed while the question was open starts nothing.
void tst_BridgeWindow::checkEndsWhileTheCloseQuestionIsOpen_data()
{
    QTest::addColumn<QString>("trigger");   // esc: reject() (Escape, Ctrl+W); close: the close button (closeEvent)
    QTest::addColumn<QString>("answer");    // yes = Discard; no = Keep editing; esc = Esc on the question
    QTest::addColumn<bool>("uninstalledMeanwhile");
    QTest::newRow("esc, discard") << QStringLiteral("esc") << QStringLiteral("yes") << false;
    QTest::newRow("esc, keep editing") << QStringLiteral("esc") << QStringLiteral("no") << false;
    QTest::newRow("esc, esc on the question") << QStringLiteral("esc") << QStringLiteral("esc") << false;
    QTest::newRow("close button, discard") << QStringLiteral("close") << QStringLiteral("yes") << false;
    QTest::newRow("close button, keep editing") << QStringLiteral("close") << QStringLiteral("no") << false;
    QTest::newRow("esc, keep editing, uninstalled meanwhile") << QStringLiteral("esc") << QStringLiteral("no") << true;
}

void tst_BridgeWindow::checkEndsWhileTheCloseQuestionIsOpen()
{
    QFETCH(QString, trigger);
    QFETCH(QString, answer);
    QFETCH(bool, uninstalledMeanwhile);
    Harness h;
    h.realDialogs = true;
    h.rig.installedRunning(configA());
    std::function<void()> whenIdle;
    h.firewallIdle = [&](std::function<void()> cb) {
        whenIdle = std::move(cb);
        return false;
    };
    h.open();
    h.rename(0, QStringLiteral("KAT500"));
    h.window->saveAndApply();
    QVERIFY(h.window->waitingForFirewallCheck());
    QVERIFY(whenIdle);
    h.rig.world.log.clear();
    // Snapshots only inside the dialog (a failed check there would leave the dialog open): asserted after it closes.
    bool startedBehind = true;
    bool waitingBehind = false;
    QStringList writesBehind{QStringLiteral("not reached")};
    QString seen;
    QString text;
    onNextDialog([&](QDialog* d) {
        text = dialogText(d);
        whenIdle();   // the check ends now, inside the question's event loop
        QCoreApplication::processEvents();
        startedBehind = h.actions->busy();
        waitingBehind = h.window->waitingForFirewallCheck();
        writesBehind = h.mutations();
        if (uninstalledMeanwhile) {   // elsewhere (the CLI, another window), while the question is open
            h.rig.service.installation.state = InstallState::NotInstalled;
            h.rig.service.engineRunning = false;
            h.rig.service.programPresent = false;
        }
        if (answer == QLatin1String("esc")) QTest::keyClick(d, Qt::Key_Escape);
        else QTest::mouseClick(d->findChild<QPushButton*>(QStringLiteral("asbDialogChoice_") + answer), Qt::LeftButton);
    }, &seen);
    if (trigger == QLatin1String("esc")) h.window->reject();
    else h.window->close();
    QCOMPARE(seen, QStringLiteral("asbDialog_discardEdits"));
    QVERIFY2(text.contains(QStringLiteral("Saving and applying, waiting for the window's firewall check, does not start")),
             qPrintable(text));
    QVERIFY2(!startedBehind, "the save started behind the open question");
    QVERIFY(waitingBehind);
    QCOMPARE(writesBehind, QStringList());
    if (answer == QLatin1String("yes")) {
        QVERIFY2(!h.actions->busy(), "the window closed while the save ran");
        QVERIFY(!h.window->isVisible());
        QVERIFY(!h.window->waitingForFirewallCheck());
        QTest::qWait(100);
        QVERIFY(!h.actions->busy());
        QVERIFY(h.finals.isEmpty());
        QCOMPARE(h.mutations(), QStringList());
        return;
    }
    QVERIFY(h.window->isVisible());
    if (uninstalledMeanwhile) {
        QTRY_VERIFY(!h.window->waitingForFirewallCheck());
        QTest::qWait(50);
        QVERIFY(!h.window->busy());
        QVERIFY(h.finals.isEmpty());
        QCOMPARE(h.mutations(), QStringList());
        QVERIFY2(h.window->footerText().contains(QStringLiteral("was not started: the installation changed")),
                 qPrintable(h.window->footerText()));
        QVERIFY(h.window->editor().dirty());
        return;
    }
    QTRY_VERIFY_WITH_TIMEOUT(h.finals.size() == 1, 20000);   // resumed once the question closed
    QCOMPARE(h.finals.last().outcome, Outcome::Ok);
    QVERIFY(!h.window->editor().dirty());
    QCOMPARE(fromJson(committed(h.rig), nullptr)->ports[0].name, QStringLiteral("KAT500"));
}

// Owner debt (WS-17 §2): 40 exposed ports: the sentence names the count and the first two, the list is bounded (at
// most 8 rows visible) and scrolls.
void tst_BridgeWindow::exposureDialogBoundedWith40Ports()
{
    Harness h;
    h.realDialogs = true;
    h.rig.installedRunning(configA());
    h.open();
    BridgeConfig& c = h.window->editor().edit();
    c.ports.clear();
    for (int i = 0; i < 40; ++i) {
        PortMapping p;
        p.name = QStringLiteral("Port%1").arg(i);
        p.comPort = QStringLiteral("COM%1").arg(10 + i);
        p.tcpPort = 5000 + i;
        p.bindAddress = QStringLiteral("0.0.0.0");
        c.ports << p;
    }
    QString seen, text;
    int rows = -1, visibleRows = 99;
    onNextDialog([&](QDialog* d) {
        text = dialogText(d);
        if (auto* list = d->findChild<QListWidget*>(QStringLiteral("asbDialogList"))) {
            rows = list->count();
            const int rowHeight = std::max(1, list->sizeHintForRow(0));
            visibleRows = list->height() / rowHeight;
        }
        QTest::keyClick(d, Qt::Key_Escape);
    }, &seen);
    h.window->saveAndApply();
    QCOMPARE(seen, QStringLiteral("asbDialog_exposure"));
    QVERIFY2(text.startsWith(QStringLiteral("40 ports (Port0, Port1 and 38 more) will listen on every network interface")), qPrintable(text));
    QCOMPARE(rows, 40);
    QVERIFY2(visibleRows <= Question::kListRows, qPrintable(QString::number(visibleRows)));
    QVERIFY(!h.window->busy());   // Esc: nothing saved
}

// INV-131: every lifecycle choice can be cancelled with no effect. Each row opens the product's dialog and leaves it the
// way a user would without reading it: Return (the focus is always a safe choice) or the close button.
void tst_BridgeWindow::lifecycleCancelChangesNothing_data()
{
    QTest::addColumn<QString>("what");
    QTest::newRow("install") << QStringLiteral("install");
    QTest::newRow("update") << QStringLiteral("update");
    QTest::newRow("uninstall") << QStringLiteral("uninstall");
    QTest::newRow("build") << QStringLiteral("build");
    QTest::newRow("restart") << QStringLiteral("restart");
}

void tst_BridgeWindow::lifecycleCancelChangesNothing()
{
    QFETCH(QString, what);
    Harness h;
    h.realDialogs = true;
    int relaunches = 0;
    if (what == QLatin1String("restart")) {
        h.readOnly = true;
        h.restartAsAdmin = [&](const QString&) {
            ++relaunches;
            BridgeWindowOptions::Relaunch r;
            r.started = true;
            return r;
        };
    }
    if (what != QLatin1String("install")) h.rig.installedRunning(configA());
    if (what == QLatin1String("update") || what == QLatin1String("build")) otherBuild(h.rig);
    h.open();
    if (what == QLatin1String("install")) h.window->addPort();
    if (what == QLatin1String("build")) h.rename(0, QStringLiteral("KAT500"));
    if (what == QLatin1String("restart")) h.window->editor().edit().ports.append(PortMapping{});
    const bool dirty = h.window->editor().dirty();
    const InstallState before = h.rig.service.installation.state;
    const QByteArray committedBefore = committed(h.rig);
    h.rig.world.log.clear();
    QString seen;
    onNextDialog([&](QDialog* d) {
        if (what == QLatin1String("build")) d->close();
        else QTest::keyClick(d, Qt::Key_Return);   // Return on an unread question
    }, &seen);
    if (what == QLatin1String("install")) h.window->installService();
    else if (what == QLatin1String("update")) h.window->updateService();
    else if (what == QLatin1String("uninstall")) h.window->uninstallService();
    else if (what == QLatin1String("build")) h.window->saveAndApply();
    else h.window->restartAsAdmin();
    const QString expectedDialog = what == QLatin1String("restart") ? QStringLiteral("restartAdmin") : what;
    QCOMPARE(seen, QStringLiteral("asbDialog_") + expectedDialog);
    QVERIFY(!h.window->busy());
    QTest::qWait(50);
    QVERIFY(h.finals.isEmpty());
    QCOMPARE(h.mutations(), QStringList());
    QCOMPARE(h.rig.service.installation.state, before);
    QCOMPARE(committed(h.rig), committedBefore);
    QCOMPARE(h.window->editor().dirty(), dirty);
    QCOMPARE(relaunches, 0);
    QVERIFY(h.window->isVisible());
}

// A standard user's window: the lifecycle buttons are shown but disabled with the read-only reason, and calling them
// does nothing; Restart as administrator appears only where the window can relaunch (Windows).
void tst_BridgeWindow::readOnlyLifecycleRefused()
{
    Harness h;
    h.readOnly = true;
    h.rig.installedRunning(configA());
    h.open();
    QPushButton* uninstall = h.window->button(QStringLiteral("asbUninstall"));
    QVERIFY(uninstall->isVisible());
    QVERIFY(!uninstall->isEnabled());
    QCOMPARE(uninstall->toolTip(), standardUserReason());
    QVERIFY(!h.window->button(QStringLiteral("asbRestartAdmin"))->isVisible());   // no relaunch hook: not offered
    h.rig.world.log.clear();
    h.window->uninstallService();
    h.window->installService();
    QVERIFY(h.asker.asked.isEmpty());
    QCOMPARE(h.mutations(), QStringList());
}

// v1.1 fix after WS-17: with nothing installed the main button read "Install and apply" while the box
// loaded unticked, so its click took the untick route (save-and-remove on Windows): the edits were saved, not applied,
// and nothing was installed. Now the box loads ticked (an install starts automatically) and the label follows it:
// ticked, "Install and apply" is Install the bridge service with the editor's ports; unticked, "Save" saves only.
void tst_BridgeWindow::mainButtonOnNothingInstalled_data()
{
    QTest::addColumn<bool>("ticked");
    QTest::newRow("box ticked: Install and apply installs") << true;
    QTest::newRow("box unticked: Save saves only") << false;
}

void tst_BridgeWindow::mainButtonOnNothingInstalled()
{
    QFETCH(bool, ticked);
    Harness h;
    h.open();
    QPushButton* save = h.window->button(QStringLiteral("asbSaveApply"));
    QCheckBox* box = h.window->autoStartBox();
    QVERIFY(box->isChecked());
    QCOMPARE(box->text(), QStringLiteral("Start automatically with Windows"));
    h.window->addPort();
    if (!ticked) box->setChecked(false);
    QCOMPARE(save->text(), ticked ? QStringLiteral("Install and apply") : QStringLiteral("Save"));
    QCOMPARE(h.window->footerText(), ticked ? QStringLiteral("Unsaved changes. Click Install and apply.")
                                            : QStringLiteral("Unsaved changes. Click Save."));
    QVERIFY(save->isEnabled());
    const Submission s = h.window->editor().submission(ticked);
    h.asker.answers[QStringLiteral("install")] = true;
    h.asker.answers[QStringLiteral("untick")] = true;   // would let a wrong route through; asked only when installed
    h.rig.world.log.clear();
    QTest::mouseClick(save, Qt::LeftButton);
    QVERIFY(h.waitIdle());
    QCOMPARE(h.finals.size(), 1);
    if (ticked) {
        // Install the bridge service, its question first (what is installed, and that the bridge starts).
        QCOMPARE(h.asker.asked, QStringList{QStringLiteral("install")});
        QVERIFY2(h.asker.questions.last().text.contains(QStringLiteral("Apps & features entry")), qPrintable(h.asker.questions.last().text));
        QVERIFY2(h.finals.last().outcome == Outcome::Ok, qPrintable(h.finals.last().message));
        QCOMPARE(committed(h.rig), s.bytes);   // the editor's ports
        QCOMPARE(h.rig.service.installation.state, InstallState::Current);
        QVERIFY(h.rig.service.engineRunning);
        QVERIFY(!h.window->editor().dirty());
        QVERIFY(box->isChecked());
        QCOMPARE(save->text(), QStringLiteral("Save && apply"));
        return;
    }
    // Save only: no question, the settings saved, not applied, and nothing else changed: no service and no firewall
    // call (save-and-remove would connect to the firewall and remove its rules).
    QVERIFY2(h.asker.asked.isEmpty(), qPrintable(h.asker.asked.join(QLatin1Char(','))));
    QVERIFY2(h.finals.last().outcome == Outcome::SavedNotApplied, qPrintable(h.finals.last().message));
    QCOMPARE(h.finals.last().persisted, Persisted::Staged);
    QCOMPARE(pending(h.rig), s.bytes);
    QVERIFY(!h.rig.has(QString::fromLatin1(ConfigFiles::kConfig)));
    QCOMPARE(h.rig.service.installation.state, InstallState::NotInstalled);
    for (const QString& e : h.mutations())
        QVERIFY2(!e.startsWith(QLatin1String("firewall:")) && !e.startsWith(QLatin1String("service:")), qPrintable(e));
    QVERIFY2(h.window->footerText().contains(QStringLiteral("saved but not applied")), qPrintable(h.window->footerText()));
    QVERIFY2(h.window->footerText().contains(QStringLiteral("not installed")), qPrintable(h.window->footerText()));
    QVERIFY(!h.window->editor().dirty());
    // Reloaded: the saved settings, and the box again shows what an install would set.
    QVERIFY(h.window->pendingNoteText().contains(QStringLiteral("Saved, not applied")));
    QVERIFY(box->isChecked());
    QCOMPARE(save->text(), QStringLiteral("Install and apply"));
}

// A standard user's window (WS-17 §3): with nothing installed the box shows ticked and the disabled main button reads
// Install and apply, which is what Restart as administrator's intent opens in the elevated window.
void tst_BridgeWindow::mainButtonStandardUserIntent()
{
    Harness h;
    h.readOnly = true;
    QStringList relaunched;
    h.restartAsAdmin = [&](const QString& intent) {
        relaunched << intent;
        BridgeWindowOptions::Relaunch r;
        r.started = true;
        return r;
    };
    h.open();
    QPushButton* save = h.window->button(QStringLiteral("asbSaveApply"));
    QCOMPARE(save->text(), QStringLiteral("Install and apply"));
    QVERIFY(!save->isEnabled());
    QVERIFY(h.window->autoStartBox()->isChecked());
    QVERIFY(!h.window->autoStartBox()->isEnabled());
    QTest::mouseClick(h.window->button(QStringLiteral("asbRestartAdmin")), Qt::LeftButton);
    QCOMPARE(relaunched, QStringList{QStringLiteral("install")});
}

// The route is decided on a fresh read, and it must be the one the label named: an installation that appeared since
// the window last read it saves nothing (Save would otherwise have become the untick route and removed the service).
void tst_BridgeWindow::mainButtonRefusesWhenTheInstallationChanged()
{
    Harness h;
    h.open();
    h.window->addPort();
    h.window->autoStartBox()->setChecked(false);
    QCOMPARE(h.window->button(QStringLiteral("asbSaveApply"))->text(), QStringLiteral("Save"));
    // Meanwhile, elsewhere (the CLI, another window):
    h.rig.installedRunning(configA());
    h.asker.answers[QStringLiteral("untick")] = true;
    h.rig.world.log.clear();
    h.window->saveAndApply();
    QVERIFY(!h.window->busy());
    QVERIFY(h.finals.isEmpty());
    QVERIFY2(h.asker.asked.isEmpty(), qPrintable(h.asker.asked.join(QLatin1Char(','))));
    QCOMPARE(h.mutations(), QStringList());
    QVERIFY(h.rig.service.engineRunning);
    QVERIFY2(h.window->footerText().startsWith(QStringLiteral("Nothing was saved: the installation changed")),
             qPrintable(h.window->footerText()));
    QVERIFY(h.window->editor().dirty());
    QCOMPARE(h.window->button(QStringLiteral("asbSaveApply"))->text(), QStringLiteral("Save && apply"));
}

// The window checks the installation before its questions; one installed while the replacement question
// is open (the CLI, another window) reaches the transaction. Save only refuses it there, under apply.lock: nothing is
// written (the pending file is still the one written elsewhere), the new service is not stopped and its starts are not
// blocked, and the window shows the installation with the edits kept. Kills: Applier::saveOnly without its precondition.
void tst_BridgeWindow::saveOnlyRefusedWhenInstalledDuringTheQuestion()
{
    Harness h;
    h.open();
    h.window->addPort();
    h.window->autoStartBox()->setChecked(false);
    QCOMPARE(h.window->button(QStringLiteral("asbSaveApply"))->text(), QStringLiteral("Save"));
    const QByteArray edits = h.window->editor().submission(false).bytes;
    // Meanwhile the CLI stages config C, so the click asks before replacing it.
    h.rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kPending), configC(), QStringLiteral("root"));
    h.asker.answers[QStringLiteral("supersede")] = true;
    h.asker.answers[QStringLiteral("untick")] = true;   // would let the untick route through; never asked here
    h.asker.onQuestion = [&](const QString& id) {
        if (id != QLatin1String("supersede")) return;
        h.rig.installedRunning(configA());   // while the question is open, elsewhere
        h.rig.world.log.clear();
    };
    QTest::mouseClick(h.window->button(QStringLiteral("asbSaveApply")), Qt::LeftButton);
    QVERIFY(h.waitIdle());
    QCOMPARE(h.asker.asked, QStringList{QStringLiteral("supersede")});
    QCOMPARE(h.finals.size(), 1);
    QVERIFY2(h.finals.last().outcome == Outcome::NothingChanged, qPrintable(h.finals.last().message));
    QCOMPARE(h.finals.last().persisted, Persisted::None);
    QCOMPARE(pending(h.rig), configC());
    QCOMPARE(committed(h.rig), configA());
    QVERIFY(h.rig.service.engineRunning);
    QVERIFY(!h.rig.service.startsBlocked);
    for (const QString& e : h.mutations())
        QVERIFY2(!e.startsWith(QLatin1String("firewall:")) && !e.startsWith(QLatin1String("service:")) &&
                     !e.startsWith(QLatin1String("io:")),
                 qPrintable(e));
    QVERIFY2(h.window->footerText().startsWith(QStringLiteral("Nothing was saved: the installation changed")),
             qPrintable(h.window->footerText()));
    QVERIFY(h.window->editor().dirty());
    QCOMPARE(h.window->editor().submission(false).bytes, edits);
    QCOMPARE(h.window->button(QStringLiteral("asbSaveApply"))->text(), QStringLiteral("Save && apply"));
    QVERIFY(!h.window->button(QStringLiteral("asbLifecycle"))->isVisible());
}

// Nothing installed, the box ticked, and the bridge installed elsewhere between the load and the click.
// The main button (Install and apply) gets call 27's refusal; the Install button says it is already installed. Either
// way the window then shows the installation: the Install button goes, the main button reads Save & apply on it, and
// the edits stay. Kills: the main button's expected not forwarded (the "already installed" footer instead of call 27's),
// and installService's "already installed" return without the refresh (the window kept saying Install).
void tst_BridgeWindow::installAfterInstalledElsewhereShowsIt_data()
{
    QTest::addColumn<bool>("mainButton");
    QTest::newRow("main button") << true;
    QTest::newRow("Install button") << false;
}

void tst_BridgeWindow::installAfterInstalledElsewhereShowsIt()
{
    QFETCH(bool, mainButton);
    Harness h;
    h.open();
    h.window->addPort();
    QPushButton* save = h.window->button(QStringLiteral("asbSaveApply"));
    QPushButton* life = h.window->button(QStringLiteral("asbLifecycle"));
    QCOMPARE(save->text(), QStringLiteral("Install and apply"));
    QVERIFY(life->isVisible());
    const QByteArray edits = h.window->editor().submission(true).bytes;
    // Meanwhile, elsewhere (the CLI, another window):
    h.rig.installedRunning(configA());
    h.asker.answers[QStringLiteral("install")] = true;
    h.asker.answers[QStringLiteral("update")] = true;
    h.rig.world.log.clear();
    QTest::mouseClick(mainButton ? save : life, Qt::LeftButton);
    QVERIFY(!h.window->busy());
    QVERIFY(h.finals.isEmpty());
    QVERIFY2(h.asker.asked.isEmpty(), qPrintable(h.asker.asked.join(QLatin1Char(','))));
    QCOMPARE(h.mutations(), QStringList());
    if (mainButton)
        QCOMPARE(h.window->footerText(), QStringLiteral("Nothing was saved: the installation changed since the window last "
                                                        "read it, so the button now reads Save & apply. Check what the window "
                                                        "shows and click again."));
    else
        QCOMPARE(h.window->footerText(), QStringLiteral("The bridge service is already installed; nothing was changed."));
    QCOMPARE(save->text(), QStringLiteral("Save && apply"));
    QVERIFY(!life->isVisible());
    QVERIFY(h.window->editor().dirty());
    QCOMPARE(h.window->editor().submission(true).bytes, edits);
}

// The click's checks pass on "nothing installed"; while the install question is open, another
// build is installed elsewhere (unfinished, so Applier::run would otherwise install over it without the build question,
// D67). The request carries the installation the questions were decided on, and the transaction refuses under apply.lock:
// nothing written, the other build's program kept and running, and the window shows it (Update to this build) with the
// edits kept. Both buttons. Kills: Applier::run without the install basis check.
void tst_BridgeWindow::anotherBuildDuringTheInstallQuestionIsRefused_data()
{
    QTest::addColumn<bool>("mainButton");
    QTest::newRow("main button") << true;
    QTest::newRow("Install button") << false;
}

void tst_BridgeWindow::anotherBuildDuringTheInstallQuestionIsRefused()
{
    QFETCH(bool, mainButton);
    Harness h;
    h.open();
    h.window->addPort();
    QPushButton* save = h.window->button(QStringLiteral("asbSaveApply"));
    QPushButton* life = h.window->button(QStringLiteral("asbLifecycle"));
    QCOMPARE(save->text(), QStringLiteral("Install and apply"));
    const QByteArray edits = h.window->editor().submission(true).bytes;
    h.asker.answers[QStringLiteral("install")] = true;
    h.asker.onQuestion = [&](const QString& id) {
        if (id != QLatin1String("install")) return;
        h.rig.installedRunning(configA());   // while the question is open, elsewhere: another build, not finished
        otherBuild(h.rig);
        h.rig.world.log.clear();
    };
    const QString otherSha = QStringLiteral("117eb29a0b1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f708192a3b4c5d6e7f8091a");
    QTest::mouseClick(mainButton ? save : life, Qt::LeftButton);
    QVERIFY(h.waitIdle());
    QCOMPARE(h.asker.asked, QStringList{QStringLiteral("install")});
    QCOMPARE(h.finals.size(), 1);
    QVERIFY2(h.finals.last().outcome == Outcome::NothingChanged, qPrintable(h.finals.last().message));
    QCOMPARE(h.finals.last().persisted, Persisted::None);
    QCOMPARE(h.rig.service.installation.state, InstallState::IncompleteInstall);
    QVERIFY(h.rig.service.installation.otherBuild);
    QCOMPARE(h.rig.service.installation.installedSha256, otherSha);
    QCOMPARE(committed(h.rig), configA());
    QVERIFY(!h.rig.has(QString::fromLatin1(ConfigFiles::kPending)));
    QVERIFY(h.rig.service.engineRunning);
    QVERIFY(!h.rig.service.startsBlocked);
    for (const QString& e : h.mutations())
        QVERIFY2(!e.startsWith(QLatin1String("firewall:")) && !e.startsWith(QLatin1String("service:")) &&
                     !e.startsWith(QLatin1String("io:")),
                 qPrintable(e));
    QVERIFY2(h.window->footerText().startsWith(QStringLiteral("Nothing was saved: the installation changed")),
             qPrintable(h.window->footerText()));
    QVERIFY(h.window->editor().dirty());
    QCOMPARE(h.window->editor().submission(true).bytes, edits);
    QCOMPARE(save->text(), QStringLiteral("Save && apply"));
    QVERIFY(life->isVisible());
    QCOMPARE(life->text(), QStringLiteral("Update to this build"));
}

// The invariant, for every install state x box x OS: the main button's label names the route its click takes.
// "Install and apply" -> install; "Save" -> save only (or, disabled, nothing); "Save & apply" -> run (box ticked) or
// the untick route (unticked). Every question is answered yes (the build question: No, keep it). The rows' labels are
// checked as well, so a label that ignores the box fails here even where its route would match.
void tst_BridgeWindow::mainButtonLabelNamesTheRoute_data()
{
    QTest::addColumn<bool>("linux");
    QTest::addColumn<QString>("state");
    QTest::addColumn<bool>("ticked");
    QTest::addColumn<QString>("label");
    QTest::addColumn<bool>("enabled");
    for (const bool linux : {false, true}) {
        for (const QString& state : {QStringLiteral("nothing installed"), QStringLiteral("program kept"),
                                     QStringLiteral("unfinished"), QStringLiteral("other build"), QStringLiteral("current")}) {
            for (const bool ticked : {true, false}) {
                const bool nothing = state == QLatin1String("nothing installed") || state == QLatin1String("program kept");
                const QString label = !nothing ? QStringLiteral("Save && apply")
                                      : ticked ? QStringLiteral("Install and apply")
                                               : QStringLiteral("Save");
                const bool enabled = !(nothing && !ticked && linux);
                const QString tag = QStringLiteral("%1, %2, box %3").arg(linux ? QStringLiteral("linux") : QStringLiteral("windows"),
                                                                         state, ticked ? QStringLiteral("ticked") : QStringLiteral("unticked"));
                QTest::newRow(qPrintable(tag)) << linux << state << ticked << label << enabled;
            }
        }
    }
}

void tst_BridgeWindow::mainButtonLabelNamesTheRoute()
{
    QFETCH(bool, linux);
    QFETCH(QString, state);
    QFETCH(bool, ticked);
    QFETCH(QString, label);
    QFETCH(bool, enabled);
    Rig rig;
    rig.world.checkLock = false;
    rig.service.installation.flavor = linux ? Installation::Flavor::Linux : Installation::Flavor::Windows;
    if (state == QLatin1String("program kept")) {
        rig.service.programPresent = true;
    } else if (state != QLatin1String("nothing installed")) {
        rig.installedRunning(configA());
        if (state == QLatin1String("unfinished")) {
            rig.service.installation.state = InstallState::IncompleteInstall;
            rig.service.installation.detail = QStringLiteral("a lock file is missing");
        } else if (state == QLatin1String("other build")) {
            otherBuild(rig);
        }
    }
    PlatformViewSource::Hooks hooks;
    hooks.autoStart = [] { return std::optional<bool>(true); };
    hooks.serialPorts = [] { return QStringList{QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5")}; };
    PlatformViewSource view(*rig.platform, hooks, rig.world.clock);
    ScriptedActions actions;
    actions.fl = rig.service.installation.flavor;
    actions.canSaveOnly = !linux;   // the Linux window's HelperActions has no route that saves without installing
    actions.result.outcome = Outcome::Ok;
    ScriptedAsker asker;
    for (const char* id : {"exposure", "untick", "install", "update", "supersede"}) asker.answers[QString::fromLatin1(id)] = true;
    asker.choices[QStringLiteral("build")] = QStringLiteral("keep");
    BridgeWindow::Options o;
    o.detailsExpanded = true;
    o.refreshMs = 0;
    o.linuxWords = linux;
    BridgeWindow w(view, actions, &asker, o);
    w.show();
    if (w.editor().config().ports.isEmpty()) w.addPort();
    w.autoStartBox()->setChecked(ticked);   // programmatically: the Linux window holds it ticked with nothing installed
    QPushButton* save = w.button(QStringLiteral("asbSaveApply"));
    const QString shown = save->text();
    const bool clickable = save->isEnabled();
    QCOMPARE(shown, label);
    QCOMPARE(clickable, enabled);
    w.saveAndApply();
    QTRY_VERIFY(!w.busy());
    const QMap<QString, QString> named = {{QStringLiteral("Install and apply"), QStringLiteral("install")},
                                          {QStringLiteral("Save"), QStringLiteral("saveOnly")},
                                          {QStringLiteral("Save && apply"), ticked ? QStringLiteral("run") : QStringLiteral("untick")}};
    const QStringList expected = clickable ? QStringList{named.value(shown)} : QStringList();
    QVERIFY2(actions.routes == expected, qPrintable(QStringLiteral("label \"%1\" took [%2]; asked [%3]; footer: %4")
                                                         .arg(shown, actions.routes.join(QLatin1Char(',')),
                                                              asker.asked.join(QLatin1Char(',')), w.footerText())));
}

#ifndef _WIN32
// INV-124 on the window: no polkit agent -> the message names the terminal command, which the window offers to copy.
void tst_BridgeWindow::linuxNoAgentShowsTerminalCommand()
{
    HelperHarness h(cli::HelperClient::kWaitDeadline, stagedProgram());
    h.open();
    h.window->addPort();
    h.window->editor().edit().ports[0].comPort = QStringLiteral("/dev/ttyUSB0");   // no device list in this harness
    h.window->editor().edit().ports[0].name = QStringLiteral("Amp");
    h.asker.answers[QStringLiteral("install")] = true;
    cli::IHelperProcess::Exit noAgent;
    noAgent.code = 127;
    noAgent.err = QByteArrayLiteral("Error creating textual authentication agent: Error opening current controlling terminal");
    h.proc.replaceWith = noAgent;
    QTRY_VERIFY(h.window->button(QStringLiteral("asbLifecycle"))->isEnabled());
    h.window->installService();
    QVERIFY(h.waitIdle());
    QVERIFY2(!h.finals.isEmpty(), qPrintable(h.asker.asked.join(QLatin1Char(',')) + QLatin1String(" | ") + h.window->footerText()));
    QCOMPARE(h.finals.last().outcome, Outcome::NothingChanged);
    QVERIFY2(h.window->footerText().contains(QStringLiteral("No polkit authentication agent")), qPrintable(h.window->footerText()));
    QVERIFY2(h.window->footerText().contains(QStringLiteral("From a terminal: sudo ")), qPrintable(h.window->footerText()));
    QPushButton* copy = h.window->button(QStringLiteral("asbCopyCommand"));
    QVERIFY(copy && copy->isVisible());
    QTest::mouseClick(copy, Qt::LeftButton);
    QVERIFY2(QGuiApplication::clipboard()->text().startsWith(QStringLiteral("sudo ")), qPrintable(QGuiApplication::clipboard()->text()));
    QVERIFY(QGuiApplication::clipboard()->text().contains(QStringLiteral("--install")));
    QCOMPARE(h.rig.service.installation.state, InstallState::NotInstalled);
}

// WS-14's reason before any click (WS-15 hand-over): the window's own program is not its build -> Install, Update and
// Uninstall are disabled with that reason; Start and Stop (the installed helper) are not.
void tst_BridgeWindow::linuxProgramMismatchDisablesStagedActions()
{
    const QString why = QStringLiteral("This window's program is not the one it was built with (test).");
    {
        HelperHarness h(cli::HelperClient::kWaitDeadline, stagedProgram(why));
        h.open();
        QPushButton* life = h.window->button(QStringLiteral("asbLifecycle"));
        QVERIFY(life->isVisible());
        QTRY_COMPARE(life->toolTip(), why);   // after the probe (a worker thread)
        h.window->refreshStatus();
        QVERIFY(!life->isEnabled());
        QVERIFY2(h.window->serviceText().contains(why), qPrintable(h.window->serviceText()));
        h.window->installService();
        QVERIFY(h.asker.asked.isEmpty());
        QCOMPARE(h.proc.runs, 0);
    }
    {
        HelperHarness h(cli::HelperClient::kWaitDeadline, stagedProgram(why));
        h.rig.installedRunning(configA());
        h.open();
        QTRY_COMPARE(h.window->button(QStringLiteral("asbUninstall"))->toolTip(), why);
        h.window->refreshStatus();
        QVERIFY(!h.window->button(QStringLiteral("asbUninstall"))->isEnabled());
        QVERIFY(h.window->button(QStringLiteral("asbStop"))->isEnabled());   // the installed helper's ops as v1.0
    }
}

// WS-14 interim disclosure, closed by WS-17: on Linux, Update in the build question really updates, through the staged
// route (the window's own program); No, keep it applies through the installed helper and keeps its program.
void tst_BridgeWindow::linuxBuildQuestionUpdateIsStaged_data()
{
    QTest::addColumn<bool>("update");
    QTest::newRow("update-staged") << true;
    QTest::newRow("keep-installed-helper") << false;
}

void tst_BridgeWindow::linuxBuildQuestionUpdateIsStaged()
{
    QFETCH(bool, update);
    HelperHarness h(cli::HelperClient::kWaitDeadline, stagedProgram());
    h.rig.installedRunning(configA());
    otherBuild(h.rig);
    h.open();
    h.rename(QStringLiteral("KAT500"));
    h.asker.choices[QStringLiteral("build")] = update ? QStringLiteral("update") : QStringLiteral("keep");
    h.rig.world.log.clear();
    h.window->saveAndApply();
    QVERIFY(h.waitIdle());
    QCOMPARE(h.proc.stagedRuns, update ? 1 : 0);
    QCOMPARE(h.proc.runs, 1);
    if (!update) {
        // The installed helper's v1.0 save-and-apply. (Here the in-process helper is this build, which sees the
        // installation as another build and refuses; the real installed helper is that build and sees it Current:
        // sysd::v10_installed_day_to_day.) Its program is never replaced.
        QVERIFY(h.rig.service.installation.otherBuild);
        QVERIFY(h.rig.world.log.indexOf(QStringLiteral("service:installOrConfigure")) < 0);
        return;
    }
    QVERIFY2(h.finals.last().outcome == Outcome::Ok, qPrintable(h.finals.last().message));
    QVERIFY(h.rig.world.log.indexOf(QStringLiteral("service:installOrConfigure")) >= 0);
    QVERIFY(!h.rig.service.installation.otherBuild);
    QVERIFY(!h.window->editor().dirty());
}
// The Linux window with nothing installed (v1.1 fix after WS-17). It has no route that saves without installing: no
// installed helper exists yet, and the staged route runs only install and uninstall (INV-123). So the box is held
// ticked, and "Install and apply" is the staged install (the polkit /bin/sh question first). A box found unticked
// (forced here) makes the button read "Save", disabled with the reason, and a click runs nothing.
void tst_BridgeWindow::linuxMainButtonOnNothingInstalled_data()
{
    QTest::addColumn<bool>("ticked");
    QTest::newRow("box ticked: Install and apply is the staged install") << true;
    QTest::newRow("box unticked: Save, disabled, runs nothing") << false;
}

void tst_BridgeWindow::linuxMainButtonOnNothingInstalled()
{
    QFETCH(bool, ticked);
    HelperHarness h(cli::HelperClient::kWaitDeadline, stagedProgram());
    h.open();
    h.window->addPort();
    h.window->editor().edit().ports[0].comPort = QStringLiteral("/dev/ttyUSB0");   // no device list in this harness
    h.window->editor().edit().ports[0].name = QStringLiteral("Amp");
    QPushButton* save = h.window->button(QStringLiteral("asbSaveApply"));
    QCheckBox* box = h.window->autoStartBox();
    QTRY_VERIFY(save->isEnabled());   // the window's own program was probed (the staged route's reason is empty)
    QCOMPARE(box->text(), QStringLiteral("Start automatically at boot"));
    QVERIFY(box->isChecked());
    QVERIFY(!box->isEnabled());
    QVERIFY2(box->toolTip().contains(QStringLiteral("saved by installing")), qPrintable(box->toolTip()));
    QCOMPARE(save->text(), QStringLiteral("Install and apply"));
    if (ticked) {
        const Submission s = h.window->editor().submission(true);
        h.asker.answers[QStringLiteral("install")] = true;
        QTest::mouseClick(save, Qt::LeftButton);
        QVERIFY(h.waitIdle());
        QVERIFY2(h.asker.asked.contains(QStringLiteral("install")), qPrintable(h.asker.asked.join(QLatin1Char(','))));
        QVERIFY2(h.asker.questions.last().text.contains(QStringLiteral("/bin/sh")), qPrintable(h.asker.questions.last().text));
        QCOMPARE(h.proc.stagedRuns, 1);   // the staged route: never the installed helper (there is none)
        QCOMPARE(h.proc.runs, 1);
        QVERIFY2(h.finals.last().outcome == Outcome::Ok, qPrintable(h.finals.last().message));
        QCOMPARE(committed(h.rig), s.bytes);
        QCOMPARE(h.rig.service.installation.state, InstallState::Current);
        QVERIFY(h.rig.service.engineRunning);
        QCOMPARE(save->text(), QStringLiteral("Save && apply"));
        QVERIFY(box->isEnabled());   // installed: the real start type, which may be unticked now
        return;
    }
    box->setChecked(false);
    QCOMPARE(save->text(), QStringLiteral("Save"));
    QVERIFY(!save->isEnabled());
    QVERIFY2(save->toolTip().contains(QStringLiteral("tick Start automatically at boot")), qPrintable(save->toolTip()));
    QVERIFY(box->isEnabled());   // so that it can be ticked again
    h.window->saveAndApply();
    QVERIFY(!h.window->busy());
    QCOMPARE(h.proc.runs, 0);
    QCOMPARE(h.proc.stagedRuns, 0);
    QVERIFY(h.asker.asked.isEmpty());
    box->setChecked(true);
    QCOMPARE(save->text(), QStringLiteral("Install and apply"));
    QVERIFY(save->isEnabled());
    QVERIFY(!box->isEnabled());
}
#else
void tst_BridgeWindow::linuxNoAgentShowsTerminalCommand() { QSKIP("Linux only (the staged helper's polkit route)"); }
void tst_BridgeWindow::linuxProgramMismatchDisablesStagedActions() { QSKIP("Linux only (the staged helper's polkit route)"); }
void tst_BridgeWindow::linuxBuildQuestionUpdateIsStaged_data()
{
    QTest::addColumn<bool>("update");
    QTest::newRow("linux-only") << true;
}
void tst_BridgeWindow::linuxBuildQuestionUpdateIsStaged() { QSKIP("Linux only (the staged helper's polkit route)"); }
void tst_BridgeWindow::linuxMainButtonOnNothingInstalled_data()
{
    QTest::addColumn<bool>("ticked");
    QTest::newRow("linux-only") << true;
}
void tst_BridgeWindow::linuxMainButtonOnNothingInstalled() { QSKIP("Linux only (the staged helper's polkit route)"); }
#endif

// v1.1 WS-19 (INV-135): the documentation images. Run by tools/docs_shots.py only (skipped otherwise), which isolates
// HOME, XDG_* and the Windows profile folders and names the demo fixture; the hook refuses to run without both
// (docs::refusal). Each image is the window (or one of its questions, rendered as its own widget, as the user sees it
// over the window) over the WS-5 fakes set up from docs/images/demo/, at the launch size (1040 x 760) in AetherSDR's
// Default Dark or Light, at the process's scale (QT_SCALE_FACTOR: 1, or 2 for the hero image). Beside each PNG,
// <name>.json lists every text the image shows (labels, buttons, fields, list and table cells, headers, the title).
namespace {

QString plainText(const QString& t)
{
    return Qt::mightBeRichText(t) ? QTextDocumentFragment::fromHtml(t).toPlainText() : t;
}

QStringList visibleTexts(QWidget* root)
{
    QStringList out{root->windowTitle()};
    QList<QWidget*> widgets{root};
    widgets += root->findChildren<QWidget*>();
    for (QWidget* w : widgets) {
        if (w != root && !w->isVisibleTo(root)) continue;
        if (auto* l = qobject_cast<QLabel*>(w)) out << plainText(l->text()).split(QLatin1Char('\n'));
        else if (auto* b = qobject_cast<QAbstractButton*>(w)) out << b->text();
        else if (auto* e = qobject_cast<QLineEdit*>(w)) out << e->text() << e->placeholderText();
        else if (auto* combo = qobject_cast<QComboBox*>(w)) {
            out << combo->currentText();
            for (int i = 0; i < combo->count(); ++i) out << combo->itemText(i);
        } else if (auto* g = qobject_cast<QGroupBox*>(w)) out << g->title();
        else if (auto* pt = qobject_cast<QPlainTextEdit*>(w)) out << pt->toPlainText().split(QLatin1Char('\n'));
        else if (auto* te = qobject_cast<QTextEdit*>(w)) out << te->toPlainText().split(QLatin1Char('\n'));
        else if (auto* lw = qobject_cast<QListWidget*>(w)) {
            for (int i = 0; i < lw->count(); ++i) out << lw->item(i)->text();
        } else if (auto* v = qobject_cast<QAbstractItemView*>(w)) {
            if (QAbstractItemModel* m = v->model()) {
                for (int c = 0; c < m->columnCount(); ++c) out << m->headerData(c, Qt::Horizontal).toString();
                for (int r = 0; r < m->rowCount(); ++r)
                    for (int c = 0; c < m->columnCount(); ++c) out << m->index(r, c).data().toString();
            }
        }
        if (!w->toolTip().isEmpty() && w == root) out << w->toolTip();
    }
    return out;
}

bool activateTheme(const QString& theme)
{
    QFile f(theme == QLatin1String("dark") ? QStringLiteral(":/themes/default-dark.json") : QStringLiteral(":/themes/default-light.json"));
    if (!f.open(QIODevice::ReadOnly)) return false;
    return AetherSDR::ThemeManager::instance().activateValidatedTheme(
        theme == QLatin1String("dark") ? QStringLiteral("Default Dark") : QStringLiteral("Default Light"), f.readAll());
}

// Renders `w` (a window or a dialog) to <dir>/<name>.png at its device pixel ratio, and writes its manifest.
bool shoot(QWidget* w, const QString& dir, docs::Shot shot)
{
    QApplication::processEvents();
    const qreal dpr = w->devicePixelRatioF();
    QImage img(w->size() * dpr, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    w->render(&img, QPoint(), QRegion(), QWidget::DrawChildren);
    shot.scale = dpr;
    shot.width = img.width();
    shot.height = img.height();
    shot.texts = visibleTexts(w);
    return img.save(dir + QLatin1Char('/') + shot.name + QStringLiteral(".png")) && docs::writeManifest(dir, shot);
}

// The demo station on the fakes: the fixture's config committed and running (Current), with its status.
void demoRunning(Rig& rig, bool windows)
{
    rig.installedRunning(docs::config(windows));
    docs::applyInstallation(rig.service.installation, QStringLiteral("current"));
    rig.service.status = docs::statusOver(rig.service.status, windows);
}

// Nothing installed; the settings an earlier uninstall kept are in the data folder (the editor shows the demo ports).
void demoNotInstalled(Rig& rig, bool windows)
{
    rig.world.files.plantFile(QString::fromLatin1(ConfigFiles::kConfig), docs::config(windows), QStringLiteral("root"));
    docs::applyInstallation(rig.service.installation, QStringLiteral("not-installed"));
    rig.service.programPresent = rig.service.folderPresent = false;
    rig.world.log.clear();
}

}  // namespace

void tst_BridgeWindow::docsShots()
{
    const QString dir = qEnvironmentVariable("ASB_DOCS_SHOTS_DIR");
    if (dir.isEmpty()) QSKIP("ASB_DOCS_SHOTS_DIR not set (tools/docs_shots.py runs this)");
    const QString why = docs::refusal();
    QVERIFY2(why.isEmpty(), qPrintable(why));
    const bool hero = qEnvironmentVariableIsSet("ASB_DOCS_SHOTS_HERO");
#ifdef _WIN32
    const bool windows = true;
#else
    const bool windows = false;
#endif
    const QString os = windows ? QStringLiteral("windows") : QStringLiteral("linux");
    const docs::Environment env = docs::environment(windows);
    const auto shotOf = [&](const QString& name, const QString& state, const QString& theme) {
        return docs::Shot{name, os, state, QStringLiteral("Default ") + (theme == QLatin1String("dark") ? QStringLiteral("Dark") : QStringLiteral("Light")),
                          1, 0, 0, {}};
    };
    const auto place = [](BridgeWindow* w) {
        w->resize(1040, 760);
        QVERIFY(QTest::qWaitForWindowExposed(w));
        w->refreshStatus();   // the status poll (refreshMs 0 here: no timer)
        QApplication::processEvents();
    };
    // The question the next call opens, rendered as its own widget, then cancelled. `seen` and `ok` are the caller's.
    const auto shootDialog = [&](QString* seen, bool* ok, const QString& name, const QString& state, const QString& theme) {
        onNextDialog([&dir, ok, shot = shotOf(name, state, theme)](QDialog* d) {
            // Wide enough that the list's lines are not cut (the user can widen the dialog the same way).
            d->resize(qMax(d->width(), 760), d->height());
            QApplication::processEvents();
            d->resize(d->width(), qMax(d->height(), d->sizeHint().height()));
            *ok = shoot(d, dir, shot);
            if (auto* c = d->findChild<QPushButton*>(QStringLiteral("asbDialogChoice_cancel"))) QTest::mouseClick(c, Qt::LeftButton);
            else d->reject();
        }, seen);
    };

    if (windows) {
        // The README's hero: the running window, dark, at scale 2 (the second run, QT_SCALE_FACTOR=2).
        const QStringList themes = hero ? QStringList{QStringLiteral("dark")} : QStringList{QStringLiteral("dark"), QStringLiteral("light")};
        for (const QString& theme : themes) {
            QVERIFY(activateTheme(theme));
            Harness h;
            h.stillSparks = true;
            h.serialPorts = env.serialDevices;
            h.localAddrs = env.localAddresses;
            h.dataFolder = env.dataFolder;
            demoRunning(h.rig, true);
            h.open();
            place(h.window.get());
            QVERIFY(shoot(h.window.get(), dir, shotOf(QStringLiteral("window-running-") + theme + (hero ? QStringLiteral("@2x") : QString()),
                                                       QStringLiteral("installed and running, one client connected"), theme)));
        }
        if (hero) return;
        {
            QVERIFY(activateTheme(QStringLiteral("light")));
            Harness h;
            h.stillSparks = true;
            h.serialPorts = env.serialDevices;
            h.localAddrs = env.localAddresses;
            h.dataFolder = env.dataFolder;
            demoNotInstalled(h.rig, true);
            h.open();
            place(h.window.get());
            QVERIFY(h.window->button(QStringLiteral("asbLifecycle"))->isVisible());
            QVERIFY(shoot(h.window.get(), dir, shotOf(QStringLiteral("window-not-installed-light"),
                                                       QStringLiteral("nothing installed (settings kept), elevated"), QStringLiteral("light"))));
        }
        QVERIFY(activateTheme(QStringLiteral("dark")));
        {
            Harness h;
            h.stillSparks = true;
            h.readOnly = true;
            h.restartAsAdmin = [](const QString&) { return BridgeWindowOptions::Relaunch{}; };
            h.serialPorts = env.serialDevices;
            h.localAddrs = env.localAddresses;
            h.dataFolder = env.dataFolder;
            demoRunning(h.rig, true);
            h.open();
            place(h.window.get());
            QVERIFY(h.window->button(QStringLiteral("asbRestartAdmin"))->isVisible());
            QVERIFY(shoot(h.window.get(), dir, shotOf(QStringLiteral("window-read-only-dark"),
                                                       QStringLiteral("a standard user's window, read-only, Restart as administrator"), QStringLiteral("dark"))));
        }
        {
            Harness h;
            h.stillSparks = true;
            h.realDialogs = true;
            h.serialPorts = env.serialDevices;
            h.localAddrs = env.localAddresses;
            h.dataFolder = env.dataFolder;
            demoRunning(h.rig, true);
            docs::applyInstallation(h.rig.service.installation, QStringLiteral("other-build"));
            h.open();
            place(h.window.get());
            h.rename(0, QStringLiteral("Amplifier"));
            QString seen;
            bool ok = false;
            shootDialog(&seen, &ok, QStringLiteral("dialog-build-question-dark"), QStringLiteral("Save & apply with another build installed"),
                        QStringLiteral("dark"));
            h.window->saveAndApply();
            QCOMPARE(seen, QStringLiteral("asbDialog_build"));
            QVERIFY(ok);
            QVERIFY(h.waitIdle());
            QCOMPARE(h.rig.service.installation.otherBuild, true);   // cancelled: nothing changed
        }
        {
            Harness h;
            h.stillSparks = true;
            h.realDialogs = true;
            h.serialPorts = env.serialDevices;
            h.localAddrs = env.localAddresses;
            h.dataFolder = env.dataFolder;
            demoRunning(h.rig, true);
            h.open();
            place(h.window.get());
            QString seen;
            bool ok = false;
            shootDialog(&seen, &ok, QStringLiteral("dialog-uninstall-dark"), QStringLiteral("Uninstall... while one client is connected"),
                        QStringLiteral("dark"));
            h.window->uninstallService();
            QCOMPARE(seen, QStringLiteral("asbDialog_uninstall"));
            QVERIFY(ok);
            QCOMPARE(h.rig.service.installation.state, InstallState::Current);
        }
        return;
    }

#ifndef _WIN32
    if (hero) return;   // the hero image is the Windows window's
    const auto linuxHarness = [&](std::unique_ptr<HelperHarness>& h) {
        h = std::make_unique<HelperHarness>(cli::HelperClient::kWaitDeadline, stagedProgram({}, env.appImage));
        h->serialPorts = env.serialDevices;
        h->localAddrs = env.localAddresses;
        h->dataFolder = env.dataFolder;
        h->stillSparks = true;
    };
    for (const QString& theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
        QVERIFY(activateTheme(theme));
        std::unique_ptr<HelperHarness> h;
        linuxHarness(h);
        demoRunning(h->rig, false);
        h->open();
        place(h->window.get());
        QVERIFY(shoot(h->window.get(), dir, shotOf(QStringLiteral("window-running-") + theme,
                                                    QStringLiteral("installed and running, one client connected"), theme)));
    }
    {
        QVERIFY(activateTheme(QStringLiteral("light")));
        std::unique_ptr<HelperHarness> h;
        linuxHarness(h);
        demoNotInstalled(h->rig, false);
        h->open();
        place(h->window.get());
        QTRY_VERIFY(h->window->button(QStringLiteral("asbLifecycle"))->isEnabled());   // after the program probe
        QVERIFY(shoot(h->window.get(), dir, shotOf(QStringLiteral("window-not-installed-light"),
                                                    QStringLiteral("nothing installed (settings kept)"), QStringLiteral("light"))));
    }
    QVERIFY(activateTheme(QStringLiteral("dark")));
    {
        // No polkit agent in the session: the window says so, changes nothing and offers the terminal command.
        std::unique_ptr<HelperHarness> h;
        linuxHarness(h);
        demoNotInstalled(h->rig, false);
        h->asker.answers[QStringLiteral("install")] = true;
        cli::IHelperProcess::Exit noAgent;
        noAgent.code = 127;
        noAgent.err = QByteArrayLiteral("Error creating textual authentication agent: Error opening current controlling terminal");
        h->proc.replaceWith = noAgent;
        h->open();
        place(h->window.get());
        QTRY_VERIFY(h->window->button(QStringLiteral("asbLifecycle"))->isEnabled());
        h->window->installService();
        QVERIFY(h->waitIdle());
        QVERIFY2(h->window->footerText().contains(QStringLiteral("No polkit authentication agent")), qPrintable(h->window->footerText()));
        QCOMPARE(h->rig.service.installation.state, InstallState::NotInstalled);
        QVERIFY(shoot(h->window.get(), dir, shotOf(QStringLiteral("window-no-agent-dark"),
                                                    QStringLiteral("Install with no polkit agent: nothing changed, the terminal command"), QStringLiteral("dark"))));
    }
    {
        std::unique_ptr<HelperHarness> h;
        linuxHarness(h);
        h->realDialogs = true;
        demoRunning(h->rig, false);
        h->open();
        place(h->window.get());
        QTRY_VERIFY(h->window->button(QStringLiteral("asbUninstall"))->isEnabled());
        QString seen;
        bool ok = false;
        shootDialog(&seen, &ok, QStringLiteral("dialog-uninstall-dark"), QStringLiteral("Uninstall... while one client is connected"),
                    QStringLiteral("dark"));
        h->window->uninstallService();
        QCOMPARE(seen, QStringLiteral("asbDialog_uninstall"));
        QVERIFY(ok);
        QCOMPARE(h->rig.service.installation.state, InstallState::Current);
    }
#endif
}

QTEST_MAIN(tst_BridgeWindow)
#include "tst_BridgeWindow.moc"
