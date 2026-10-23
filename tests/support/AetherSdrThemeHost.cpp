// AetherSDR's own ThemeManagerHost, for the positive-control probe only (asb_theme_probe_aetherhost):
// the expression patch P1 moved out of ThemeManager::scanAvailableThemes(). Never linked into the
// bridge.

#include <QStandardPaths>

#include "ThemeManagerHost.h"

namespace AetherSDR {

QString aetherUserThemeDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/AetherSDR/themes");
}

}  // namespace AetherSDR
