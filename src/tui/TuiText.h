#pragma once

// The TUI's fixed sentences (master section 6; the GUI's where the two share one, WS-10 5.5). One place, so the
// tests name the exact text the screens show.

namespace asb::tui::text {

inline constexpr char kTitle[] = "Aether Serial Bridge";
inline constexpr char kSubtitle[] = "Serial gear on the network for AetherSDR: one port, one client.";

// Section 6.1: the banner when not elevated; every key that would change something refuses with it.
inline constexpr char kReadOnly[] = "Read-only: run as administrator / with sudo to change ports";
// The Linux GUI executable shows the TUI read only: it never runs the Applier (WS-9, D2). Its banner names the same
// command elevatedCommand() (TuiView.h) does, so the banner and the update notice never disagree:
//   - beside this build's headless program: %1 is sudo '<that program>' --tui;
//   - from the AppImage: %1 is sudo "<AppImage>" --install (the window's own terminal command, INV-124), which installs
//     this build; the AppImage itself has no elevated TUI (AppRun runs the window for --tui: a loop);
//   - otherwise this program has no command to name: the window does it.
inline constexpr char kReadOnlyGuiExecutable[] = "Read-only: this is the GUI executable; run %1 to change ports";
inline constexpr char kReadOnlyAppImage[] =
    "Read-only: this is the GUI executable; to change ports, run sudo aether-serial-bridge --tui, after %1 if this "
    "build isn't the one installed";
inline constexpr char kReadOnlyGuiExecutableAlone[] =
    "Read-only: this is the GUI executable, which can't change ports or update the bridge; use the window";

// INV-33 (the GUI's footer).
inline constexpr char kUntrusted[] = "Config not loaded: the data folder isn't trusted.";
inline constexpr char kUntrustedEdit[] = "Config not loaded: the data folder isn't trusted. Press R to repair it first.";

inline constexpr char kPendingNote[] = "Saved, not applied (config.pending.json): s applies it, D discards it.";
inline constexpr char kDisabledNote[] = "The bridge is disabled. Save & apply to start it.";
inline constexpr char kUnsaved[] = "Unsaved changes. Press s to Save & apply.";
inline constexpr char kNoPorts[] = "No ports yet. Press a to add one.";
inline constexpr char kNothingChanged[] = "Nothing was changed.";

// D57 / INV-118: while an operation is outstanding the TUI does not quit (q, Ctrl+C) and starts nothing else.
inline constexpr char kBusyQuit[] = "An operation is still finishing; you can quit when it ends.";
inline constexpr char kBusyAction[] = "An operation is still finishing; try again when it ends.";

// A complete installation of another build (user report 2026-10-20): the header's notice.
inline constexpr char kOtherBuildNote[] = "A different build of the bridge is installed. Press u to update it to this one.";
// Read-only: %1 is elevatedCommand() (TuiView.h), this running program by its full path (a
// bare "aether-serial-bridge" runs the installed, other build).
inline constexpr char kOtherBuildNoteWindows[] =
    "A different build of the bridge is installed. To update it, run %1 in an administrator terminal.";
inline constexpr char kOtherBuildNoteLinux[] = "A different build of the bridge is installed. To update it, run %1.";
// The GUI executable with no command to name (elevatedCommand() is empty): the window's button.
inline constexpr char kOtherBuildNoteWindowOnly[] =
    "A different build of the bridge is installed. This program can't update it: use Update to this build in the window.";
inline constexpr char kUpdateWindowOnly[] = "This program can't update the bridge: use Update to this build in the window.";
// Elevated, but u would refuse: why, instead of "Press u".
inline constexpr char kOtherBuildNoteAutoStart[] =
    "A different build of the bridge is installed. Turn auto-start on to update it.";
inline constexpr char kOtherBuildNoteUnloaded[] =
    "A different build of the bridge is installed. It can be updated once the settings load.";

inline constexpr char kQuitDirty[] = "Discard unsaved changes and quit?";
inline constexpr char kReloadDirty[] = "Discard unsaved changes and reload?";
inline constexpr char kDiscardPort[] = "Discard the changes to this port?";
inline constexpr char kDiscardPending[] =
    "Discard the saved, not applied settings (config.pending.json)? The bridge keeps running the applied ones.";
inline constexpr char kSupersede[] =
    "The saved, not applied settings (config.pending.json) changed since they were loaded. Replace them with yours?";

}  // namespace asb::tui::text
