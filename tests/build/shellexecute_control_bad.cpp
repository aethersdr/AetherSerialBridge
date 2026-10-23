// The ShellExecuteExW control of process_creation_audit (WS-16, D64, INV-126): an object other than SelfElevate.cpp
// that calls ShellExecuteExW. The audit, with SelfElevate.cpp's exception in force, must still report it. Windows only;
// never part of a product executable.
#include <windows.h>
#include <shellapi.h>

int main()
{
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof info;
    info.lpVerb = L"runas";
    info.lpFile = L"cmd.exe";
    return ShellExecuteExW(&info) ? 0 : 1;
}
