#pragma once

// Structural validator for AetherSDR theme JSON (master §5.6, WS-11, S6, INV-36).
//
// A theme file is user-writable input read by an elevated process, and its string values end up
// in Qt stylesheets. Only a document that passes here is handed to
// ThemeManager::activateValidatedTheme (P2), and it is handed over as the same bytes.
//
// The rules mirror the loader's own value discrimination (ThemeManager.cpp flattenTokens): an
// object with a string "type" is a gradient, one with a string "family" is a font, any other
// object is a group. The full rule list, with one failure code per rule, is in
// tools/theme_validator_ref.py, the independent reference implementation; the two must agree on
// every fixture in tests/data/themes/manifest.json (tst_ThemeFollower::oracleFixturesAgree).

#include <QByteArray>
#include <QString>

namespace asb::gui {

struct ThemeValidation {
    bool ok = false;
    // On failure: the rule that failed (see theme_validator_ref.py) and where, as a dotted key
    // path with [i] for array elements ("" = the document itself).
    QString code;
    QString path;
    // On success: scalar leaves outside gradient and font objects, gradients, fonts.
    int leaves = 0;
    int gradients = 0;
    int fonts = 0;

    // "<code> at <path>", for log lines and the follower's reason text.
    QString describe() const;
};

// Largest theme document accepted, in bytes (also the discovery read cap, INV-85).
inline constexpr qsizetype kMaxThemeBytes = 256 * 1024;

ThemeValidation validateTheme(const QByteArray& json);

}  // namespace asb::gui
