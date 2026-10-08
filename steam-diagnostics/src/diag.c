#include <windows.h>
#include <stdio.h>
#include <share.h>
#include <stdint.h>
#include <string.h>
#include "profile.h"
#include "probe.h"

static HMODULE self;
static HANDLE worker;
static volatile LONG status = DIAG_STARTING;
#pragma comment(linker, "/EXPORT:AsmpDiagGetStatus=_AsmpDiagGetStatus@0")
#pragma comment(linker, "/EXPORT:AsmpDiagWait=_AsmpDiagWait@4")
DWORD WINAPI AsmpDiagGetStatus(void) { return (DWORD)InterlockedCompareExchange(&status, 0, 0); }
DWORD WINAPI AsmpDiagWait(DWORD timeout) { return WaitForSingleObject(worker, timeout); }

static DWORD WINAPI run(LPVOID unused)
{
    wchar_t exe[MAX_PATH], directory[MAX_PATH], log_path[MAX_PATH], stop_path[MAX_PATH];
    char digest[65];
    FILE* log = NULL;
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    enum ProbeResult previous = (enum ProbeResult)-1;
    Snapshot last = {0};
    DWORD last_log = 0;
    (void)unused;
    if (!GetModuleFileNameW(self, directory, MAX_PATH) || !GetModuleFileNameW(NULL, exe, MAX_PATH)) goto fail;
    wchar_t* slash = wcsrchr(directory, L'\\');
    if (!slash) goto fail;
    *slash = 0;
    if (swprintf_s(log_path, MAX_PATH, L"%s\\logs", directory) < 0) goto fail;
    if (!CreateDirectoryW(log_path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) goto fail;
    if (swprintf_s(log_path, MAX_PATH, L"%s\\logs\\asmp-diag-%lu.log", directory, GetCurrentProcessId()) < 0) goto fail;
    if (swprintf_s(stop_path, MAX_PATH, L"%s\\asmp-diag.stop", directory) < 0) goto fail;
    log = _wfsopen(log_path, L"w", _SH_DENYWR);
    if (!log) goto fail;
    setvbuf(log, NULL, _IONBF, 0);
    fprintf(log, "ASMP_DIAG loaded pid=%lu image=%08lX mode=read-only\n", GetCurrentProcessId(), (unsigned long)base);
    if (!hash_file_sha256(exe, digest)) {
        fprintf(log, "ERROR executable hash could not be read\n");
        goto fail;
    }
    fprintf(log, "exe_sha256=%s\n", digest);
    if (strcmp(digest, STEAM_EXE_SHA256)) {
        fprintf(log, "REJECTED unsupported executable; no game memory accessed\n");
        InterlockedExchange(&status, DIAG_REJECTED);
        fclose(log);
        return 0;
    }
    /* Also validate the mapped function before any gameplay memory reads. */
    static const unsigned char accessor[] = {0x55,0x8B,0xEC,0x8B,0x45,0x08,0x83,0xE0,0x03,
        0x8B,0x84,0x81,0x44,0x02,0x00,0x00,0x8B,0x40,0x10,0x5D,0xC2,0x04,0x00};
    __try {
        if (memcmp((void*)(base + STEAM_PLAYER_ACCESSOR_RVA), accessor, sizeof(accessor))) {
            fprintf(log, "REJECTED mapped-image signature mismatch\n");
            InterlockedExchange(&status, DIAG_REJECTED);
            fclose(log);
            return 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { goto fail; }
    fprintf(log, "PROFILE accepted Steam 33100; no hooks installed\n");
    fprintf(log, "milliseconds,state,game,player,army,x,y,z,health,animation,direction\n");
    InterlockedExchange(&status, DIAG_WAITING);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (GetFileAttributesW(stop_path) == INVALID_FILE_ATTRIBUTES) {
        Snapshot sample;
        enum ProbeResult result = probe_read(base, &sample);
        DWORD now = GetTickCount();
        if (result != previous || (result == PROBE_OK && memcmp(&last, &sample, sizeof(sample))) || now - last_log >= 5000) {
            fprintf(log, "%lu,%s,%08lX,%08lX,%u,%.3f,%.3f,%.3f,%u,%u,%u\n", now,
                probe_result_name(result), (unsigned long)sample.game, (unsigned long)sample.player,
                sample.army_index, sample.x, sample.y, sample.z, sample.health, sample.animation, sample.direction);
            last_log = now;
            last = sample;
            previous = result;
        }
        InterlockedExchange(&status, result == PROBE_OK ? DIAG_SAMPLING : DIAG_WAITING);
        Sleep(250);
    }
    fprintf(log, "STOP requested by asmp-diag.stop\n");
    fclose(log);
    return 0;
fail:
    if (log) { fprintf(log, "ERROR diagnostic initialization failed\n"); fclose(log); }
    InterlockedExchange(&status, DIAG_ERROR);
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        self = instance;
        worker = CreateThread(NULL, 0, run, NULL, 0, NULL);
        if (!worker) InterlockedExchange(&status, DIAG_ERROR);
    } else if (reason == DLL_PROCESS_DETACH && worker) CloseHandle(worker);
    return TRUE;
}
