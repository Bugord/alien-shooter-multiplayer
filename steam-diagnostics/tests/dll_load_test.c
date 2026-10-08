#include <windows.h>
#include <stdio.h>
#include "../src/profile.h"

typedef DWORD (WINAPI* StatusFn)(void);
typedef DWORD (WINAPI* WaitFn)(DWORD);
int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 2;
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) { fprintf(stderr, "DLL load failed: %lu\n", GetLastError()); return 1; }
    union { FARPROC address; StatusFn status; WaitFn wait; } function;
    function.address = GetProcAddress(dll, "AsmpDiagGetStatus");
    StatusFn status = function.status;
    function.address = GetProcAddress(dll, "AsmpDiagWait");
    WaitFn wait = function.wait;
    if (!status || !wait) return 1;
    if (wait(15000) != WAIT_OBJECT_0) { fprintf(stderr, "Diagnostic worker did not finish.\n"); return 1; }
    DWORD actual = status();
    FreeLibrary(dll);
    if (actual != DIAG_REJECTED) { fprintf(stderr, "Unexpected status %lu\n", actual); return 1; }
    puts("DLL loading and unsupported-EXE rejection: passed");
    return 0;
}
