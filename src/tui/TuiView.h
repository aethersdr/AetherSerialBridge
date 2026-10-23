#pragma once

// The TUI's screens (master section 6, WS-12): TuiState in, one FTXUI element of exactly width x height cells out.
// Pure and deterministic: no clock, no paths beyond what the state holds, so the screens are tested as text
// (render goldens at 80 x 24 and 40 x 20).
//
// Layout tiers (section 6.3): >= 60 columns with room for it, the logo beside the header; < 60 columns no logo, a
// one-line header and no bind column; < 40 columns a port is its name and state; < 12 rows no header.

#include "tui/TuiState.h"

#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

namespace asb::tui {

ftxui::Element render(const TuiState&, int width, int height);

// The same screen as plain text: `height` lines of exactly `width` cells (tests and goldens).
std::vector<std::string> renderText(const TuiState&, int width, int height);

// The command that runs this program's TUI elevated, quoted to run as shown: sudo '<program>' --tui on Linux,
// "<program>" --tui on Windows (in an administrator terminal). Without a known path, the program's name.
//
// The Linux GUI executable (readOnlyExecutable) never names itself: its elevated TUI is the window again. From the
// AppImage: sudo "<AppImage>" --install (cli::staged::terminalCommand, INV-124), which installs this build; beside
// this build's headless program: sudo '<that program>' --tui; otherwise empty (no command: the window updates it).
QString elevatedCommand(const TuiState&);

// $APPIMAGE when this process runs from that AppImage's mount (`ownPath` lies under a non-empty $APPDIR), else empty:
// an $APPIMAGE or $APPDIR inherited from another AppImage is not this program.
QString appImageOf(const QString& appImage, const QString& appDir, const QString& ownPath);

// The GUI executable's read-only banner (TuiText.h kReadOnlyGuiExecutable*): the same command elevatedCommand() names.
QString readOnlyGuiExecutableBanner(const TuiState&);

// What the auto-start row is called on this OS.
QString autoStartLabel(bool windows);

// The Confirm screen shows its whole decision (the question, or its one-sentence decision) at this size. When it
// does not, y is not offered and the controller ignores it.
bool decisionVisible(const TuiState&, int width, int height);

// The largest useful Page scroll offset at this size: the wrapped body minus the rows it gets, at least 0.
int pageMaxTop(const TuiState&, int width, int height);

// Word-wraps `text` to lines of at most `width` cells (a longer word is cut).
std::vector<std::string> wrap(const std::string& text, int width);

}  // namespace asb::tui
