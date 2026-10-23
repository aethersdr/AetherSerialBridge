#pragma once

// Aether Serial Bridge shim for AetherSDR's LogManager (master §3.2, WS-2): the vendored canon
// closure logs through lcGui only, so that is the only category here. It is the bridge's own
// category, "asb.gui"; AetherSDR's log manager (file sinks, its other categories) is not vendored.

#include <QLoggingCategory>

namespace AetherSDR {

Q_DECLARE_LOGGING_CATEGORY(lcGui)

}  // namespace AetherSDR
