#pragma once

// The Applier's current phase and its deadline, published as one snapshot {operationId, phase,
// deadline} (D57, INV-118). The Applier writes it on the operation worker as it enters each phase; the
// OperationHost's timer reads it on the host thread and, when the deadline has passed while the
// worker is still inside that phase, delivers one interim notification. The snapshot is guarded by a
// mutex so a reader never sees the phase of one operation with the deadline of another.

#include "core/EngineClaim.h"
#include "core/Interfaces.h"

#include <QString>

#include <mutex>

namespace asb {

enum class Phase { None, Lock, Prologue, Barrier, Install, Stage, Firewall, Commit, Activate, Ack, Cleanup, Folder, Remove };
QString phaseName(Phase);
// The "still processing" text for an interim notification in that phase. What it says about the bridge
// comes from the evidence the operation holds while it waits (engineClaim; WS-5 round 6): "stays stopped"
// only once the stop is proven, never during the prologue, the barrier or the cleanup.
QString interimMessage(Phase, StopEvidence = StopEvidence::None);

struct PhaseSnapshot {
    quint64 operationId = 0;
    Phase phase = Phase::None;
    Deadline deadline{};
    StopEvidence evidence = StopEvidence::None;   // what proves the engine stopped, as of the last native call
};

class PhaseClock {
public:
    void beginOperation(quint64 operationId);
    void enter(Phase, Deadline);
    void setEvidence(StopEvidence);   // the operation's stop evidence changed (between native calls)
    void clear();   // the operation's worker is done (before its final result is posted)
    PhaseSnapshot load() const;

private:
    mutable std::mutex m_m;
    PhaseSnapshot m_s;
};

}  // namespace asb
