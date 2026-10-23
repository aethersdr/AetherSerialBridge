#include "tui/Logo.h"

#include "tui/LogoArt.inc"

namespace asb::tui {

namespace {

using namespace ftxui;

static_assert(sizeof logo_art::kTrueColor / sizeof logo_art::kTrueColor[0] ==
              std::size_t(logo_art::kCellsWide) * std::size_t(logo_art::kCellsHigh) * 2u);
static_assert(sizeof logo_art::kPalette256 / sizeof logo_art::kPalette256[0] ==
              sizeof logo_art::kTrueColor / sizeof logo_art::kTrueColor[0]);
static_assert(sizeof logo_art::kAscii / sizeof logo_art::kAscii[0] == std::size_t(logo_art::kAsciiHigh));

// One pixel's colour at the depth, or nothing where it is transparent.
std::optional<Color> pixel(ColorMode mode, int x, int y)
{
    const int i = y * logo_art::kCellsWide + x;
    if (mode == ColorMode::TrueColor) {
        const int v = logo_art::kTrueColor[i];
        if (v < 0) return std::nullopt;
        return Color::RGB(std::uint8_t((v >> 16) & 0xff), std::uint8_t((v >> 8) & 0xff), std::uint8_t(v & 0xff));
    }
    const int v = logo_art::kPalette256[i];
    if (v < 0) return std::nullopt;
    return Color(static_cast<Color::Palette256>(v));
}

}  // namespace

bool logoUsesBlocks(ColorMode mode, bool utf8)
{
    return utf8 && (mode == ColorMode::TrueColor || mode == ColorMode::Palette256);
}

LogoSize logoSize(ColorMode mode, bool utf8)
{
    if (logoUsesBlocks(mode, utf8)) return {logo_art::kCellsWide, logo_art::kCellsHigh};
    return {logo_art::kAsciiWide, logo_art::kAsciiHigh};
}

Element logoElement(ColorMode mode, bool utf8)
{
    Elements rows;
    if (!logoUsesBlocks(mode, utf8)) {
        for (int y = 0; y < logo_art::kAsciiHigh; ++y) {
            Element row = text(logo_art::kAscii[y]);
            if (const std::optional<Color> c = colorFor(Role::Cyan, mode)) row = row | color(*c);
            rows.push_back(row);
        }
        return vbox(std::move(rows));
    }
    // An upper half block per cell: foreground = the upper pixel, background = the lower one. A transparent pixel
    // takes no colour, so the screen's background shows through.
    for (int cy = 0; cy < logo_art::kCellsHigh; ++cy) {
        Elements cells;
        for (int x = 0; x < logo_art::kCellsWide; ++x) {
            const std::optional<Color> top = pixel(mode, x, 2 * cy);
            const std::optional<Color> bottom = pixel(mode, x, 2 * cy + 1);
            Element cell;
            if (top && bottom) {
                cell = text("▀") | color(*top) | bgcolor(*bottom);
            } else if (top) {
                cell = text("▀") | color(*top);
            } else if (bottom) {
                cell = text("▄") | color(*bottom);
            } else {
                cell = text(" ");
            }
            cells.push_back(std::move(cell));
        }
        rows.push_back(hbox(std::move(cells)));
    }
    return vbox(std::move(rows));
}

std::vector<std::string> logoAsciiRows()
{
    std::vector<std::string> out;
    for (int y = 0; y < logo_art::kAsciiHigh; ++y) out.emplace_back(logo_art::kAscii[y]);
    return out;
}

}  // namespace asb::tui
