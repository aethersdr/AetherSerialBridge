#include "platform/win/ServiceExit.h"

#include <atomic>

namespace asb::win::ServiceExit {

namespace {
std::atomic<IServiceStatusReporter*> g_reporter{nullptr};
}

void setFinalReporter(IServiceStatusReporter* r) { g_reporter.store(r, std::memory_order_release); }

IServiceStatusReporter* finalReporter() { return g_reporter.load(std::memory_order_acquire); }

void reportStopped(int code)
{
    IServiceStatusReporter* r = g_reporter.load(std::memory_order_acquire);
    if (!r) return;
    const NativeServiceStatus row = stoppedRow(code);   // on the stack
    r->report(row);
}

}  // namespace asb::win::ServiceExit
