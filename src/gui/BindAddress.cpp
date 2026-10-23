#include "gui/BindAddress.h"

#include "core/BridgeConfig.h"

namespace asb::gui {

bool bindsEveryInterface(const QString& bindAddress)
{
    const QString text = bindAddress.trimmed();
    if (text.isEmpty()) return true;
    // The core's judgement by value (WS-12's isUnspecified / listensOnEveryInterface): one implementation for the
    // window, the TUI and the engine's own parser.
    return listensOnEveryInterface(text);
}

}  // namespace asb::gui
