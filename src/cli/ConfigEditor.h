#pragma once

// --add-port, --remove-port, --set-enabled and --show-config (so a
// headless station can be set up without hand-editing JSON). Pure: the changes are made to a BridgeConfig in
// memory; where the result is stored is the caller's (WS-9 Commands):
//   - the managed config: through Applier::edit (OperationHost::submitEdit), which loads the pending file (or the
//     committed one), runs the change, validates the whole config and writes config.pending.json; nothing is
//     applied until --apply (master §4.6 Edit);
//   - a scratch config (--config <file>): the file itself, after the same whole-config validation.
// A change that cannot be made throws EditRefused; Applier::edit turns it into NothingChanged, so a refused edit
// never writes anything. Every accepted change is validated as a whole config before it is written.

#include "cli/CommandLine.h"
#include "core/BridgeConfig.h"

#include <QString>
#include <QStringList>

#include <stdexcept>

namespace asb::cli {

// A refused change: nothing was written. what() is the user-facing reason (no trailing full stop).
class EditRefused : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace ConfigEditor {

inline constexpr char kAllInterfaces[] = "0.0.0.0";

struct Format {
    int dataBits = 8;
    Parity parity = Parity::None;
    StopBits stopBits = StopBits::One;
};
// "8N1", "7E2", "8n1.5": data bits 5-8, parity N/E/O/M/S, stop bits 1, 1.5 or 2. Throws EditRefused.
Format parseFormat(const QString&);
// The inverse, as --show-config prints it ("8N1").
QString formatText(const PortMapping&);

// Each throws EditRefused (the config is then unchanged) and otherwise appends what it did, and any advice, to
// *said (one line each, without trailing newline). Names compare without regard to ASCII case.
void addPort(BridgeConfig&, const AddPortArgs&, QStringList* said);
void removePort(BridgeConfig&, const QString& name, QStringList* said);
void setEnabled(BridgeConfig&, const QString& name, bool enabled, QStringList* said);

// Advice about the device path that needs no I/O, and does not open anything (INV-32): a /dev/ttyUSB<n> or
// /dev/ttyACM<n> name may change after a replug.
QStringList deviceNotes(const QString& device);

// --show-config's lines for one config: the ports, then "Problem: ..." for every validation error.
// `program` is the command the hint names ("aether-serial-bridge", "aether-serial-bridge.exe" on Windows).
QStringList describe(const BridgeConfig&, const QString& program = QStringLiteral("aether-serial-bridge"));

// Throws EditRefused naming every problem when the config is not valid as a whole ("Not saved: ...").
void requireValid(const BridgeConfig&);

}  // namespace ConfigEditor
}  // namespace asb::cli
