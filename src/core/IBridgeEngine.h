#pragma once

// The engine seam (WS-3): WS-4 implements both interfaces (BridgeHost, SideChannel) and WS-5's
// BridgeRunner drives them, so the two workstreams meet only here.

#include "core/BridgeConfig.h"
#include "core/StatusStore.h"

#include <QList>
#include <QString>
#include <QtGlobal>

#include <chrono>
#include <functional>

namespace asb {

// The bridging engine: one listener per enabled port, serial device opened only while a client is
// connected (WS-4). Lives on the main thread; every method is called from it.
class IBridgeEngine {
public:
    virtual ~IBridgeEngine() = default;
    // Starts every enabled port of an already validated config. Called at most once.
    virtual void start(const BridgeConfig&) = 0;
    // Stops listening, ends every session and closes every device; then calls `done` (from the event
    // loop, once). The caller then publishes a final status and exits through Exit::now.
    virtual void stop(std::function<void()> done) = 0;
    // The current per-port status, in config order.
    virtual QList<PortStatus> snapshot() const = 0;
};

// All log and status I/O of a process goes through one worker that owns it (S5, D29), so the engine
// thread never blocks on a sink (INV-6). Every method is callable from any thread.
class ISideChannel {
public:
    virtual ~ISideChannel() = default;
    // Never blocks: the line goes into a bounded queue; when it is full the line is dropped and counted.
    virtual void log(QtMsgType, const QString&) = 0;
    // Latest wins: replaces any report the worker has not written yet (Runtime policy, AtomicJson).
    virtual void publishStatus(const StatusReport&) = 0;
    // Waits at most `budget` for everything queued so far to be written; false on timeout.
    virtual bool flush(std::chrono::milliseconds budget) = 0;
};

}  // namespace asb
