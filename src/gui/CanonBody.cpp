#include "gui/CanonBody.h"

#include <QApplication>
#include <QDebug>
#include <QFontDatabase>
#include <QStyleFactory>

namespace asb::gui {

// Excerpt: AetherSDR @ 34363a38, src/gui/StagePage.cpp:119-130 (namespace changed; text unchanged).
QString canonBodyStyleSheet()
{
    // Popups are descendants too (a combo's list, PLAY's context menu): they
    // are separate windows, so they get their panel back after the rule above.
    return QStringLiteral(
        "QWidget#canonBody, QWidget#canonBody QWidget { background: transparent; }"
        "QWidget#canonBody { color: {{color.canon.inkSoft}}; }"
        "QWidget#canonBody QMenu, QWidget#canonBody QComboBoxPrivateContainer,"
        " QWidget#canonBody QComboBox QAbstractItemView"
        " { background: {{color.canon.raised}}; }");
}

// Excerpt: AetherSDR @ 34363a38, src/main.cpp:478-491 (made a function that counts what registered).
int registerInterFonts()
{
    // Bundled Inter UI font (SIL OFL 1.1). The theme's `font.family.ui` resolves to
    // "Inter", and px-tuned QSS assumes it, so register it rather than fall through
    // to a per-platform family (#4036). Static Regular + Bold, not the variable
    // font: variable-axis support needs Qt 6.7+.
    int registered = 0;
    static constexpr const char* kInterFonts[] = {
        ":/fonts/Inter-Regular.ttf",
        ":/fonts/Inter-Bold.ttf",
    };
    for (const char* path : kInterFonts) {
        if (QFontDatabase::addApplicationFont(QString::fromLatin1(path)) < 0) {
            qWarning() << "Failed to load bundled font:" << path;
        } else {
            ++registered;
        }
    }
    return registered;
}

// Excerpt: AetherSDR @ 34363a38, src/main.cpp:698-700.
void applyFusionStyle(QApplication& app)
{
    // Use Fusion style as a clean cross-platform base
    // (our dark theme overrides colors via stylesheet)
    app.setStyle(QStyleFactory::create("Fusion"));
}

}  // namespace asb::gui
