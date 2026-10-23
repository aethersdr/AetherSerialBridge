#pragma once

// The host hook patch P1 (vendor/aethersdr/patches/0001-user-theme-dir-hook.patch) adds to
// ThemeManager::scanAvailableThemes(): the directory user themes are listed from.
//
// AetherSDR's own host returns the expression the patch replaced,
//     QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
//         + QStringLiteral("/AetherSDR/themes")
// Aether Serial Bridge returns "" (ThemeManagerHost.cpp): ThemeManager lists no user themes at
// all, and does not check, list or open any directory for them (INV-52). Theme following reads
// and validates AetherSDR's theme file itself and hands ThemeManager the validated bytes
// (activateValidatedTheme, patch P2; WS-11).

#include <QString>

namespace AetherSDR {

QString aetherUserThemeDir();

}  // namespace AetherSDR
