#pragma once

// Stylesheet composition for the bridge window (master §5.2, WS-10, B6, INV-69).
//
// AetherSDR's canonBodyStyleSheet() (CanonBody.h) makes `QWidget#canonBody QWidget` transparent. That selector has
// an ID, so it outranks every bare control selector (QLineEdit, QPushButton, QTableView) in the same sheet and
// leaves fields and buttons without their fill. The bridge does not use that rule. The window carries three
// levels instead:
//
//   application  appStylesheetTemplate() (AetherSDR Theme.h) resolved at root scope, qApp->setStyleSheet, re-resolved
//                on ThemeManager::themeChanged (applyApplicationSheet)
//   window       CanonWindow's own sheet (untouched)
//   body         one applyCanonSheet(bodyWidget(), composedBodySheet): ModemChrome::styleSheet(Dialog, Canon) +
//                bridgeContainerRules() + bridgeTableRules()
//
// bridgeContainerRules() makes only CONTAINERS transparent, by object name or class, never a control. Every
// container the window creates carries one of the names listed in kContainerNames.

#include <QString>
#include <QStringList>

class QApplication;

namespace asb::gui {

// The object names of the bridge's transparent containers (each one is a QWidget, the card host a QFrame).
inline constexpr const char* kHeaderName = "asbHeader";
inline constexpr const char* kServiceRowName = "asbServiceRow";
inline constexpr const char* kPortsPanelName = "asbPortsPanel";
inline constexpr const char* kCardHostName = "asbCardHost";

// Containers transparent (by name or class), the canon ink colour on the body, and the popup rules (menus, combo
// lists on color.canon.raised) of canonBodyStyleSheet.
QString bridgeContainerRules();
// QTableView / QTableView::item / the header with ModemChrome's table declarations (ModemChrome styles
// QTableWidget, which a QTableView is not). Canon palette: inkSoft text on control, raised alternate rows,
// line grid, nested selection.
QString bridgeTableRules();
// The bridge's own named labels and frames: the pending note, the banner, the footer, the card.
QString bridgeLabelRules();
// ModemChrome::styleSheet(Dialog, Canon) + bridgeContainerRules() + bridgeTableRules() + bridgeLabelRules().
QString composedBodySheet();

// Sets the application sheet (appStylesheetTemplate resolved at root scope) and re-resolves it whenever the
// theme changes. Call once, after ThemeManager::instance() exists.
void applyApplicationSheet(QApplication& app);

// The application's own icon (resources/icon/, built by tools/make_icons.sh): every size in the qrc's /appicon
// prefix (16 to 256), so the title bar, the taskbar and Alt-Tab each take a frame drawn for their size instead of a
// reduction of the largest. Also names the desktop entry (aether-serial-bridge.desktop): Wayland ignores the window
// icon and finds it through the app id. Call once, after the QApplication exists.
void applyApplicationIcon(QApplication& app);

}  // namespace asb::gui
