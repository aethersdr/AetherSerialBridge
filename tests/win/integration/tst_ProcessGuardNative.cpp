// WS-6 native rows on windows-latest, label win-integration: INV-104 applier_process_cannot_create_children,
// and the guard's error report. Its own executable: once this process joins the job it can never
// create a process again, so the real guard runs last (QtTest runs the slots in order).
#include "platform/win/NativeSeams.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <QTest>

#include <atomic>
#include <thread>

using namespace asb::win;

class tst_ProcessGuardNative : public QObject {
    Q_OBJECT

    static DWORD tryCreate(DWORD flags)
    {
        wchar_t cmd[] = L"cmd.exe /c exit 0";
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, flags | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 10000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return ERROR_SUCCESS;
        }
        return GetLastError();
    }

    // Job calls that fail like Win32 does, and a close that overwrites the last error (CloseHandle may).
    static inline int s_closes = 0;
    static inline DWORD s_failCode = 0;
    static inline QString s_failing;
    static detail::JobCalls failingCalls(const QString& failing, DWORD code)
    {
        s_closes = 0;
        s_failCode = code;
        s_failing = failing;
        detail::JobCalls c;
        c.create = []() -> void* {
            if (s_failing != QLatin1String("create")) return reinterpret_cast<void*>(quintptr(0x4321));   // never a real handle
            SetLastError(s_failCode);
            return nullptr;
        };
        c.limit = [](void*) -> bool {
            if (s_failing != QLatin1String("limit")) return true;
            SetLastError(s_failCode);
            return false;
        };
        c.assign = [](void*) -> bool {
            if (s_failing != QLatin1String("assign")) return true;
            SetLastError(s_failCode);
            return false;
        };
        c.close = [](void*) {
            ++s_closes;
            SetLastError(ERROR_INVALID_HANDLE);
        };
        return c;
    }

private slots:
    // The message carries the failing call's own error code, not the one CloseHandle left behind.
    void guard_reports_the_failing_calls_own_error_code_data()
    {
        QTest::addColumn<QString>("failing");
        QTest::addColumn<uint>("code");
        QTest::addColumn<QString>("message");
        QTest::addColumn<int>("closes");
        QTest::newRow("create fails") << QStringLiteral("create") << uint(ERROR_NOT_ENOUGH_MEMORY)
                                      << QStringLiteral("CreateJobObjectW failed (error 8)") << 0;
        QTest::newRow("limit fails") << QStringLiteral("limit") << uint(ERROR_INVALID_PARAMETER)
                                     << QStringLiteral("SetInformationJobObject failed (error 87)") << 1;
        QTest::newRow("assign fails") << QStringLiteral("assign") << uint(ERROR_ACCESS_DENIED)
                                      << QStringLiteral("AssignProcessToJobObject failed (error 5)") << 1;
    }
    void guard_reports_the_failing_calls_own_error_code()
    {
        QFETCH(QString, failing);
        QFETCH(uint, code);
        QFETCH(QString, message);
        QFETCH(int, closes);
        QString error;
        QVERIFY(!detail::installProcessGuard(&error, failingCalls(failing, DWORD(code))));
        QCOMPARE(error, message);
        QCOMPARE(s_closes, closes);   // the job handle is still closed on failure
    }

    void applier_process_cannot_create_children()
    {
        QCOMPARE(tryCreate(0), DWORD(ERROR_SUCCESS));   // control: before the guard this process can
        QString error;
        QVERIFY2(installProcessGuard(&error), qPrintable(error));
        QCOMPARE(tryCreate(0), DWORD(ERROR_NOT_ENOUGH_QUOTA));                  // ACTIVE_PROCESS = 1
        QCOMPARE(tryCreate(CREATE_BREAKAWAY_FROM_JOB), DWORD(ERROR_ACCESS_DENIED));   // no breakaway
        QVERIFY(tryCreate(CREATE_SUSPENDED) != DWORD(ERROR_SUCCESS));
        // Threads are not processes: the guarded process keeps working.
        std::atomic<bool> ran{false};
        std::thread t([&] { ran = true; });
        t.join();
        QVERIFY(ran);
        BOOL inJob = FALSE;
        QVERIFY(IsProcessInJob(GetCurrentProcess(), nullptr, &inJob));
        QVERIFY(inJob);
    }
};

QTEST_GUILESS_MAIN(tst_ProcessGuardNative)
#include "tst_ProcessGuardNative.moc"
