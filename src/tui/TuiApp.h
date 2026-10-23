#pragma once

// The TUI's controller (master section 6, WS-12): keys in, TuiState changes out, the same operations and outcome
// texts as the GUI (WS-10 5.5), through the same OperationHost (D57).
//
//  - Load is ReadOnly (master §4.4): an untrusted folder shows "Config not loaded: ..." and nothing from it (INV-33);
//    config.pending.json, when present and different from config.json, is the working copy, loaded clean (D48).
//  - Read-only unless elevated: the banner says so and every key that would change something refuses with the
//    same sentence; nothing is submitted.
//  - Consent first (exposure, untick, Repair, discard, supersede), then one submit; "n" writes nothing and calls no
//    service method (INV-35).
//  - The Applier runs on the OperationHost worker. An interim notification is a status line, never a result;
//    q, Ctrl+C and every other operation key are refused until the final result is delivered (INV-118).
//  - Dirty is cleared only when the result's `persisted` is not None and its persistedSha256 is the digest of the
//    exact bytes submitted (INV-107); an edit made during the operation stays dirty (EditTracker).

#include "cli/Commands.h"
#include "core/EditTracker.h"
#include "core/Interfaces.h"
#include "core/OperationHost.h"
#include "tui/TuiState.h"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

namespace asb::tui {

class TuiApp final : public IOperationObserver {
public:
    struct Env {
        bool elevated = false;
        bool readOnlyExecutable = false;   // the Linux GUI executable: always read only
        bool windows = false;
        bool utf8 = true;
        ColorMode color = ColorMode::None;
        int statusPollMs = 1000;           // status.json
        int servicePollMs = 2000;          // the service manager (not while an operation runs)
        QString program;                   // this running program's full path (the update instructions name it)
        bool appImage = false;             // program is $APPIMAGE
        QString headlessProgram;           // readOnlyExecutable: this build's headless program beside it, else empty
    };

    // `platform` may be null (it could not be built; `platformError` says why): the TUI then shows that and
    // changes nothing. Constructed on the thread whose event loop delivers the operation callbacks.
    TuiApp(cli::Host& host, Platform* platform, const QString& platformError, const Env& env);
    ~TuiApp() override;
    TuiApp(const TuiApp&) = delete;
    TuiApp& operator=(const TuiApp&) = delete;

    const TuiState& state() const { return m_s; }
    // FTXUI decides the terminal's colour depth once its loop is installed; the runner passes it on.
    void setColorMode(ColorMode);
    ftxui::Element render(int width, int height) const;

    // One key; true when it was handled (every key is: the TUI owns the terminal).
    bool onEvent(const ftxui::Event&);

    // The poll timers' work, callable directly.
    void refreshStatus();
    void refreshService();

    // The terminal went away (SIGHUP, SIGTERM, the console closed): quit now, or after the outstanding
    // operation's final result when one runs (master §4.6: the TUI never exits because of an operation and never
    // abandons one).
    void hangup();
    // SIGTERM: quit now, or after the outstanding operation's final result; the screen stays.
    void terminate();
    bool quitRequested() const { return m_quit; }
    bool drawing() const { return !m_s.hangup; }
    bool busy() const;

    // Called whenever the state changed (the runner posts a redraw).
    std::function<void()> changed;

    // IOperationObserver (host thread).
    void interim(const Interim&) override;
    void finished(const Result&) override;

    // The dirty rule (INV-107): a result clears what was submitted only when it persisted exactly those bytes.
    static bool clearsDirty(const Result&, const QByteArray& submittedBytes);

private:
    enum class Op { None, Save, Start, Stop, Repair, DiscardPending };

    void notify();
    void reload();
    void loadInstallation();
    void say(const QString& message, Tone tone = Tone::Plain);
    bool refuseReadOnly();
    bool refuseBusy();
    bool refuseUnloaded();
    void markEdited();
    // `noActs`: No is an action too (it does something other than cancel), so it needs the decision visible like Yes.
    void ask(const QString& question, std::function<void(bool)> then, const QString& decision = {}, bool noActs = false);
    std::optional<QByteArray> pendingDigest();
    // v1.1 INV-128: after each read of the data folder its handle is closed again, so the TUI holds
    // nothing there between uses (an uninstall elsewhere can remove it). Never while an operation runs: the worker
    // uses the same folder object.
    void closeFolderIdle();
    void showResult(const QString& title, const QStringList& lines, Tone tone);
    void showPage(const QString& title, const QStringList& lines, const QList<int>& headings = {});
    void startBusy(Op op);
    void flushPendingResult();
    void refreshDiskEvidence();

