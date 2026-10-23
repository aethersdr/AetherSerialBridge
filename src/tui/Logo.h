#pragma once

// The logo (master section 6, WS-12 "Logo pipeline"), from the generated src/tui/LogoArt.inc
// (tools/gen_logo_art.py): TrueColor or xterm-256 half-blocks (32 x 16 cells) on a UTF-8 terminal with that colour
// depth, otherwise the 24 x 12 ASCII ramp (NO_COLOR, 16 colours, a console that is not UTF-8).

#include "tui/TuiPalette.h"

#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

namespace asb::tui {

struct LogoSize {
    int width, height;
};

bool logoUsesBlocks(ColorMode, bool utf8);
LogoSize logoSize(ColorMode, bool utf8);
ftxui::Element logoElement(ColorMode, bool utf8);

// The ASCII rows, for tests.
std::vector<std::string> logoAsciiRows();

}  // namespace asb::tui
