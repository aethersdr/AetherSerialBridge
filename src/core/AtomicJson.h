#pragma once

// Atomic, exclusive, durable file replacement through the FileIo seam (INV-15, INV-77, INV-96).
// A unique temp created exclusively, renamed over the target, removed on failure; create-time ownership by
// policy and durability barriers.

#include "core/FileIo.h"

#include <QByteArray>
#include <QString>

namespace asb {

// The temp-name suffix every bridge temp carries (P5 removes stale ones; WS-6 treats the pattern as a
// known file that must be regular).
inline constexpr char kTempSuffix[] = ".asbtmp";

// "<base>.<32 lower-case hex digits from the system CSPRNG>.asbtmp", a fresh name on every call.
NameGen defaultNameGen(const QString& base = QStringLiteral("asb"));
// True for a name defaultNameGen could have produced (any base), i.e. one of our temps.
bool isTempName(const QString& name);

struct WriteResult {
    bool replaced = false;   // the target now holds `bytes`
    bool durable = false;    // ... and the directory sync after the rename succeeded
    QString error;
};

// createTemp → write → flush → commitAs. Before the rename, a failure leaves the target untouched and
// the temp removed (TempFile's destructor); after it, `replaced` is true and `durable` says whether the
// directory sync succeeded. `policy` is passed through to createTemp unchanged. With no generator, the
// temp is named "<name>.<hex>.asbtmp".
WriteResult atomicWrite(FileIo&, const DirHandle&, const QString& name, const QByteArray& bytes, OwnerPolicy policy,
                        const NameGen& gen = {});

}  // namespace asb
