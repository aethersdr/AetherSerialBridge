#include "engine/Pump.h"

#include "engine/PortBridge.h"

#include <QMetaObject>

#include <algorithm>

namespace asb::engine {

Pump::Pump(PortBridge& bridge, Session& session, Direction dir, ByteEnd& src, ByteEnd& dst,
           std::chrono::milliseconds stallTimeout, std::chrono::milliseconds nativeDrainPoll)
    : QObject(&session), m_bridge(bridge), m_session(&session), m_dir(dir), m_src(src), m_dst(dst)
{
    m_stall.setSingleShot(true);
    m_stall.setInterval(stallTimeout);
    connect(&m_stall, &QTimer::timeout, this, &Pump::onStallTimeout);
    m_nativePoll.setSingleShot(true);
    m_nativePoll.setInterval(nativeDrainPoll);
    connect(&m_nativePoll, &QTimer::timeout, this, &Pump::pump);
}

// Active: both directions run. Draining (the client closed its side): only the up direction, until
// the client's last bytes have left the port (Qt's buffer and the device's native queue).
bool Pump::live() const
{
    const Session* s = m_session.data();
    if (!m_bridge.isLive(s)) return false;
    return s->state == Session::Active || (s->state == Session::Draining && m_dir == Direction::Up);
}

void Pump::pump()
{
    if (!live()) return;
    if (m_inPump) {   // defensive: queued connections never re-enter (INV-80, §8.5.1)
        m_again = true;
        return;
    }
    Session* const s = m_session.data();
    m_inPump = true;
    do {
        m_again = false;
        if (!m_pending.isEmpty()) {
            const qint64 k = m_dst.write(m_pending.constData(), m_pending.size());
            if (!m_bridge.isLive(s)) break;   // re-check after every call that may emit signals
            if (k < 0) {
                m_bridge.endSession(s, EndReason::WriteFailed,
                                    QStringLiteral("%1 write failed: %2").arg(m_dst.name(), m_dst.lastErrorText()));
                break;
            }
            if (k > 0 && m_dir == Direction::Up) s->upWrote = true;   // the native queue is now ours to wait for
            m_pending.remove(0, k);                 // the pump owns the unwritten suffix (INV-55)
            if (!m_pending.isEmpty()) break;        // wait for the destination's bytesWritten
        }
        if (m_dst.bytesToWrite() >= kCap) break;    // destination full (INV-27)
        const qint64 room = std::min(kChunk, kCap - m_dst.bytesToWrite());
        const qint64 n = m_src.read(m_buf, room);
        if (!m_bridge.isLive(s)) break;
        if (n < 0) {   // -1 is an error, not "empty" (INV-59)
            m_bridge.endSession(s, EndReason::ReadFailed,
                                QStringLiteral("%1 read failed: %2").arg(m_src.name(), m_src.lastErrorText()));
            break;
        }
        if (n == 0) break;
        m_pending = QByteArray(m_buf, n);
    } while (true);
    m_inPump = false;
    if (!m_bridge.isLive(s)) return;   // ended above: its timers are stopped and stay stopped
    updateStall();
    if (s->state == Session::Draining) {
        m_bridge.checkDrained(s);       // everything delivered: the session ends here
        if (!m_bridge.isLive(s)) return;
    }
    if (m_again) QMetaObject::invokeMethod(this, &Pump::pump, Qt::QueuedConnection);
}

void Pump::onBytesWritten(qint64 k)
{
    if (!live()) return;   // a queued delivery for an ended session changes nothing (INV-80)
    m_bridge.addForwarded(m_dir, k);
    if (k > 0) m_stall.start();   // only positive destination progress restarts the deadline (INV-57)
    pump();
}

void Pump::updateStall()
{
    bool outputPending = !m_pending.isEmpty() || m_dst.bytesToWrite() > 0;
    const Session* s = m_session.data();
    if (outputPending || m_dir != Direction::Up || s->state != Session::Draining) {
        m_lastNative = -1;
    } else {
        // Draining, and Qt's buffer is empty: the bytes the device's driver or adapter still holds are
        // output pending too. The session must not close the device under them (a close can cancel
        // them), and the same deadline bounds the wait. Nothing signals that the queue emptied, so it
        // is asked again shortly (never tcdrain / FlushFileBuffers: the engine thread does not block).
        const qint64 native = m_bridge.nativePending(s);
        if (native > 0) {
            outputPending = true;
            if (m_lastNative >= 0 && native < m_lastNative) m_stall.start();   // bytes left the port
            m_lastNative = native;
            m_nativePoll.start();
        } else {
            m_lastNative = -1;
        }
    }
    if (outputPending) {
        if (!m_stall.isActive()) m_stall.start();   // keep a running deadline
    } else {
        m_stall.stop();
    }
}

void Pump::onStallTimeout()
{
    if (!live()) return;
    const qint64 ms = std::chrono::duration_cast<std::chrono::milliseconds>(m_stall.intervalAsDuration()).count();
    const QString span = ms % 1000 == 0 ? QStringLiteral("%1 s").arg(ms / 1000) : QStringLiteral("%1 ms").arg(ms);
    if (m_lastNative > 0) {
        // Only the device's own queue was left, and it did not move: those bytes are lost at the close.
        m_bridge.endSession(m_session.data(), EndReason::Stalled,
                            QStringLiteral("stalled: the %1 device did not finish transmitting (%2 bytes still "
                                           "queued, no progress for %3)")
                                .arg(m_dst.name())
                                .arg(m_lastNative)
                                .arg(span));
        return;
    }
    m_bridge.endSession(m_session.data(), EndReason::Stalled,
                        QStringLiteral("stalled: the %1 side accepted no data for %2").arg(m_dst.name(), span));
}

}  // namespace asb::engine
