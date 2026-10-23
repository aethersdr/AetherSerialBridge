#pragma once

// The Linux --service activation identity (D41, INV-100; WS-8 "cgroup identity, qualified"). Before it reads
// any config or creates the engine, --service reads /proc/self/cgroup and requires EXACTLY one line,
// "0::/system.slice/aether-serial-bridge.service" (cgroup v2, the unified hierarchy). Anything else — a
// hybrid or v1 host (more than one line, or no "0::" line), a user unit (/user.slice/...), another system
// unit, a `systemd-run --scope` scope, a rebased path in a nested cgroup namespace ("0::/") — is refused:
// exit 2. Environment variables ($INVOCATION_ID) are never consulted: they are forgeable.
//
// What this authenticates (N1): "a process placed by THIS host's (or this container's) systemd into the
// managed unit". The paths in /proc/self/cgroup are relative to the process's cgroup namespace; writing a
// process into the unit's cgroup needs write access to its root-owned cgroup.procs (root, not only PID 1),
// and systemd places processes there only when it starts the unit, which it refuses while the unit is
// masked. The supported deployments are a host, or one container whose PID 1 is systemd.

#include <QByteArray>
#include <QString>

namespace asb::posix::ServiceIdentity {

inline constexpr char kExpectedLine[] = "0::/system.slice/aether-serial-bridge.service";
inline constexpr char kRefusal[] = "--service runs only as the aether-serial-bridge system service";
inline constexpr int kExitRefused = 2;

// Pure: the content of /proc/self/cgroup. Exactly kExpectedLine, optionally followed by one "\n".
bool isManagedUnit(const QByteArray& procSelfCgroup);

// Reads /proc/self/cgroup (at most 64 KiB; `path` is injectable for tests) and judges it. A read failure is
// a refusal. `why` describes what was seen.
bool check(QString* why, const char* path = "/proc/self/cgroup");

}  // namespace asb::posix::ServiceIdentity
