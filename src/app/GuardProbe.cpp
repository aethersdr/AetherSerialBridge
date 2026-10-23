#include "app/GuardProbe.h"

#include <QByteArray>
#include <QtGlobal>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace asb::app {

void guardProbe()
{
    const QByteArray mode = qgetenv("ASB_TEST_GUARD_PROBE");
    if (mode.isEmpty()) return;
    bool created = false;
    char why[128] = "";
#ifdef _WIN32
    STARTUPINFOW si{};
    si.cb = static_cast<DWORD>(sizeof si);
    PROCESS_INFORMATION pi{};
    wchar_t cmd[] = L"cmd.exe /c exit 0";
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        created = true;
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        std::snprintf(why, sizeof why, "CreateProcessW error %lu", static_cast<unsigned long>(GetLastError()));
    }
#else
    const pid_t pid = ::fork();
    if (pid == 0) ::_exit(0);
    if (pid > 0) {
        created = true;
        int status = 0;
        ::waitpid(pid, &status, 0);
    } else {
        std::snprintf(why, sizeof why, "fork: %s", std::strerror(errno));
    }
#endif
    // The QtTest-shaped line names the ctest (ASB_TEST_GUARD_PROBE_NAME), so a failing run names the test.
    if (created) {
        QByteArray name = qgetenv("ASB_TEST_GUARD_PROBE_NAME");
        if (name.isEmpty()) name = "guard_installed_before_first_object";
        std::fprintf(stderr, "ASB-GUARD-PROBE created a process\nFAIL!  : %s() the probe created a process (no guard at this mode entry)\n",
                     name.constData());
    }
    else std::fprintf(stderr, "ASB-GUARD-PROBE denied: %s\n", why);
    std::fflush(stderr);
    if (mode == "exit") {
#ifdef _WIN32
        ExitProcess(created ? 1u : 0u);
#else
        ::_exit(created ? 1 : 0);
#endif
    }
}

}  // namespace asb::app
