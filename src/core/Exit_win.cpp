// Exit primitives, Windows (WS-3 contract in Exit.h; WS-4 implementation).

#include "core/Exit.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace asb {

namespace {

[[noreturn]] void terminateSelf(int code)
{
    // TerminateProcess on the current process does not return. If it ever did, ExitProcess ends the
    // process anyway; it runs DLL detach routines, so it is only the fallback.
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    ExitProcess(static_cast<UINT>(code));
}

}  // namespace

void faultDie(int code) { terminateSelf(code); }

namespace Exit {

void now(IDeadlineWatchdog& killDeadline, int code, void (*report)(int))
{
    // 1. The kill deadline first: whatever the report does (SetServiceStatus is an RPC that can
    //    block), the process ends by now + 2 s with faultDie(code) and no report (D59).
    killDeadline.tighten(std::chrono::steady_clock::now() + kReportBudget, code);
    // 2. The pre-built final report (SetServiceStatus STOPPED), best effort.
    if (report) report(code);
    // 3. Immediately.
    terminateSelf(code);
}

}  // namespace Exit
}  // namespace asb
