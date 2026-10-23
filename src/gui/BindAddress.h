#pragma once

// Whether a port's listen address binds every interface (the exposure question, INV-35), delegated to the core's
// listensOnEveryInterface (WS-12). Judged by the address's
// parsed value with the core parser the engine listens with (parseIpAddress), never by its spelling: "0.0.0.0",
// "000.000.000.000", "::", "0:0:0:0:0:0:0:0", "0000::" and the IPv4-mapped "::ffff:0.0.0.0" all bind every
// interface (a literal comparison misses most of them). A blank address counts too (the safe answer; the
// validator refuses it before anything is saved).

#include <QString>

namespace asb::gui {

bool bindsEveryInterface(const QString& bindAddress);

}  // namespace asb::gui
