#include "core/Barrier.h"

#include <exception>

namespace asb {

QString BarrierEffects::describe() const
{
    QStringList parts;
    if (tokenRevoked) parts << QStringLiteral("start token revoked");
    if (tokenMaybeInvalid) parts << QStringLiteral("start token possibly invalidated");
    if (startsBlocked) parts << QStringLiteral("starts blocked");
    if (stopRequested) parts << QStringLiteral("stop requested");
    return parts.isEmpty() ? QStringLiteral("nothing") : parts.join(QStringLiteral(", "));
}

QString enumName(Barrier::End e)
{
    switch (e) {
    case Barrier::End::Held: return QStringLiteral("Held");
    case Barrier::End::RevokeNotOpened: return QStringLiteral("RevokeNotOpened");
    case Barrier::End::RevokeNotFlushed: return QStringLiteral("RevokeNotFlushed");
    case Barrier::End::BlockFailed: return QStringLiteral("BlockFailed");
    case Barrier::End::StopFailed: return QStringLiteral("StopFailed");
    case Barrier::End::LockMissing: return QStringLiteral("LockMissing");
    case Barrier::End::PairNotMatching: return QStringLiteral("PairNotMatching");
    case Barrier::End::LockInsecure: return QStringLiteral("LockInsecure");
    case Barrier::End::LockFileInsecure: return QStringLiteral("LockFileInsecure");
    case Barrier::End::LockUnsupported: return QStringLiteral("LockUnsupported");
    case Barrier::End::Deadline: return QStringLiteral("Deadline");
    }
    return {};
}

Barrier::Outcome Barrier::run(const Options& o, BarrierEffects& fx, std::unique_ptr<LockHold> alreadyHeld)
{
    Outcome out;
    auto late = [&] { return !o.bestEffort && m_clock.now() > o.deadline; };
    auto end = [&](End e, const QString& err = {}) {
        out.end = e;
        if (out.error.isEmpty()) out.error = err;
        return std::move(out);
    };

    // B0 — the activation fence, before anything else changes (INV-113).
    if (o.revokeToken) {
        QString err;
        switch (m_s.revokeStartToken(m_gate.admit(Step::TokenRevoke), o.deadline, &err)) {
        case IStartToken::Revocation::Done:
            fx.tokenRevoked = true;
            break;
        case IStartToken::Revocation::Absent:
            break;
        case IStartToken::Revocation::NotOpened:
            if (!o.bestEffort) return end(End::RevokeNotOpened, err);
            out.error = err;
            break;
        case IStartToken::Revocation::NotFlushed:
            fx.tokenMaybeInvalid = true;
            if (!o.bestEffort) return end(End::RevokeNotFlushed, err);
            out.error = err;
            break;
        }
        if (late()) return end(End::Deadline);
    }
    // B1 — starts blocked and flushed, BEFORE the stop request (INV-92).
    if (o.blockStarts) {
        try {
            m_s.blockStarts(m_gate.admit(Step::BlockStarts), o.deadline);
            fx.startsBlocked = true;
        } catch (const std::exception& e) {
            if (!o.bestEffort) return end(End::BlockFailed, QString::fromLocal8Bit(e.what()));
            if (out.error.isEmpty()) out.error = QString::fromLocal8Bit(e.what());
        }
        if (late()) return end(End::Deadline);
    }
    // B2 — asynchronous stop request; the lock, not the service state, is the proof.
    try {
        m_s.requestEngineStop(m_gate.admit(Step::StopRequest), o.deadline);
        fx.stopRequested = true;
    } catch (const std::exception& e) {
        if (!o.bestEffort) return end(End::StopFailed, QString::fromLocal8Bit(e.what()));
        if (out.error.isEmpty()) out.error = QString::fromLocal8Bit(e.what());
    }
    if (late()) return end(End::Deadline);
    // B3 — engine.lock.
    if (alreadyHeld) {
        out.hold = std::move(alreadyHeld);
        out.end = End::Held;
        return out;
    }
    LockResult why = LockResult::Busy;
    std::unique_ptr<LockHold> hold = m_s.acquireEngineLock(o.deadline, &why);
    if (!hold) {
        switch (why) {
        case LockResult::Missing:
            out.pair = LockIdentity::classify(LockFileRead{LockFileRead::Status::Missing, {}, {}}, m_s.readRecord());
            return end(End::LockMissing, QStringLiteral("engine.lock does not exist"));
        case LockResult::Insecure:
            return end(End::LockInsecure, QStringLiteral("the engine lock directory is not secured"));
        case LockResult::InsecureFile:
            // install/Repair replace the file after the engine-free proof: as for a mismatched pair.
            out.pair = LockIdentity::PairState::Mismatched;
            return end(End::LockFileInsecure, QStringLiteral("engine.lock has the wrong owner or permissions"));
        case LockResult::Unsupported:
            return end(End::LockUnsupported, QStringLiteral("the lock directory is on an unsupported file system"));
        case LockResult::Acquired:
        case LockResult::Busy:
            break;
        }
        return end(End::Deadline, QStringLiteral("engine.lock is still held"));
    }
    if (late()) {
        out.hold = std::move(hold);   // the caller releases it after failClosed
        return end(End::Deadline);
    }
    // Lock identity through the held handle (D58): never refreshed here.
    out.pair = LockIdentity::classify(m_s.readHeldLock(), m_s.readRecord());
    // The identity reads are calls of the barrier too: a pair judged after the deadline is no proof, so the
    // barrier ends as Deadline (B4), keeping the hold for failClosed.
    if (late()) {
        out.hold = std::move(hold);
        return end(End::Deadline);
    }
    if (out.pair != LockIdentity::PairState::Matching && !o.toleratePair) {
        hold.reset();
        return end(End::PairNotMatching, LockIdentity::describe(out.pair));
    }
    out.hold = std::move(hold);
    out.end = End::Held;
    return out;
}

}  // namespace asb