    // Keys per screen.
    bool onPorts(const ftxui::Event&);
    bool onEdit(const ftxui::Event&);
    bool onConfirm(const ftxui::Event&);
    bool onResult(const ftxui::Event&);
    bool onPage(const ftxui::Event&);

    void requestQuit();
    void moveSelection(int delta);
    void openForm(int index);
    void acceptForm();
    void addPort();
    void deletePort();
    void toggle();

    // Operations.
    // What Save & apply will do, decided when it is pressed (before any question) and checked again at submit.
    struct SaveIntent {
        QByteArray bytes;
        quint64 rev = 0;
        enum class Action { Run, SaveAndRemove, Deactivate } action = Action::Run;
        bool install = false;       // Run: install (the service is not Current)
        bool untickAsks = false;    // the chain includes the untick question
        bool restarted = false;     // the decision changed under an earlier chain: it is asked about itself too
        // Run over a complete installation of another build (Installation::otherBuild): the chain asks before the
        // program is replaced; No keeps it (SaveRequest::keepInstalledBuild) and applies the settings to it.
        bool buildAsks = false;
        bool keepBuild = false;     // the answer: No
        // The update question (u, or on open) was answered y: it is the build question, so the chain does not ask
        // it again and the program is replaced. Never set by a restart (submitSave), which asks it again.
        bool buildAnswered = false;
        QString installedSha256;    // the other build the question names
    };
    SaveIntent captureIntent(const QByteArray& bytes, quint64 rev) const;
    bool refuseUnknownAutoStart();
    // `update`: the update question's y (offerUpdate): Save & apply's chain with its build question answered Update.
    // `askedSha256`: the build that question named; only that one is replaced.
    void saveAndApply(bool restarted = false, bool update = false, const QString& askedSha256 = {});
    void askExposure(const SaveIntent&);
    void askUntick(const SaveIntent&);
    void askBuild(const SaveIntent&);
    void askSupersede(const SaveIntent&);
    void askChanged(const SaveIntent&, bool supersede);
    void submitSave(const SaveIntent&, bool supersede);
    void start();
    void stop();
    void repair();
    void discardPending();
    void loadPending();
    void reloadAsked();
    // A complete installation of another build (user report 2026-10-20; the window asks the same on Update): on
    // open, after a reload that finds a build not yet asked about, and on u. Elevated: the update question; y runs
    // Save & apply's update (saveAndApply(false, true)); n and Esc change nothing. Read-only: what updating needs,
    // Enter dismisses.
    void offerUpdate();
    void maybeOfferUpdate();
    QString buildPhrase() const;

    void showPortList();
    void showLog();
    void showHelp();

    cli::Host& m_host;
    Platform* m_platform;
    QString m_platformError;
    Env m_env;
    TuiState m_s;
    EditTracker m_tracker;
    Source m_source;                       // where the working copy came from (INV-102)
    InstallState m_installState = InstallState::NotInstalled;
    bool m_otherBuild = false;          // detect(): a complete installation of another build
    QString m_installedSha256;          // its program's digest, when known
    std::optional<QString> m_installedVersion;   // its build stamp (v1.1 D61): the question's wording only
    std::optional<QString> m_offeredSha;         // the other build the update question last asked about
    bool m_offerPending = false;                 // reload() found one not asked about yet: asked on the ports screen
    std::unique_ptr<OperationHost> m_ops;
    Op m_op = Op::None;
    std::optional<QByteArray> m_submitted;
    quint64 m_submittedRev = 0;
    struct PendingResult {
        QString title;
        QStringList lines;
        Tone tone = Tone::Plain;
    };
    std::optional<PendingResult> m_pendingResult;   // arrived while a form or a question was open
    std::function<void(bool)> m_onAnswer;
    Screen m_answerReturn = Screen::Ports;
    QTimer m_statusTimer, m_serviceTimer, m_stepTimer;
    bool m_quit = false;
    bool m_quitAfterFinished = false;
    // The size render() was last called with: page scrolling is bounded by the page as drawn.
    mutable int m_viewWidth = 80, m_viewHeight = 24;
};

}  // namespace asb::tui
