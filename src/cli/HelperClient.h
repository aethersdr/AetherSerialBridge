#pragma once

// The Linux GUI's side of the pkexec helper (master §5.7, D2, D57; INV-107, INV-118). Linux only.
//
// HelperClient owns the protocol and every decision the window needs; it never creates a process. The process
// boundary is IHelperProcess, which the Linux GUI implements in gui/PkexecClient.cpp (WS-10) with QProcess:
// `pkexec /usr/local/bin/aether-serial-bridge --helper`, the request written to stdin and stdin closed, the exit
// code and both outputs handed back. That object is the only process creation in the product (INV-112).
//
// What WS-10 wires (one call per button; the callback runs on the GUI thread):
//   Save & apply (ticked)    saveAndApply(bytes, source, supersedePending, editor revision, done, waiting)
//   Save & apply (unticked)  deactivate(bytes, ...)          Start / Stop / Repair / Discard pending   start() ...
//   preflight / refresh      inspect(done)
//   then: show outcome.message; if (outcome.clearsDirty) tracker.markSaved(outcome.revision); while busy(), the
//   window refuses to close (D57).
//
// Decisions (pure, HelperClient::interpret):
//   - dirty is cleared only when persisted ≠ None AND persistedSha256 equals the sha256 the CLIENT computed over the
//     exact bytes it submitted (never the echo alone; INV-107);
//   - pkexec exit 126 (the authentication dialog was dismissed) → Cancelled: "Cancelled; nothing was changed.";
//   - pkexec exit 127 → NoAgent when pkexec's stderr says no agent could ask (polkit 122 says "Error creating textual
//     authentication agent", older ones "No authentication agent found"), else NotAuthorized; nothing changed. The
//     stderr text picks the message, never the outcome (INV-124);
//   - helper exit 2 (it refused the request before any lock) or 5 (its process guard could not be installed, at mode
//     entry) → Refused, nothing changed;
//   - exit 0/1 with a response that parses and agrees with the exit code → Done (the helper's Result);
//   - anything else (a crash, no response, a response that does not parse) → Failed: the bridge's state is not known
//     (StateUnknown, dirty kept).
// A wait longer than the wait deadline calls `waiting` once (the interim banner); the client keeps waiting for the
// helper (D57: the operation's lifetime is the helper's, not the window's).
//
// v1.1 (WS-15, D60, INV-122, INV-124): the STAGED route (StagedHelper.h) runs the window's own program through the
// staging script: install (with or without the editor's ports) and uninstall. Before a launch the client compares the
// digest of the exact program bytes it will send (taken where they were read, off the window's thread: StagedProgram)
// with the window's build and refuses to launch when they differ (a mismatch never launches). On the staged route the script's own exits are classified from its one "asb-stage:" line: 3 Refused (the
// program does not match its fingerprint), 4 Refused (nothing was changed: the line; an unsafe directory adds the
// remedy and the terminal command), 6 Refused (the copy could not be started). On the installed route 3, 4 and 6 stay
// what they were in v1.0: the helper never exits with them, so the state is not known.

#include "cli/HelperProtocol.h"
#include "cli/StagedHelper.h"

#include <QByteArray>
#include <QObject>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace asb::cli {

class IHelperProcess {
public:
    struct Exit {
        bool started = true;    // false: the program (pkexec) could not be started at all
        bool crashed = false;   // ended by a signal
        int code = -1;
        QByteArray out, err;
    };
    virtual ~IHelperProcess() = default;
    // Starts the helper, writes `request` to its stdin and closes it; calls `done` exactly once, later, on the
    // calling thread (never from inside run()).
    virtual void run(const QByteArray& request, std::function<void(const Exit&)> done) = 0;
    // The staged route (D60): `pkexec /bin/sh -c <script> asb-stage <expectedSha256> <program length> <request length>`,
    // `program` then `request` written to its stdin, then stdin closed; `done` as for run().
    virtual void runStaged(const QByteArray& expectedSha256, const QByteArray& program, const QByteArray& request,
                           std::function<void(const Exit&)> done) = 0;
};

class HelperClient {
public:
    static constexpr int kPkexecDismissed = 126;
    static constexpr int kPkexecNotAuthorized = 127;
    // The GUI-side wait deadline: the Applier's first phase deadline (the barrier, 30 s) plus start-up slack.
    static constexpr std::chrono::milliseconds kWaitDeadline{35000};

    struct Outcome {
        enum class Kind { Done, Cancelled, NotAuthorized, NoAgent, Refused, Failed };
        Kind kind = Kind::Failed;
        helper::Op op = helper::Op::Inspect;
        Route route = Route::Installed;
        Result result;                                  // Done: the helper's; otherwise NothingChanged or StateUnknown
        std::optional<helper::InspectReport> inspect;   // Done, op Inspect
        QString message;                                // the line to show
        quint64 revision = 0;                           // the editor revision the submission carried
        QByteArray submittedSha256;                     // sha256Hex of the submitted bytes (save ops)
        bool clearsDirty = false;                       // INV-107
    };
    using Done = std::function<void(const Outcome&)>;
    using Waiting = std::function<void(const QString&)>;

