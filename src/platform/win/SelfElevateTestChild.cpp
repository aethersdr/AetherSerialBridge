// Test builds only (ASB_BUILD_TESTS): the relaunch seam ASB_TEST_ELEVATOR=child:<pid file>, in an object of its own so
// that process_creation_audit excuses CreateProcessW here by name, and no release object can carry it.
//
// tst_GuiNative::standardUserWindow(real-child) (v1.1 station fix, 2026-10-18): the same image and arguments
// as the runas relaunch, started as a direct child. It lands in the window's job exactly as runas's child does (AppInfo
// creates that one with the window as its parent process, and a process created that way inherits the parent's job),
// so the row sees what kill-on-job-close did to Restart as administrator. No UAC: this child is not elevated. Its pid
// goes to the file; it inherits the window's output; its test ops are a single wait, so it shows itself and idles.

#include <QFile>
#include <QString>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace asb::win::SelfElevate {

bool relaunchAsChildForTest(const QString& image, const QString& parameters, const QString& pidFile, QString* error)
{
    // This changes the CALLING window's own environment (the child inherits it): harmless only because the window
    // closes right after a started relaunch. Do not reuse this seam from a process that stays alive.
    SetEnvironmentVariableW(L"ASB_TEST_ELEVATOR", nullptr);
    SetEnvironmentVariableW(L"ASB_TEST_GUI_OPS", L"wait");
    std::wstring cmd = (QLatin1Char('"') + image + QStringLiteral("\" ") + parameters).toStdWString();
    const std::wstring file = image.toStdWString();
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(file.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        *error = QStringLiteral("Windows error %1").arg(GetLastError());
        return false;
    }
    QFile f(pidFile);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QByteArray::number(quint64(pi.dwProcessId)));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace asb::win::SelfElevate
