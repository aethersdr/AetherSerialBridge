#pragma once

// The per-port table printed by --status, the same on both OSes. Lines end in '\n'.

#include "core/StatusStore.h"

#include <QString>

namespace asb::StatusPrinter {

// `report == nullptr`: no status has been reported yet.
QString format(const StatusReport* report);

}  // namespace asb::StatusPrinter
