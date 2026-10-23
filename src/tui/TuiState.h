#pragma once

// What the TUI shows (WS-12): one plain value the view renders. The controller (TuiApp) owns it and changes it;
// the view (TuiView) only reads it, so the screens are rendered and tested without a terminal.

#include "core/BridgeConfig.h"
#include "core/Interfaces.h"
#include "core/StatusStore.h"
#include "tui/TuiPalette.h"

#include <QString>
#include <QStringList>

#include <array>
#include <optional>

namespace asb::tui {

enum class Screen { Ports, Edit, Confirm, Result, Page };
enum class Tone { Plain, Good, Warn, Bad };

// How the configuration loaded (ReadOnly, master §4.4).
enum class Load {
    Ok,           // trusted folder; the working copy is config.json, or config.pending.json when it is newer
    NotCreated,   // nothing at the data folder yet: an empty configuration
    Untrusted,    // INV-33: nothing in the folder was read
    Error,        // the folder is trusted but a file could not be read; loadError says why
};

// The edit form's fields, in Tab order.
enum class Field { Name, Enabled, Device, Baud, Format, Listen, TcpPort, Allowed, Dtr, Rts };
inline constexpr int kFieldCount = 10;
bool isToggle(Field);

struct EditForm {
    int index = -1;                     // the port being edited; -1: a new one (appended when accepted)
    std::array<QString, kFieldCount> text;   // text fields
    bool enabled = true, dtr = false, rts = false;
    int focus = 0;
    bool changed = false;
    QString error;                      // why Enter did not accept
};

struct TuiState {
    Screen screen = Screen::Ports;

    // The environment.
    bool elevated = false;              // may change things (administrator / root)
    QString readOnlyReason;             // the banner when not elevated (empty when elevated)
    bool windows = false;
    bool utf8 = true;
    ColorMode color = ColorMode::None;
    QString dataFolder;
    QString program;                    // this running program's full path ($APPIMAGE for an AppImage); may be empty
    bool readOnlyExecutable = false;    // the Linux GUI executable (always read only)
    bool appImage = false;              // program is $APPIMAGE
    QString headlessProgram;            // the GUI executable's: this build's headless program beside it, else empty

    // The configuration.
    Load load = Load::NotCreated;
    QString loadError;
    BridgeConfig work;                  // the working copy shown and edited
    bool dirty = false;
    bool pendingPresent = false;        // config.pending.json is the working copy (loaded clean, D48)
    int selected = 0;                   // row in the ports table; == work.ports.size() selects the auto-start row

    // The service.
    bool autoStart = false;             // the working copy of "Start automatically"
    bool autoStartKnown = true;         // false: the installation or the activation could not be read ("[?]")
    QString autoStartWhy;               // why it is not known
    bool installKnown = false;          // detect() answered
    bool installed = false;
    QString serviceLine;                // "running (pid 412)", "not installed", "unknown (...)"
    Tone serviceTone = Tone::Plain;
    bool disabledNote = false;          // installed but without an activation: "The bridge is disabled. ..."
    bool otherBuild = false;            // a complete installation of another build: the notice says how to update it
    std::optional<StatusReport> status;

    // An operation.
    bool busy = false;
    QString busyStep;                   // the Applier's current phase
    QString interim;                    // the "still processing" text (D57); never a result
    bool hangup = false;                // the terminal went away while busy: nothing is drawn
    QString resultPending;              // an operation finished while a form or a question was open: its title

    // The footer: the last thing the user should know.
    QString message;
    Tone messageTone = Tone::Plain;

    // Confirm.
    QString question;
    QString questionShort;              // the decision in one sentence, shown when the whole question does not fit
    bool questionNoActs = false;        // No does something too (keep the installed build and apply): it needs the decision

    // Result.
    QString resultTitle;
    QStringList resultLines;
    Tone resultTone = Tone::Plain;

    // Page (list ports, log, help).
    QString pageTitle;
    QStringList pageLines;
    QList<int> pageHeadings;            // indices into pageLines drawn as headings (the help's sections)
    int pageTop = 0;

    EditForm form;
};

}  // namespace asb::tui
