#pragma once

// Excerpts from AetherSDR @ 34363a38 the bridge GUI uses outside the vendored closure (master §3.3,
// WS-2). AetherSDR is GPL-3.0, as is this program; each definition carries its provenance.

#include <QString>

class QApplication;

namespace asb::gui {

// AetherSDR src/gui/StagePage.cpp:119-130, canonBodyStyleSheet(). REFERENCE ONLY: the bridge window
// does not apply it. Its ID-selector transparency rule outranks field and button styles; the
// bridge composes a container-scoped rule instead (§5.2, WS-10, INV-69), which keeps this sheet's
// ink colour and popup rules. Kept callable so that rule's mutant (apply this sheet) can be built.
QString canonBodyStyleSheet();

// AetherSDR src/main.cpp:478-491: register the bundled Inter Regular + Bold (":/fonts/Inter-*",
// resources/asb.qrc), which the themes' `font.family.ui` names. Returns how many registered.
int registerInterFonts();

// AetherSDR src/main.cpp:698-700: Fusion as the cross-platform base style.
void applyFusionStyle(QApplication& app);

}  // namespace asb::gui
