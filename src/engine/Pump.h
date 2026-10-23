#pragma once

// One direction of a session (master §4.3, S8). Entered only from the event loop: every signal that
// drives it is connected with Qt::QueuedConnection (INV-80), so a signal emitted inside a write() or
// read() call is delivered after the pump returns, never re-entrantly. The pump owns the unwritten
// suffix of the last chunk (INV-55), never reads while the destination holds kCap unwritten bytes
// (INV-27), and has its own stall deadline (INV-57).

#include "engine/Session.h"

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <chrono>

namespace asb::engine {

class PortBridge;

class Pump final : public QObject {
    Q_OBJECT

public:
    Pump(PortBridge& bridge, Session& session, Direction dir, ByteEnd& src, ByteEnd& dst,
         std::chrono::milliseconds stallTimeout, std::chrono::milliseconds nativeDrainPoll);

    Direction direction() const { return m_dir; }
    qint64 pendingBytes() const { return m_pending.size(); }
    bool stallArmed() const { return m_stall.isActive(); }
    // Remaining time of the stall deadline (-1 when disarmed), for tests.
    int stallRemainingMs() const { return m_stall.isActive() ? m_stall.remainingTime() : -1; }

    // Stops the stall timer and the native-queue poll; called by PortBridge::endSession.
    void halt()
    {
        m_stall.stop();
        m_nativePoll.stop();
    }

public slots:
    // Moves bytes until the source is empty, the destination is full, or an error ends the session.
    void pump();
    // The destination reported k bytes written. Only k > 0 is progress: it restarts the deadline.
    void onBytesWritten(qint64 k);

private:
    bool live() const;
    // Output pending (unwritten suffix or destination buffer; while Draining, also the device's native
    // output queue) → arm the deadline if it is not armed (keeping a running deadline); nothing
    // pending → disarm. Never restarts a running deadline, except when the native queue shrank: bytes
    // left the port, which is destination progress like a positive bytesWritten.
    void updateStall();
    void onStallTimeout();

    PortBridge& m_bridge;
    QPointer<Session> m_session;
    const Direction m_dir;
    ByteEnd& m_src;
    ByteEnd& m_dst;
    QByteArray m_pending;
    bool m_inPump = false;
    bool m_again = false;
    QTimer m_stall;
    QTimer m_nativePoll;        // Draining (up): asks the native queue again; runs pump()
    qint64 m_lastNative = -1;   // Draining (up): the native queue at the last look; -1 = not looking
    char m_buf[kChunk];
};

}  // namespace asb::engine
