#pragma once

// Elevation (master §4.10): the commands that change the service, the firewall or the managed configuration run
// only elevated. Windows: the process token's TokenElevation (an administrator's filtered token is not elevated);
// POSIX: geteuid() == 0. Read only; nothing is requested or raised.

#include <QString>

namespace asb::cli::Elevation {

bool isElevated();

// What to tell the user who ran `command` without elevation (one sentence, actionable).
QString refusal(const QString& command, bool windows);

}  // namespace asb::cli::Elevation
