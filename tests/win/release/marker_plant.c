/* The INV-88 plant (WS-13 release_gui_self_test): a DLL that exports nothing and, if any process ever maps it,
 * leaves a marker file beside itself named <its own file name>.loaded. packaging/windows/release-gui-self-test.ps1
 * copies it under every name the GUI's import and delay-load tables list and as plugins/platforms/qwindows.dll;
 * a marker afterwards means the packaged GUI loaded a file from its own folder. */
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        WCHAR path[MAX_PATH + 16];
        DWORD n = GetModuleFileNameW(instance, path, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            lstrcatW(path, L".loaded");
            HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    }
    return TRUE;
}
