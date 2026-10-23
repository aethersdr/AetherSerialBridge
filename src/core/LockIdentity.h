#pragma once

// Engine-lock identity (D55, D58, INV-116): a random 128-bit id inside engine.lock, recorded in
// engine.lock.id. Device numbers are not stable across reboot, so nothing compares st_dev/st_ino.
// Pure parts (format, parse, pair-state classification, the recovery each state needs) plus the one
// publication primitive, over the FileIo seam of the lock directory: temp, write, file flush, rename,
// directory flush (master §4.6 "Lock identity"). The Applier decides WHEN to publish (install/Repair
// only, after the engine-free proof); the engine compares the ids after acquiring the lock (exit 4).

#include "core/AtomicJson.h"
#include "core/Interfaces.h"

#include <QByteArray>
#include <QString>

#include <optional>

namespace asb::LockIdentity {

inline constexpr char kLockName[] = "engine.lock";
inline constexpr char kRecordName[] = "engine.lock.id";
inline constexpr qint64 kMaxBytes = 64;

QByteArray newId();                                   // 32 lower-case hex digits from the system CSPRNG
QByteArray format(const QByteArray& id);              // {"lockId":"<id>"}
std::optional<QByteArray> parse(const QByteArray&);   // exactly that form, <= 64 bytes; anything else nullopt

// Master §4.6 pair-state table, plus the README v10 row "record present / lock missing" (RecordOnly).
enum class PairState {
    Fresh,        // neither file (temps may exist)
    Partial,      // engine.lock valid, engine.lock.id missing (crash between the two publications)
    RecordOnly,   // engine.lock.id present, engine.lock missing
    Mismatched,   // both present but ids differ, or either is corrupt/oversized/wrong owner
    Matching,     // both valid, ids equal
    Unknown,      // engine.lock exists but cannot be read without holding it (Windows): B3 decides
};
PairState classify(const LockFileRead& lock, const LockFileRead& record);
QString describe(PairState);

// What install/Repair do about each state, always after the engine-free proof:
//  Fresh       publish a new lock, then its record
//  Partial     acquire the lock, publish the record from the id read THROUGH the held lock
//  RecordOnly  publish a lock with a NEW id, then replace the record (never re-create the recorded
//              id: an old engine may still hold an unlinked inode with that id)
//  Mismatched  publish a new lock, then replace the record
//  Matching    nothing (reinstall over a matching pair is a no-op)
enum class Recovery { None, PublishLockThenRecord, PublishRecordFromHeldLock, ReplaceBoth };
Recovery recoveryFor(PairState);

// One publication: atomicWrite(name, {"lockId":id}, Config policy). Complete only when both the
// rename and the directory flush succeeded.
WriteResult publish(FileIo&, const DirHandle&, const QString& name, const QByteArray& id);

}  // namespace asb::LockIdentity
