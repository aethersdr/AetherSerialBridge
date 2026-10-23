#include "cli/Elevation.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace asb::cli::Elevation {

bool isElevated()
{
#ifdef _WIN32
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, static_cast<DWORD>(sizeof elevation), &size);
    CloseHandle(token);
    return ok != FALSE && elevation.TokenIsElevated != 0;
#else
    return ::geteuid() == 0;
#endif
}

QString refusal(const QString& command, bool windows)
{
    if (windows)
        return QStringLiteral("%1 changes the service and the firewall: run this from an elevated terminal (Run as "
                              "administrator)")
            .arg(command);
    return QStringLiteral("%1 changes system files and services and must run as root: sudo aether-serial-bridge %1")
        .arg(command);
}

}  // namespace asb::cli::Elevation
