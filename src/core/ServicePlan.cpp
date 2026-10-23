#include "core/ServicePlan.h"

namespace asb::ServicePlan {

ScmConfig windowsConfig(const QString& installedExe)
{
    ScmConfig c;
    c.serviceName = QString::fromLatin1(kServiceName);
    c.displayName = QString::fromLatin1(kDisplayName);
    c.description = QString::fromLatin1(kDescription);
    c.binaryPath = QStringLiteral("\"%1\" --service").arg(installedExe);
    c.account = QStringLiteral("LocalSystem");
    c.startType = ScmConfig::StartType::Disabled;
    c.failureActions = {{FailureAction::Type::Restart, 5000},
                        {FailureAction::Type::Restart, 5000},
                        {FailureAction::Type::Restart, 30000}};
    c.failureResetSeconds = 86400;
    c.failureActionsOnNonCrashFailures = true;
    return c;
}

bool changesStartType(Step::Kind k)
{
    switch (k) {
    case Step::Kind::SetStartTypeDisabled:
    case Step::Kind::SetStartTypeAutomatic:
    case Step::Kind::MaskUnit:
    case Step::Kind::DisableUnit:
    case Step::Kind::UnmaskUnit:
    case Step::Kind::EnableUnit:
        return true;
    default:
        return false;
    }
}

bool isDurabilityBarrier(Step::Kind k)
{
    return k == Step::Kind::FlushServiceKey || k == Step::Kind::SyncUnitDirectory;
}

QList<Step> blockStarts(Os os)
{
    if (os == Os::Windows) return {{Step::Kind::SetStartTypeDisabled}, {Step::Kind::FlushServiceKey}};
    return {{Step::Kind::MaskUnit}, {Step::Kind::DisableUnit}, {Step::Kind::ReloadManager},
            {Step::Kind::SyncUnitDirectory}};
}

QList<Step> allowStarts(Os os)
{
    if (os == Os::Windows) return {{Step::Kind::SetStartTypeAutomatic}, {Step::Kind::FlushServiceKey}};
    return {{Step::Kind::UnmaskUnit}, {Step::Kind::EnableUnit}, {Step::Kind::ReloadManager},
            {Step::Kind::SyncUnitDirectory}};
}

}  // namespace asb::ServicePlan
