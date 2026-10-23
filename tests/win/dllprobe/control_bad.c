/* The ImportAudit control (WS-7, INV-67): imports DBGHELP.dll, which no allowlist names; the audit must reject it. */
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>

int main(void)
{
    printf("%d\n", MakeSureDirectoryPathExists("C:\\nonexistent-asb-control\\") ? 1 : 0);
    return 0;
}
