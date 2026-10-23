#include "platform/win/SelfElevate.h"

#include <QFile>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <string>

namespace asb::win::SelfElevate {

#ifdef ASB_TEST_SEAMS
// SelfElevateTestChild.cpp (test builds only).
bool relaunchAsChildForTest(const QString& image, const QString& parameters, const QString& pidFile, QString* error);
#endif

namespace {

QString ownImage()
{
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    return QString::fromStdWString(buf);
}

}  // namespace

Outcome relaunch(const QStringList& arguments, Mode mode)
{
    Outcome o;
    const QString image = ownImage();
    if (image.isEmpty()) {
        o.error = QStringLiteral("this program's own path could not be read (%1)").arg(GetLastError());
        return o;
    }
    const QString parameters = commandLine(arguments);
#ifdef ASB_TEST_SEAMS
    const QString elevator = qEnvironmentVariable("ASB_TEST_ELEVATOR");
    if (elevator.startsWith(QLatin1String("record:"))) {
        QFile f(elevator.mid(7));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            o.error = f.errorString();
            return o;
        }
        f.write((image + QLatin1Char('\n') + parameters + QLatin1Char('\n') +
                 QStringLiteral("wait=%1 show=%2\n").arg(int(mode.wait)).arg(int(mode.show)))
                    .toUtf8());
        o.started = true;
        return o;
    }
    if (elevator.startsWith(QLatin1String("child:"))) {
        // tst_GuiNative::standardUserWindow(real-child): its own object, test builds only (process_creation_audit).
        o.started = relaunchAsChildForTest(image, parameters, elevator.mid(6), &o.error);
        return o;
    }
#endif
    const std::wstring file = image.toStdWString();
    const std::wstring params = parameters.toStdWString();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof info;
    info.fMask = (mode.wait ? SEE_MASK_NOCLOSEPROCESS : 0) | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = file.c_str();
    info.lpParameters = params.c_str();
    // The console exe: its result is a message box, not a console window. The window shows itself.
    info.nShow = mode.show ? SW_SHOWNORMAL : SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        const DWORD e = GetLastError();
        if (e == ERROR_CANCELLED) o.cancelled = true;
        else o.error = QStringLiteral("Windows error %1").arg(e);
        return o;
    }
    o.started = true;
    if (mode.wait && info.hProcess) {
        WaitForSingleObject(info.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(info.hProcess, &code);
        o.exitCode = int(code);
        CloseHandle(info.hProcess);
    }
    return o;
}

}  // namespace asb::win::SelfElevate
