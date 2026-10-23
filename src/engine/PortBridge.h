#pragma once

// One serial port published on one TCP port, one client at a time (master §4.3).
// The device is opened only while a client is connected (INV-1); a new client replaces the current one
// (INV-2); the open is tried three times 250 ms apart, and a disconnect with nothing to deliver,
// a replacement or stop() cancels the retries (INV-3); a listener that cannot bind retries
// without affecting other ports (INV-5). Lives on the engine (main) thread; logs only through the
// side channel (INV-6, INV-68).
//
// Liveness (INV-80): every callback captures a QPointer<Session> and starts with isLive(session).
// Qt's disconnect does not retract deliveries already queued, so these checks are what stop a stale
// delivery. They are layered, not a single gate: isLive (the session is current and not ending),
// Pump::live (adds the per-direction state), onDeviceError's own state check, and endSession's
// idempotence. Every path that moves m_current off a session goes through endSession, which sets
// Closing first, so isLive's `m_current == s` and its state check imply each other; each one alone
// would do.

#include "core/IBridgeEngine.h"
#include "engine/ISerialDevice.h"
#include "engine/Session.h"

#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QTimer>

#include <chrono>

namespace asb::engine {

struct EngineTimings {
    std::chrono::milliseconds openRetryDelay{250};      // INV-3
    std::chrono::milliseconds listenRetryDelay{10000};  // INV-5
    std::chrono::milliseconds stallTimeout{5000};       // INV-57
    // Draining: how often the device's native output queue is asked again while it still holds the
    // client's last bytes (nothing signals that it emptied; the query returns at once).
    std::chrono::milliseconds nativeDrainPoll{10};
};
inline constexpr int kOpenAttempts = 3;                 // INV-3

class PortBridge final : public QObject {
    Q_OBJECT

public:
    PortBridge(const asb::PortMapping&, SerialDeviceFactory, asb::ISideChannel&, EngineTimings = {},
               QObject* parent = nullptr);
    ~PortBridge() override;

    // Starts listening (and retrying a failed bind). Called once.
    void start();
    // Stops listening, cancels a listen retry, ends the current session (its open retries included)
    // and closes its device. State Stopped. Synchronous.
    void stop();

    asb::PortStatus snapshot() const { return m_status; }
    const asb::PortMapping& mapping() const { return m_mapping; }

    // The bridge-level liveness check (INV-80): the session is the current one and is not ending.
    bool isLive(const Session* s) const
    {
        return s && m_current == s
            && (s->state == Session::Opening || s->state == Session::Active || s->state == Session::Draining);
    }
    // Ends a session: idempotent (INV-4); detaches it from the bridge FIRST, then disconnects its
    // signals, stops its timers, closes the device, aborts the socket, logs, updates the status and
    // deletes it later. `detail` is the failure text, or the new client's endpoint for Replaced.
    void endSession(Session*, EndReason, const QString& detail = {});

    // Destination progress of a live session's pump (status byte counters).
    void addForwarded(Direction, qint64 k);
    // A Draining session whose client → serial bytes have all left the port (socket buffer, the up
    // pump's pending suffix, the device's write buffer and its native output queue all empty) ends as
    // ClientGone. Called by the up pump after every run; a no-op in any other state.
    void checkDrained(Session*);
    // The session's device's native output queue (ISerialDevice::nativeOutputPending); 0 when the
    // device cannot tell, so the drain then ends once Qt's buffer is empty (the last thing visible).
    qint64 nativePending(const Session*) const;

    // Introspection for tests.
    Session* currentSession() const { return m_current; }
    int listenAttempts() const { return m_listenAttempts; }
    QTcpServer& server() { return m_server; }

private:
    void tryListen();
    void onNewConnection();
    void accept(QTcpSocket*);
    void attemptOpen(const QPointer<Session>&);
    void activate(Session*);
    // `orderly`: the client closed its side (disconnected, or RemoteHostClosedError), rather than a
    // socket error. An orderly close with client → serial bytes still pending drains them first: an
    // Active session at once, an Opening one (Session::clientClosed) once its open succeeds. Any other
    // close ends the session; in Opening that cancels the open retries (INV-3).
    void onSocketGone(const QPointer<Session>&, const QString& why, bool orderly);
    bool upstreamPending(const Session*) const;
    void beginDrain(Session*);
    void onDeviceError(const QPointer<Session>&, SerialError, const QString& message);
    void setState(asb::BridgeState, const std::optional<QString>& client);
    void log(QtMsgType, const QString&);
    QString name() const { return m_mapping.displayName(); }

    const asb::PortMapping m_mapping;
    const SerialDeviceFactory m_factory;
    asb::ISideChannel& m_side;
    const EngineTimings m_timings;

    QTcpServer m_server;
    QTimer m_listenRetry;
    int m_listenAttempts = 0;
    bool m_stopped = false;
    Session* m_current = nullptr;
    asb::PortStatus m_status;
};

}  // namespace asb::engine
