#pragma once

// The TUI's colours (master section 6, WS-12 "Colour and terminals"): the aethersdr.com palette, rendered at the
// depth the terminal supports, or not at all. No QtGui: plain constants.
//
// NO_COLOR (non-empty) is a fixed policy (INV-72): the view then applies no colour at all, so the screen emits no
// SGR colour parameter (30-37, 38;..., 39, 40-47, 48;..., 49, 90-97, 100-107). Bold and reverse stay; states stay
// readable as words, and the status dots become '*', 'o' and '!'.

#include <ftxui/screen/color.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace asb::tui {

struct Rgb {
    std::uint8_t r, g, b;
};

namespace palette {
inline constexpr Rgb kBg{0x06, 0x0b, 0x13};
inline constexpr Rgb kCyan{0x5d, 0xe3, 0xff};
inline constexpr Rgb kTeal{0x7b, 0xf2, 0xdc};
inline constexpr Rgb kInk{0xea, 0xf2, 0xfb};
inline constexpr Rgb kMuted{0x85, 0x98, 0xb4};
inline constexpr Rgb kDanger{0xff, 0x4d, 0x4d};
inline constexpr Rgb kWarning{0xff, 0xb8, 0x4d};
}  // namespace palette

// How much colour the screen gets. None = NO_COLOR, or a terminal without colour.
enum class ColorMode { None, Basic16, Palette256, TrueColor };

enum class Role { Bg, Cyan, Teal, Ink, Muted, Danger, Warning };

Rgb rgbOf(Role);

// The colour of a role at a depth; nullopt for ColorMode::None (the view then applies nothing).
std::optional<ftxui::Color> colorFor(Role, ColorMode);

// The nearest xterm-256 index (16-255: the 6x6x6 cube and the grey ramp), by squared RGB distance, ties to the
// lower index: the quantiser tools/gen_logo_art.py uses for the logo.
int nearest256(Rgb);

// NO_COLOR is set to a non-empty value (https://no-color.org).
bool noColorRequested();

// The mode for this process: None under NO_COLOR, otherwise FTXUI's view of the terminal
// (ftxui::Terminal::ColorSupport(), which it computes from $TERM, $COLORTERM and the terminal's replies).
ColorMode currentColorMode();

}  // namespace asb::tui
