#pragma once

// The Linux window's actions: every operation through a root helper started by pkexec (WS-9b HelperClient over this
// window's PkexecClient). The helper runs the Applier as root and answers with a Result; HelperClient turns pkexec's own
// exits (126 dismissed, 127 not authorized or no agent), a refused request (2), the staging script's own exits (3, 4,
// 6) and a lost helper into NothingChanged / StateUnknown results with a line to show, so the window treats every answer
// like an in-process final result (INV-107: the window clears dirty only for persisted != None with the digest it
// computed over the bytes it submitted).
//
// Routes (v1.1 WS-15 §3, cli::routeFor; INV-123). Install, Update (Save & apply's Update included) and Uninstall run
// the window's OWN program, staged and verified (D60, D62): its bytes are read and hashed on a worker thread (never the
// window's), and the client launches exactly those bytes only when their digest is this window's build. Save & apply on an installation the window keeps (Current, or
// "No, keep it"), Start, Stop, Repair and the untick use the installed helper with v1.0 ops; Discard sends
// `discard-pending-if` (with the digest) only to an installed program that carries a build stamp (v1.1 or later),
// else v1.0's `discard-pending`. The window's buttons and dialogs are WS-17's; it calls install(), update() and uninstall() here.

#include "cli/HelperClient.h"
#include "core/Applier.h"
#include "gui/Actions.h"

#include <QObject>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace asb::gui {

class HelperActions final : public IActions {
public:
    // The window's own program (WS-14's windowProgram()): read on a worker thread at each staged action.
    struct ProgramFile {
        QString path;
        QByteArray bytes;
        QString problem;   // why it cannot be used (the locator's reason, an unreadable file); empty when it can
    };
    struct Staged {
        QByteArray expectedSha256;               // the digest compiled into the window (lower-case hex)
        std::function<ProgramFile()> read;       // runs on a worker thread; nullptr: no staged route in this window
        QString appImage;                        // $APPIMAGE when the window runs from an AppImage, else empty
        // The digest of the bytes read (lower-case hex), on the same worker thread, over the same buffer that is sent.
        // nullptr: sha256Hex. A test seam (tst_HelperActions); the window never sets it.
        std::function<QByteArray(const QByteArray&)> hash;
        // Why the window's own program cannot be used (WS-14's windowProgram().problem), empty when it can: run once on a
        // worker thread when this object is made, so the window greys out Install, Update and Uninstall with the reason
        // before any click (WS-17 §1). nullptr: nothing is known before a click (the click still checks, D60).
        std::function<QString()> probe;
    };
    using Facts = std::function<cli::RouteFacts()>;

    // `facts` is read on the window's thread at each routed action (default: the installed program's stamp).
    explicit HelperActions(cli::HelperClient& client, Staged staged = {}, Facts facts = {});
    ~HelperActions() override;
    HelperActions(const HelperActions&) = delete;
    HelperActions& operator=(const HelperActions&) = delete;

    bool available() const override { return true; }
    Installation::Flavor flavor() const override { return Installation::Flavor::Linux; }
    bool busy() const override { return m_loading || m_client.busy(); }
    // Ticked Save & apply: `install` (not Current, or Update on another build) is the staged install with the editor's
    // ports; otherwise the installed helper's save-and-apply.
    bool run(const SaveRequest&, bool install, IOperationObserver&) override;
    bool untick(const SaveRequest&, IOperationObserver&) override;
    bool start(IOperationObserver&) override;
    bool stop(IOperationObserver&) override;
    bool repair(IOperationObserver&) override;
    bool discardPending(const QString& sha256, IOperationObserver&) override;

    // The staged lifecycle actions (D60): the window's own program, read, hashed and launched only when it is this build.
    bool install(const std::optional<SaveRequest>&, IOperationObserver&) override;
    bool update(const std::optional<SaveRequest>&, IOperationObserver&) override;
    bool uninstall(UninstallOptions, IOperationObserver&) override;
    // While the probe runs: "Checking this window's program…"; then its problem (empty when the program is this build).
    QString lifecycleUnavailableReason() const override;
    void setLifecycleListener(std::function<void()> listener) override { m_listener = std::move(listener); }

    // The final result a HelperClient outcome becomes (its message is the line HelperClient chose). The window decides
    // dirty from it with the same rule HelperClient's clearsDirty applies (persisted != None and the digest of the
    // exact bytes submitted, INV-107); the outcome itself is kept for diagnostics and tests.
    static Result toResult(const cli::HelperClient::Outcome&);
    const std::optional<cli::HelperClient::Outcome>& lastOutcome() const { return m_last; }

private:
    using StagedSubmit = std::function<bool(const cli::HelperClient::StagedProgram&)>;
    cli::HelperClient::Done finishTo(IOperationObserver&);
    cli::HelperClient::Waiting interimTo(IOperationObserver&);
    cli::RouteFacts facts() const;
    bool prepareInstalled();
    bool staged(IOperationObserver&, StagedSubmit);
    bool stagedInstall(const std::optional<SaveRequest>&, IOperationObserver&);

    struct Shared {
        std::mutex m;
        bool alive = true;
    };
    cli::HelperClient& m_client;
    Staged m_staged;
    Facts m_facts;
    std::optional<cli::HelperClient::Outcome> m_last;
    bool m_loading = false;
    QObject m_context;   // the worker's result is posted here
    std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
    std::thread m_worker;
    std::thread m_probe;
    bool m_probing = false;
    QString m_probeProblem;
    std::function<void()> m_listener;
};

}  // namespace asb::gui
