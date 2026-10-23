#include "core/AceJudge.h"

#include "core/DataDirectoryTrust.h"

namespace asb {

bool TrustPolicy::isTrustedOwner(const std::optional<QString>& sid) const
{
    if (!sid || sid->isEmpty()) return false;
    return trustedOwners.contains(*sid) || (!alsoTrusted.isEmpty() && *sid == alsoTrusted);
}

TrustPolicy TrustPolicy::windows(const QString& alsoTrusted)
{
    return TrustPolicy{{QString::fromLatin1(DataDirectoryTrust::kLocalSystemSid),
                        QString::fromLatin1(DataDirectoryTrust::kAdministratorsSid)},
                       alsoTrusted};
}

namespace AceJudge {

bool isUntrusted(const AceInfo& ace, const TrustPolicy& policy)
{
    switch (ace.kind) {
    case AceKind::Unknown:
        return true;   // undecodable: never trusted, whatever the SID or mask
    case AceKind::Deny:
        return false;  // a deny never widens access
    case AceKind::Allow:
        break;
    }
    return (ace.rights & Dangerous) != 0 && !policy.isTrustedOwner(ace.sid);
}

bool anyUntrusted(const QList<AceInfo>& acl, const TrustPolicy& policy)
{
    for (const AceInfo& a : acl)
        if (isUntrusted(a, policy)) return true;
    return false;
}

}  // namespace AceJudge
}  // namespace asb
