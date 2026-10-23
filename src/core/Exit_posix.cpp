// Exit primitives, POSIX (WS-3 contract in Exit.h; WS-4 implementation).

#include "core/Exit.h"

#include <unistd.h>

namespace asb {

void faultDie(int code)
{
    // No I/O, no atexit handlers, no static destructors, no allocation.
    _exit(code);
}

namespace Exit {

void now(IDeadlineWatchdog& killDeadline, int code, void (*report)(int))
{
    // 1. The kill deadline first: whatever the report does, the process ends by now + 2 s (D59).
    killDeadline.tighten(std::chrono::steady_clock::now() + kReportBudget, code);
    // 2. The pre-built final report (none on Linux --service: report == nullptr).
    if (report) report(code);
    // 3. Immediately.
    _exit(code);
}

}  // namespace Exit
}  // namespace asb
