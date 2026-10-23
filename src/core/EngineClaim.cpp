#include "core/EngineClaim.h"

namespace asb {

QString engineClaim(StopEvidence e, EngineClaim c, const QString& detail)
{
    const bool proven = e != StopEvidence::None;
    switch (c) {
    case EngineClaim::OffSentence:
        return proven ? QStringLiteral("the bridge is off") : QString();
    case EngineClaim::Interim:
        return proven ? QStringLiteral("the bridge stays stopped until it finishes")
                      : QStringLiteral("the result says what state the bridge is in");
    case EngineClaim::StopSentence:
        if (e == StopEvidence::NotInstalled) return QStringLiteral("The service is not installed; nothing to stop.");
        return proven ? QStringLiteral("The bridge is stopped. It stays stopped until Start or Save & apply.")
                      : QStringLiteral("The bridge was not proven stopped.");
    case EngineClaim::Disabled:
    case EngineClaim::StartTypeKept:
    case EngineClaim::StartsNotBlocked:
        break;
    }
    if (e == StopEvidence::NotInstalled) return QStringLiteral("not installed");
    if (!proven) {
        const QString unproven = QStringLiteral("could not prove the bridge stopped");
        return detail.isEmpty() ? unproven : QStringLiteral("%1 (%2)").arg(unproven, detail);
    }
    const QString verified = e == StopEvidence::ValidatedHold ? QStringLiteral("verified: engine lock acquired")
                                                              : QStringLiteral("verified: no engine process");
    switch (c) {
    case EngineClaim::Disabled: return QStringLiteral("stopped and disabled (%1)").arg(verified);
    case EngineClaim::StartTypeKept: return QStringLiteral("stopped (%1); start type unchanged").arg(verified);
    default: return QStringLiteral("stopped (%1), but starts could not be blocked: %2").arg(verified, detail);
    }
}

}  // namespace asb
