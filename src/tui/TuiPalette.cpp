#include "tui/TuiPalette.h"

#include <ftxui/screen/terminal.hpp>

#include <QByteArray>
#include <QtGlobal>

namespace asb::tui {

Rgb rgbOf(Role r)
{
    switch (r) {
    case Role::Bg: return palette::kBg;
    case Role::Cyan: return palette::kCyan;
    case Role::Teal: return palette::kTeal;
    case Role::Ink: return palette::kInk;
    case Role::Muted: return palette::kMuted;
    case Role::Danger: return palette::kDanger;
    case Role::Warning: return palette::kWarning;
    }
    return palette::kInk;
}

int nearest256(Rgb c)
{
    static constexpr int kLevels[6] = {0, 95, 135, 175, 215, 255};
    int best = 16;
    long bestD = -1;
    auto consider = [&](int index, int r, int g, int b) {
        const long d = long(c.r - r) * (c.r - r) + long(c.g - g) * (c.g - g) + long(c.b - b) * (c.b - b);
        if (bestD < 0 || d < bestD) {
            bestD = d;
            best = index;
        }
    };
    for (int i = 0; i < 216; ++i) consider(16 + i, kLevels[i / 36], kLevels[(i / 6) % 6], kLevels[i % 6]);
    for (int i = 0; i < 24; ++i) consider(232 + i, 8 + 10 * i, 8 + 10 * i, 8 + 10 * i);
    return best;
}

std::optional<ftxui::Color> colorFor(Role role, ColorMode mode)
{
    switch (mode) {
    case ColorMode::None: return std::nullopt;
    case ColorMode::TrueColor: {
        const Rgb c = rgbOf(role);
        return ftxui::Color::RGB(c.r, c.g, c.b);
    }
    case ColorMode::Palette256:
        return ftxui::Color(static_cast<ftxui::Color::Palette256>(nearest256(rgbOf(role))));
    case ColorMode::Basic16:
        switch (role) {
        case Role::Bg: return ftxui::Color(ftxui::Color::Black);
        case Role::Cyan: return ftxui::Color(ftxui::Color::CyanLight);
        case Role::Teal: return ftxui::Color(ftxui::Color::Cyan);
        case Role::Ink: return ftxui::Color(ftxui::Color::White);
        case Role::Muted: return ftxui::Color(ftxui::Color::GrayLight);
        case Role::Danger: return ftxui::Color(ftxui::Color::RedLight);
        case Role::Warning: return ftxui::Color(ftxui::Color::YellowLight);
        }
        break;
    }
    return std::nullopt;
}

bool noColorRequested()
{
    return !qEnvironmentVariableIsEmpty("NO_COLOR");
}

ColorMode currentColorMode()
{
    if (noColorRequested()) return ColorMode::None;
    switch (ftxui::Terminal::ColorSupport()) {
    case ftxui::Terminal::Palette1: return ColorMode::None;
    case ftxui::Terminal::Palette16: return ColorMode::Basic16;
    case ftxui::Terminal::Palette256: return ColorMode::Palette256;
    case ftxui::Terminal::TrueColor: return ColorMode::TrueColor;
    }
    return ColorMode::Basic16;
}

}  // namespace asb::tui
