#pragma once

// What a result, a service message or an interim notification may say about the engine (WS-5 round 6,
// "stopped, not activated" was claimed after the lock identity had been rejected).
//
// Every "stopped", "verified", "no engine process" and "the bridge is off" in the core is worded here, and
// only from the evidence passed in. The Applier derives that evidence from the same Proofs the
// IrreversibleGate admits steps on (Proofs::stopEvidence, which is also the gate's EngineStopped
// derivation), at the moment the text is made. Without evidence the wording says the stop is not proven.
// tst_IrreversibleGate::stopClaimsOnlyFromTheProof scans the core for stop wording anywhere else.

#include <QString>

namespace asb {

enum class StopEvidence {
    None,           // nothing proves the engine stopped
    ValidatedHold,  // engine.lock held by this operation, its id read through the hold matched the record
    EngineFree,     // the engine-free proof: starts blocked by this operation, service stopped, no process
    NotInstalled,   // no service was detected, and this operation installed none
};

enum class EngineClaim {
    Disabled,           // a postcondition after starts were blocked: "stopped and disabled (verified: ...)"
    StartTypeKept,      // Stop's postcondition: "stopped (verified: ...); start type unchanged"
    StartsNotBlocked,   // failClosed whose B1 failed; `detail` is the error
    StopSentence,       // Stop's message
    OffSentence,        // the untick's clause "the bridge is off"; empty without evidence
    Interim,            // the clause of an interim notification
};

// `detail`: StartsNotBlocked's error; without evidence, what was established instead (in parentheses).
QString engineClaim(StopEvidence, EngineClaim, const QString& detail = {});

}  // namespace asb
