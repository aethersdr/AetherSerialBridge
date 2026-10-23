#pragma once

// The command line of both executables (master §4.10, WS-9; INV-26). Pure: parse() looks at nothing but its
// arguments, so every rule is testable without a process, a terminal or a platform.
//
// Syntax. Exactly one command; options may come before or after it, in any order:
//   --install [--force-recreate]   --apply [--force-recreate]   --start   --stop   --uninstall [--remove-settings]
//   --repair [--yes]               --status [--config <path>]    --show-config [--config <path>]
//   --list-ports                   --run --config <path>         --discard-pending
//   --add-port <name> <device> <tcp-port> [--baud <n>] [--format 8N1] [--bind <ip>] [--allow <list>] [--config <path>]
//   --remove-port <name> [--config <path>]       --set-enabled <name> on|off [--config <path>]
//   --help | -h | /?   --version   --gui   --tui   (and the hidden --service, --helper, --self-test-init, and on
//   Windows --uninstall-from-apps [--confirmed], the Apps & features entry's UninstallString: WS-16)
// Command and option names match without regard to ASCII case. A value never starts with
// "--": "--remove-port --stop" is "--remove-port needs a port name", not a port named "--stop".
//
// Usage errors (exit 2), each with a message that names the problem:
//   - no command, an unknown argument, a missing or malformed value;
//   - two commands ("--install and --apply cannot be combined");
//   - an option given twice ("--config was given twice"), never "the last one wins";
//   - an option the command does not take: --force-recreate only with --install/--apply, --yes only with
//     --repair, --remove-settings only with --uninstall, --confirmed only with --uninstall-from-apps,
//     --baud/--format/--bind/--allow only with --add-port, and --config only with --run, --status,
//     --show-config and the three port edits (the service commands always use the installed config.json:
//     INV-26, INV-90);
//   - --run without --config (the installed config runs only as the service);
//   - --help, --version, --gui, --tui, --service, --helper and --self-test-init take nothing else.
// What parse() cannot know — whether the --config path is the managed config.json or inside the data folder,
// whether this build has a GUI — is decided by the caller (Dispatch, runCommand).

#include <QString>
#include <QStringList>

#include <optional>

namespace asb::cli {

enum class Command {
    None,
    Help,
    Version,
    Gui,
    Tui,
    Install,
    Apply,
    Start,
    Stop,
    Uninstall,
    Repair,
    Status,
    ShowConfig,
    ListPorts,
    Run,
    Service,
    AddPort,
    RemovePort,
    SetEnabled,
    DiscardPending,
    Helper,
    SelfTestInit,
    UninstallFromApps,   // WS-16 (D63, D64): hidden, the console exe on Windows only
};

// The command's own spelling ("--add-port"), for messages.
QString commandName(Command);

// --add-port's values, as typed; checked as a whole config by the editor (the port's numbers are validated by
// BridgeConfig::validate, not here). `tcpPort` and `baud` are digits-only.
struct AddPortArgs {
    QString name, device;
    int tcpPort = 0;
    int baud = 38400;
    QString format = QStringLiteral("8N1");
    std::optional<QString> bind;   // absent: every interface, with a warning
    QString allow;
};

struct Parsed {
    Command command = Command::None;
    bool forceRecreate = false;
    bool yes = false;
    bool removeSettings = false;     // --uninstall --remove-settings (WS-16, INV-125)
    bool confirmed = false;          // --uninstall-from-apps --confirmed: the elevated half of the Apps & features uninstall
    // --gui --intent=<install|update|uninstall|none> (v1.1 WS-17 §3): Restart as administrator's relaunch names the
    // action whose confirmation the elevated window opens once loaded. Only with --gui; any other word is a usage error.
    std::optional<QString> intent;
    std::optional<QString> config;   // --config <path>, as typed
    AddPortArgs addPort;
    QString portName;                // --remove-port, --set-enabled
    bool enable = false;             // --set-enabled
    QString usageError;              // non-empty: exit 2 with this message

    bool ok() const { return usageError.isEmpty(); }
};

Parsed parse(const QStringList& args);

// Changes the service, the firewall or the managed configuration: needs elevation, runs through the Applier,
// and enters the process-creation guard first (INV-112). The edits count only on the managed config.
bool isMutating(Command, bool scratchConfig);

}  // namespace asb::cli
