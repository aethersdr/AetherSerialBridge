/* INV-67 dllprobe (WS-7): an executable that statically imports VERSION.dll, a System32 DLL that is not a
   KnownDLL, and prints where the loader took it from. Built twice: dllprobe_unmitigated (no flag) and
   dllprobe_mitigated (asb_apply_dll_policy ALWAYS, /DEPENDENTLOADFLAG:0x800). RunProbe.cmake copies each next to
   a planted VERSION.dll in a fresh directory: the unmitigated one must load the plant (the positive control), the
   mitigated one System32's. */
#include <windows.h>
#include <stdio.h>

int main(void)
{
    DWORD handle = 0;
    wchar_t path[MAX_PATH] = L"?";
    GetFileVersionInfoSizeW(L"C:\\Windows\\System32\\kernel32.dll", &handle);
    GetModuleFileNameW(GetModuleHandleW(L"version.dll"), path, MAX_PATH);
    printf("dllprobe: version.dll loaded from: %ls\n", path);
    return 0;
}
