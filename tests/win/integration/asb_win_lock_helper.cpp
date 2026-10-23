// A separate process for the WS-6 lock rows (tst_WinLocksNative): the real FileApplyLock / EngineLock in another
// process, so exclusivity, release on death and the cross-process view are observed, not assumed.
//
//   asb_win_lock_helper apply <lockDir> <holdMs> <outFile>   acquire apply.lock (60 s), record QPC enter/leave
//                                                             around a <holdMs> hold, release; exit 0 (3: not acquired)
//   asb_win_lock_helper hold-apply <lockDir>                  acquire apply.lock, print "held", wait to be killed
//   asb_win_lock_helper hold-engine <lockDir>                 take engine.lock as the service would; print "held"
//                                                             (or "refused <n>") and wait to be killed
#include "platform/win/EngineLock.h"
#include "platform/win/FileApplyLock.h"
#include "platform/win/NativeSeams.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <string>

using namespace asb;
using namespace asb::win;

static qint64 qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) return 2;
    const std::wstring mode = argv[1];
    const QString dir = QString::fromWCharArray(argv[2]);
    if (mode == L"apply" && argc == 5) {
        FileApplyLock lock(nativeSeams(), dir);
        std::unique_ptr<LockHold> hold = lock.acquire(std::chrono::seconds(60));
        if (!hold) {
            std::fprintf(stderr, "not acquired: %s\n", lock.lastError().toLocal8Bit().constData());
            return 3;
        }
        const qint64 enter = qpc();
        Sleep(DWORD(_wtoi(argv[3])));
        const qint64 leave = qpc();
        FILE* f = nullptr;
        if (_wfopen_s(&f, argv[4], L"w") != 0 || !f) return 4;
        std::fprintf(f, "%lld %lld\n", enter, leave);
        std::fclose(f);
        return 0;   // the hold is released here (and would be by the kernel on any exit)
    }
    if (mode == L"hold-apply") {
        FileApplyLock lock(nativeSeams(), dir);
        std::unique_ptr<LockHold> hold = lock.acquire(std::chrono::seconds(60));
        std::fputs(hold ? "held\n" : "refused\n", stdout);
        std::fflush(stdout);
        Sleep(INFINITE);
    }
    if (mode == L"hold-engine") {
        EngineLock lock(nativeSeams(), dir, LockRole::Service);
        LockResult why = LockResult::Acquired;
        std::unique_ptr<LockHold> hold = lock.tryAcquire(&why);
        if (hold) std::printf("held\n");
        else std::printf("refused %d\n", int(why));
        std::fflush(stdout);
        Sleep(INFINITE);
    }
    return 2;
}
