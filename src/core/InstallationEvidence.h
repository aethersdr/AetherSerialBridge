#pragma once

// Installation-state detection from read-only evidence (D33, INV-98), pure. The adapters (WS-7
// Windows, WS-8 Linux) gather the evidence without executing anything — the service's binPath and the
// installed program's digest; the unit files' content — and these functions classify it. A service or
// unit this program did not write, and any inconclusive evidence, is Foreign (refused).

#include "core/Interfaces.h"

#include <QByteArray>
#include <QString>

#include <optional>

namespace asb::InstallationEvidence {

// ---- Windows ------------------------------------------------------------------------------------
struct WindowsEvidence {
    bool serviceExists = false;
    QString binPath;                  // QueryServiceConfigW lpBinaryPathName
    QString expectedQtExe;            // %ProgramFiles%\AetherSerialBridge\aether-serial-bridge.exe
    bool qtExeDigestOk = false;       // the installed copy's digest is the build's (INV-41)
    QString installedSha256;          // the installed copy's digest, when it was read whole (empty: missing/unreadable)
    bool lockFilesPresent = false;    // engine.lock and engine.lock.id
    // What of the SCM configuration --install writes is missing or different (empty: all of it is there). Judged
    // for the Qt service only: a service created and then left half configured (a failed or late reinstall) is
    // IncompleteInstall, never Current (WS-7 review round 3).
    QString scmConfigProblem;
    // The installed program's build stamp (D61), copied to Installation::installedVersion for wording only; never an
    // input to the classification (INV-120).
    std::optional<QString> installedVersion;
    // WS-16 (D63): whether the Apps & features entry exists. Recorded for diagnostics only and NEVER classified: a v1.0
    // install has no entry and must stay Current (an "unfinished" one would be replaced without asking).
    bool appsEntryPresent = false;
    // The installed Qt program exists at expectedQtExe, read with or without a service: with no
    // service it is Installation::programRemains, an uninstall that kept the program. Never an input to the state.
    bool qtExePresent = false;
};
Installation classifyWindows(const WindowsEvidence&);

// ---- systemd (Linux) -----------------------------------------------------------------------------
inline constexpr char kQtUnitHeader[] = "# Written by aether-serial-bridge (Qt) --install";

struct LinuxEvidence {
    // /etc/systemd/system/aether-serial-bridge.service: where systemd keeps the mask link (a regular file
    // there is not ours).
    enum class EtcUnit { Absent, MaskLink, RegularFile, Other } etcUnit = EtcUnit::Absent;
    QByteArray etcUnitContent;
    // /usr/local/lib/systemd/system/aether-serial-bridge.service
    bool qtUnitPresent = false;
    QByteArray qtUnitContent;
    bool binaryPresent = false;       // /usr/local/bin/aether-serial-bridge, a regular file (never a link)
    bool lockFilesPresent = false;    // engine.lock and engine.lock.id
    // Build identity (v1.1 D61, INV-120), as on Windows: the installed program's SHA-256 against this build's.
    bool thisBuildKnown = false;      // the caller passed this build's digest; without it nothing is Current
    bool programDigestOk = false;     // the installed program was read whole and its digest is this build's
    bool programUnreadable = false;   // present, but not read whole (not root's, over the cap, a read error)
    QString installedSha256;          // its digest when it was read whole (empty: missing or unreadable)
    std::optional<QString> installedVersion;   // its build stamp: wording only, never an input (INV-120)
};
Installation classifyLinux(const LinuxEvidence&);

}  // namespace asb::InstallationEvidence
