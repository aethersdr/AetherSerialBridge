#pragma once

// The service configuration and the start-type steps, pure (no sc.exe: WS-7 applies the Windows plan through the SCM API and WS-8 the
// Linux one over D-Bus). The service is always created or reconfigured with starts BLOCKED (disabled /
// masked); starts are allowed only in R9, after the firewall matches the committed config. Every
// start-type change is followed by its durability barrier before anything else happens (INV-77):
// Windows RegFlushKey on the service key, Linux fsync of /etc/systemd/system after the mask link.

#include <QList>
#include <QString>

namespace asb::ServicePlan {

enum class Os { Windows, Linux };

// Windows SCM configuration (WS-7 applies it with CreateServiceW / ChangeServiceConfigW /
// ChangeServiceConfig2W).
inline constexpr char kServiceName[] = "AetherSerialBridge";
inline constexpr char kDisplayName[] = "Aether Serial Bridge";
inline constexpr char kDescription[] =
    "Publishes serial ports (amplifiers, tuners) on TCP for AetherSDR. Config: "
    "%ProgramData%\\AetherSerialBridge\\config.json";
// Linux unit name (WS-8).
inline constexpr char kUnitName[] = "aether-serial-bridge.service";

struct FailureAction {
    // Every SC_ACTION type, so a read-back keeps what SCM really holds: a list with a reboot or a command among the
    // restarts never compares equal to the plan (WS-7 merge note, carried by WS-9).
    enum class Type { None, Restart, Reboot, RunCommand, Other } type = Type::Restart;
    int delayMs = 0;
    bool operator==(const FailureAction&) const = default;
};

struct ScmConfig {
    QString serviceName, displayName, description;
    QString binaryPath;                  // "\"<exe>\" --service"
    QString account;                     // LocalSystem
    enum class StartType { Disabled, Automatic } startType = StartType::Disabled;
    QList<FailureAction> failureActions; // restart 5 s / 5 s / 30 s
    int failureResetSeconds = 0;         // 86400
    bool failureActionsOnNonCrashFailures = false;   // the failure flag: also on a non-zero exit code
};
ScmConfig windowsConfig(const QString& installedExe);

struct Step {
    enum class Kind {
        // Windows
        SetStartTypeDisabled, SetStartTypeAutomatic, FlushServiceKey,
        // Linux (D-Bus)
        MaskUnit, DisableUnit, UnmaskUnit, EnableUnit, ReloadManager, SyncUnitDirectory,
    } kind;
    bool operator==(const Step&) const = default;
};
bool changesStartType(Step::Kind);
bool isDurabilityBarrier(Step::Kind);

// B1 (and install): starts blocked, durably.
QList<Step> blockStarts(Os);
// R9: starts allowed, durably.
QList<Step> allowStarts(Os);

}  // namespace asb::ServicePlan
