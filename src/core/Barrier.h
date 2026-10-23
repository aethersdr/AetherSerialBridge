#pragma once

// The engine barrier (S15, D52, INV-92, INV-113; master §4.6):
//   B0 revoke start.token (only inside a trusted folder)        — before any firewall or config change
//   B1 block starts and flush (not for Stop)
//   B2 request stop (asynchronous; "not running" is not an error)
//   B3 acquire engine.lock, polling until the deadline; then compare the lock id read through the held
//      handle with engine.lock.id (D58)
//   B4 deadline -> the caller reports StateUnknown and runs no step from R3 on
// One deadline covers B0-B3 (30 s for operations, the 10 s cleanup budget for failClosed). A call that
// returns after the deadline ends the barrier as Deadline, whatever it returned; so do lock identity reads
// that return after it: Held is only ever reported inside the deadline.
// B0-B2 are irreversible steps: each is admitted by the operation's IrreversibleGate and made through
// GatedSeams, never through the Platform directly.

#include "core/GatedSeams.h"
#include "core/Interfaces.h"
#include "core/IrreversibleGate.h"
#include "core/LockIdentity.h"

#include <QString>

#include <memory>

namespace asb {

// What the barrier changed so far, for honest messages ("token revoked, starts blocked, stop requested").
struct BarrierEffects {
    bool tokenRevoked = false;
    bool tokenMaybeInvalid = false;   // revocation started but its flush did not complete
    bool startsBlocked = false;
    bool stopRequested = false;
    bool any() const { return tokenRevoked || tokenMaybeInvalid || startsBlocked || stopRequested; }
    QString describe() const;         // "start token revoked, starts blocked, stop requested" / "nothing"
};

class Barrier {
public:
    enum class End {
        Held,             // engine.lock acquired, pair matching (or tolerated); `hold` owns it
        RevokeNotOpened,  // B0: the token could not be opened safely; nothing was changed
        RevokeNotFlushed, // B0: write/flush failed; the token may already be invalid
        BlockFailed,      // B1 threw
        StopFailed,       // B2 threw
        LockMissing,      // B3: no engine.lock (pair Fresh or RecordOnly)
        PairNotMatching,  // B3: acquired, but the lock id is not the recorded one (hold released)
        LockInsecure,     // B3: the lock directory (or anything a replacement would not fix) is not secured
        LockFileInsecure, // B3: engine.lock itself fails its owner/mode check in a trusted directory
        LockUnsupported,
        Deadline,         // B3 timed out, or a call returned after the deadline (B4)
    };
    struct Options {
        bool revokeToken = true;      // the folder inspection said Trust
        bool blockStarts = true;      // false for Stop
        bool toleratePair = false;    // install/Repair: a non-matching pair is reported, not refused
        bool bestEffort = false;      // failClosed: go on to B3 after a B0-B2 failure, ignore lateness
        Deadline deadline{};
    };
    struct Outcome {
        End end = End::Deadline;
        QString error;                // what failed
        LockIdentity::PairState pair = LockIdentity::PairState::Matching;
        std::unique_ptr<LockHold> hold;
    };

    Barrier(GatedSeams& seams, IrreversibleGate& gate, IClock& clock) : m_s(seams), m_gate(gate), m_clock(clock) {}
    // `alreadyHeld`: the caller holds engine.lock (failClosed between R3 and R9); B3 keeps that hold. The
    // caller passes only a hold whose identity matched the record (a validated hold): B3 does not judge it
    // again, and Held is then the "verified" proof (WS-5 round 4).
    Outcome run(const Options&, BarrierEffects&, std::unique_ptr<LockHold> alreadyHeld = nullptr);

private:
    GatedSeams& m_s;
    IrreversibleGate& m_gate;
    IClock& m_clock;
};

QString enumName(Barrier::End);

}  // namespace asb
