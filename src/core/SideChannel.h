#pragma once

// The side-channel worker (master §4.7, S5, D29; INV-6, INV-68, INV-83): one thread per process owns
// all log and status I/O. The engine thread only pushes:
//  - log(): one line into a bounded ring (kQueueCapacity entries of at most kMaxLineBytes UTF-8 bytes)
//    under a mutex held only for the push; a full ring drops the line and counts it;
//  - publishStatus(): replaces a single latest-wins slot.
// The worker writes log lines to the sink and the status (through a StatusWriter, normally
// StatusStore::write with OwnerPolicy::Runtime) at most once per statusInterval. Nothing here hosts a
// safety check (D56): a blocked sink delays diagnostics, never bytes or the firewall deadline.
//
// Scheduling: before every log line the worker writes a status that is due (the interval has passed,
// a flush is waiting for it, or the channel is stopping), so a sustained log backlog never starves the
// status file, and the rate limit keeps the status from starving the log. A "N log line(s) dropped"
// notice goes into the stream right after the lines that were queued when the first of them was
// dropped.
//
// Lifetime: the worker is a detached thread that owns the shared state, the sink and the status
// writer. A worker still blocked when shutdown's budget runs out is abandoned and may run on after the
// SideChannel is destroyed, so the sink and the status writer must own (or share) everything they use;
// nothing they hold may be a reference to an object the caller destroys.

#include "core/FileIo.h"
#include "core/IBridgeEngine.h"
#include "core/LogSinks.h"
#include "core/StatusStore.h"

#include <chrono>
#include <functional>
#include <memory>

namespace asb {

// Writes one status report; false (with *err) on failure. Called only on the worker thread, possibly
// after the SideChannel is gone (see Lifetime above): it must not capture references to the caller's
// objects.
using StatusWriter = std::function<bool(const StatusReport&, QString* err)>;

// StatusStore::write (AtomicJson, OwnerPolicy::Runtime) into `dir`. The writer shares ownership of `io`
// and `dir`, so an abandoned worker can still use them after the caller has let go of its own.
StatusWriter statusFileWriter(std::shared_ptr<FileIo> io, std::shared_ptr<const DirHandle> dir);

class SideChannel final : public ISideChannel {
public:
    static constexpr int kQueueCapacity = 1024;
    static constexpr int kMaxLineBytes = 512;

    struct Options {
        std::chrono::milliseconds statusInterval{1000};
    };

    // `status` may be empty (no status file, e.g. a CLI process).
    SideChannel(std::unique_ptr<LogSink> sink, StatusWriter status);
    SideChannel(std::unique_ptr<LogSink> sink, StatusWriter status, Options options);
    // shutdown(kShutdownBudget). A worker that is still blocked after that is abandoned: it owns its
    // state and finishes, or dies with the process, on its own.
    ~SideChannel() override;
    SideChannel(const SideChannel&) = delete;
    SideChannel& operator=(const SideChannel&) = delete;

    static constexpr std::chrono::milliseconds kShutdownBudget{2000};

    void log(QtMsgType, const QString&) override;
    void publishStatus(const StatusReport&) override;
    bool flush(std::chrono::milliseconds budget) override;

    // Stops accepting (later log/publishStatus calls are ignored), lets the worker write what is queued
    // and waits at most `budget` for it to finish. False on timeout (master §4.7: the process then ends
    // through Exit::now regardless).
    bool shutdown(std::chrono::milliseconds budget);

    // Lines dropped because the ring was full, since construction.
    quint64 droppedLines() const;

    struct State;

private:
    std::shared_ptr<State> m_state;
};

// The text kept for one line: newlines flattened, cut to at most kMaxLineBytes UTF-8 bytes on a
// character boundary.
QByteArray sideChannelLine(const QString&);

}  // namespace asb