    // What a staged request runs: the window's program bytes, the SHA-256 of exactly these bytes, the digest compiled
    // into the window (both lower-case hex), and the program a terminal command names ($APPIMAGE, else the headless
    // program itself; INV-124). Hashing 20-40 MB would freeze the window (most on a Pi), so bytesSha256 is taken where
    // the bytes were read, off the window's thread, over this very buffer: QByteArray is implicitly shared and nothing
    // here modifies it, so the digest stays bound to the bytes that are sent, and the file is never read again.
    struct StagedProgram {
        QByteArray bytes;
        QByteArray bytesSha256;      // sha256Hex(bytes); empty or anything else than expectedSha256: never launched
        QByteArray expectedSha256;
        QString commandProgram;
    };
    // What interpret() is told besides the exit: the terminal command a 127 or an unsafe directory shows (empty: none),
    // and, on the installed route, whether the view says nothing is installed (v1.0's "Is the bridge installed?").
    struct Context {
        QString terminalCommand;
        bool nothingInstalled = true;
    };

    explicit HelperClient(IHelperProcess&, std::chrono::milliseconds waitDeadline = kWaitDeadline);
    HelperClient(const HelperClient&) = delete;
    HelperClient& operator=(const HelperClient&) = delete;

    // One request at a time: false (nothing started) while one is outstanding.
    bool busy() const { return m_busy; }

    bool saveAndApply(const QByteArray& configBytes, const Source&, bool supersedePending, quint64 revision, Done,
                      Waiting = {});
    // Unticked Save & apply: the edits (if any) are kept as pending, the bridge is stopped and blocked (Deactivate).
    bool deactivate(const std::optional<QByteArray>& configBytes, const Source&, bool supersedePending, quint64 revision,
                    Done, Waiting = {});
    bool start(Done, Waiting = {});
    bool stop(Done, Waiting = {});
    bool repair(Done, Waiting = {});
    bool discardPending(Done, Waiting = {});
    bool inspect(Done, Waiting = {});
    // v1.1, installed route: Discard only while config.pending.json still has `sha256` (INV-127); a 1.1+ helper only.
    bool discardPendingIf(const QString& sha256, Done, Waiting = {});
    // v1.1, staged route (D60): install, update or finish an install, with the editor's ports when `configBytes` is
    // given (Save & apply's Update in one prompt); uninstall, with or without the settings (D62).
    bool install(const std::optional<QByteArray>& configBytes, const Source&, bool supersedePending, quint64 revision,
                 const StagedProgram&, Done, Waiting = {});
    bool uninstall(bool removeSettings, const StagedProgram&, Done, Waiting = {});
    // Any request on the staged route (the two above, and the systemd suite's staged inspect).
    bool submitStaged(const helper::Request&, quint64 revision, const StagedProgram&, Done, Waiting = {});

    // The view's facts for the installed route's 127 hint (default: nothing installed, as v1.0 always said).
    void setNothingInstalled(bool nothingInstalled) { m_nothingInstalled = nothingInstalled; }

    // The decision for one finished helper run of `request`, submitted as `revision` with digest `submittedSha256`.
    static Outcome interpret(const helper::Request&, const QByteArray& submittedSha256, quint64 revision,
                             const IHelperProcess::Exit&, Route, const Context&);
    // The installed route with no view facts (v1.0's decision, and its "Is the bridge installed?" hint).
    static Outcome interpret(const helper::Request& r, const QByteArray& submittedSha256, quint64 revision,
                             const IHelperProcess::Exit& e)
    {
        return interpret(r, submittedSha256, revision, e, Route::Installed, Context{QString(), true});
    }
    static QString waitingMessage();
    static QString fingerprintMismatchMessage();

private:
    using Launch = std::function<void(const QByteArray& request, std::function<void(const IHelperProcess::Exit&)>)>;
    bool submit(const helper::Request&, quint64 revision, Done, Waiting);
    bool submitVia(const helper::Request&, quint64 revision, Route, const Context&, Launch, Done, Waiting);

    IHelperProcess& m_process;
    std::chrono::milliseconds m_waitDeadline;
    QObject m_context;          // the wait timer's receiver
    // Expires with the client: a helper run that ends after the client was destroyed is ignored.
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
    quint64 m_generation = 0;   // the current submission (a stale wait timer is ignored)
    bool m_busy = false;
    bool m_nothingInstalled = true;
};

}  // namespace asb::cli
