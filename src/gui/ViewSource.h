#pragma once

// What the window shows, read ReadOnly (master §4.4: every viewer, never writes, INV-44). The reads are exactly the
// ones `--status` and `--show-config` make (WS-9 Commands.cpp): TrustedConfig::open(ReadOnly), readCommitted /
// readPending, IServiceControl::detect / query, IStatusReader::read, IStartToken::read; plus the real start type
// for the auto-start box (INV-23), which IServiceControl does not carry (a GUI-owned read, StartType.h).
//
// The window's view Platform is never the one an operation runs on: an operation's Applier works on its own
// Platform on the OperationHost worker (the adapters keep per-object state such as the folder handle of the latest
// inspection), so the 1 s refresh never races it.

#include "core/BridgeConfig.h"
#include "core/Interfaces.h"
#include "core/StatusStore.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace asb::gui {

// The data folder as a ReadOnly viewer finds it.
enum class FolderView {
    Missing,     // nothing at the path yet ("not created yet")
    Trusted,
    Untrusted,   // Recreate / Refuse: nothing inside is read; the window offers Repair (INV-33)
};

struct ConfigView {
    bool present = false;
    QByteArray bytes;                     // exactly as on disk (the source digest is sha256Hex of these)
    std::optional<BridgeConfig> config;   // nullopt: present but unreadable / invalid JSON (error says why)
    QString error;
};

// Every read here can fail, and a failure is never "absent" or "off": `queried` false means the
// installation is UNKNOWN (not "not installed"); a missing `state` on an installed service means its state is unknown
// (not "stopped"); a missing `autoStart` means the start type is unknown (not "disabled").
struct ServiceView {
    bool queried = false;                 // the installation was detected; false = unknown (error says why)
    QString error;                        // why detect failed
    InstallState installation = InstallState::NotInstalled;   // meaningful only when queried
    QString installationDetail;           // the refusal text for Foreign / IncompleteInstall
    std::optional<ServiceQuery> state;    // Current / IncompleteInstall only; nullopt = unknown (stateError says why)
    QString stateError;
    std::optional<bool> autoStart;        // the real start type; nullopt = unknown (false only when known off or absent)
    bool otherBuild = false;              // IncompleteInstall that is complete but another build (Installation::otherBuild)
    QString installedSha256;              // that program's digest (empty when unknown)
    std::optional<QString> installedVersion;   // the installed program's build stamp (WS-14): wording only; nullopt = not read
    bool programRemains = false;          // NotInstalled, the program kept by an unfinished uninstall (Installation::programRemains)

    enum class Presence { Absent, Present, Unknown };
    Presence presence() const
    {
        if (!queried) return Presence::Unknown;
        return installation == InstallState::NotInstalled ? Presence::Absent : Presence::Present;
    }
};

struct Snapshot {
    FolderView folder = FolderView::Missing;
    QString folderReason;                 // why it is untrusted
    ConfigView committed, pending;
    ServiceView service;
    std::optional<StatusReport> status;
    bool tokenPresent = false;            // a valid start.token (the bridge is activated)
    QString tokenWhy;                     // why there is none
    std::optional<FirewallGeneration> tokenGeneration;   // the firewall generation the token names
};

class IViewSource {
public:
    virtual ~IViewSource() = default;
    // Everything (the load and every reload).
    virtual Snapshot load() = 0;
    // Everything about the service: installation (detect: on Windows that reads the installed exe for its digest),
    // state, real start type. At load, after every operation, before Save & apply.
    virtual ServiceView service() = 0;
    // The 1 s refresh: only the state of an installed service (SCM / systemd query), the rest kept from `last`. A
    // service not known to be installed is detected again only when `allowDetect` (never while an operation runs, so
    // the view reads no installation evidence beside an install or uninstall).
    virtual ServiceView serviceState(const ServiceView& last, bool allowDetect) = 0;
    virtual std::optional<StatusReport> status() = 0;
    // Names for the device combo, enumerated without opening anything (INV-32): Windows SERIALCOMM, Linux by-id
    // and the kernel names.
    virtual QStringList serialPorts() = 0;
    // The service's log (bridge.log) through the trusted folder, at most `cap` bytes from its end; nullopt + *why
    // when there is none to show (Linux: the journal).
    virtual std::optional<QString> logTail(qint64 cap, QString* why) = 0;
    // Where the managed config lives (messages).
    virtual QString dataFolder() const = 0;
};

// IViewSource over a Platform (the viewer's own: never the operation worker's).
class PlatformViewSource final : public IViewSource {
public:
    struct Hooks {
        // The start type: Windows SERVICE_AUTO_START, Linux enabled and not masked. nullopt = unknown.
        std::function<std::optional<bool>()> autoStart;
        std::function<QStringList()> serialPorts;
        QString dataFolder;
        bool journalLog = false;          // Linux: the service logs to the journal, not to bridge.log
    };
    PlatformViewSource(Platform& p, Hooks hooks, IClock& clock = systemClock());

    Snapshot load() override;
    ServiceView service() override;
    ServiceView serviceState(const ServiceView& last, bool allowDetect) override;
    std::optional<StatusReport> status() override;
    QStringList serialPorts() override;
    std::optional<QString> logTail(qint64 cap, QString* why) override;
    QString dataFolder() const override { return m_hooks.dataFolder; }

private:
    Platform& m_p;
    Hooks m_hooks;
    IClock& m_clock;
};

// Nothing to show (the --self-test-init window, and a window whose platform could not be built): an empty, trusted,
// not installed view.
class EmptyViewSource final : public IViewSource {
public:
    explicit EmptyViewSource(QString why = {}) : m_why(std::move(why)) {}
    Snapshot load() override;
    ServiceView service() override;
    ServiceView serviceState(const ServiceView& last, bool) override { return last; }
    std::optional<StatusReport> status() override { return std::nullopt; }
    QStringList serialPorts() override { return {}; }
    std::optional<QString> logTail(qint64, QString* why) override;
    QString dataFolder() const override { return {}; }

private:
    QString m_why;
};

// The installation refusal text, as the CLI words it (WS-9 describeInstallation).
QString describeInstallation(const Installation&);

}  // namespace asb::gui
