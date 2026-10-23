/* The planted VERSION.dll of the INV-67 dllprobe: exports what the probe imports, does nothing. */
#include <windows.h>

DWORD WINAPI PlantedGetFileVersionInfoSizeW(LPCWSTR file, LPDWORD handle)
{
    (void)file;
    if (handle) *handle = 0;
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
