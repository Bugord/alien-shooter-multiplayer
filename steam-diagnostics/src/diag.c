#include <windows.h>
#include <stdio.h>
#include <share.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "profile.h"
#include "probe.h"
#include "tick_hook.h"
#include "state_client.h"

static HMODULE self;
static HANDLE worker;
static volatile LONG status = DIAG_STARTING;
#pragma comment(linker, "/EXPORT:AsmpDiagGetStatus=_AsmpDiagGetStatus@0")
#pragma comment(linker, "/EXPORT:AsmpDiagWait=_AsmpDiagWait@4")
DWORD WINAPI AsmpDiagGetStatus(void) { return (DWORD)InterlockedCompareExchange(&status, 0, 0); }
DWORD WINAPI AsmpDiagWait(DWORD timeout) { return WaitForSingleObject(worker, timeout); }

static void log_frame(FILE* log, const FrameSample* frame, Snapshot* last,
    enum ProbeResult* previous, DWORD* last_log)
{
    const Snapshot* sample = &frame->snapshot;
    if (frame->result == *previous &&
        (frame->result != PROBE_OK || !memcmp(last, sample, sizeof(*sample))) &&
        frame->milliseconds - *last_log < 5000) return;
    fprintf(log, "%lu,%s,%08lX,%08lX,%u,%.3f,%.3f,%.3f,%d,%u,%u,%d,%d,%d,%d",
        frame->milliseconds, probe_result_name(frame->result), (unsigned long)sample->game,
        (unsigned long)sample->player, sample->army_index, sample->x, sample->y, sample->z,
        sample->health, sample->animation, sample->direction, sample->weapon_slot,
        sample->weapon_vid, sample->current_ammo, sample->current_ammo_raw);
    for (unsigned int i = 0; i < STEAM_STORED_AMMO_COUNT; ++i)
        fprintf(log, ",%u", sample->stored_ammo[i]);
    fprintf(log, ",%ld\n", frame->tick);
    *last_log = frame->milliseconds;
    *last = *sample;
    *previous = frame->result;
    InterlockedExchange(&status, frame->result == PROBE_OK ? DIAG_SAMPLING : DIAG_WAITING);
}

static void log_stats(FILE* log)
{
    TickStats stats = tick_hook_stats();
    fprintf(log, "# TICK_STATS calls=%ld captured=%ld dropped=%ld installed=%ld\n",
        stats.calls, stats.captured, stats.dropped, stats.installed);
}

static DWORD WINAPI run(LPVOID unused)
{
    wchar_t exe[MAX_PATH], directory[MAX_PATH], log_path[MAX_PATH], stop_path[MAX_PATH];
    char digest[65];
    FILE* log = NULL;
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    enum ProbeResult previous = (enum ProbeResult)-1;
    Snapshot last = {0};
    DWORD last_log = 0;
    DWORD last_stats = GetTickCount();
    int hook_attempted = 0;
    StateClient* network = NULL;
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
    fprintf(log, "ASMP_DIAG loaded pid=%lu image=%08lX mode=tick-hook\n", GetCurrentProcessId(), (unsigned long)base);
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
        static const unsigned char tick_prefix[] = {0x55,0x8B,0xEC,0x6A,0xFF};
        if (memcmp((void*)(base + STEAM_GAME_TICK_RVA), tick_prefix, sizeof(tick_prefix)) ||
            *(uintptr_t*)(base + STEAM_GAME_VTABLE_RVA + STEAM_GAME_TICK_SLOT * sizeof(uintptr_t)) !=
                base + STEAM_GAME_TICK_RVA) {
            fprintf(log, "REJECTED tick signature or vtable mismatch; no hook installed\n");
            InterlockedExchange(&status, DIAG_REJECTED);
            fclose(log);
            return 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { goto fail; }
    fprintf(log, "PROFILE accepted Steam 33100; gameplay reads after original tick\n");
    fprintf(log, "milliseconds,state,game,player,army,x,y,z,health,animation,direction,weapon_slot,weapon_vid,current_ammo,current_ammo_raw");
    for (unsigned int i = 0; i < STEAM_STORED_AMMO_COUNT; ++i)
        fprintf(log, ",stored_ammo_slot_%u", i + STEAM_STORED_AMMO_FIRST_SLOT);
    fprintf(log, ",tick\n");
    hook_attempted = 1;
    enum TickHookResult hook_result = tick_hook_install(
        (void* volatile*)(base + STEAM_GAME_VTABLE_RVA + STEAM_GAME_TICK_SLOT * sizeof(uintptr_t)),
        (void*)(base + STEAM_GAME_TICK_RVA), base);
    if (hook_result != TICK_HOOK_OK) {
        fprintf(log, "ERROR tick hook installation result=%d\n", hook_result);
        goto fail;
    }
    fprintf(log, "# TICK_HOOK installed slot=%u original=%08lX\n", STEAM_GAME_TICK_SLOT,
        (unsigned long)(base + STEAM_GAME_TICK_RVA));
    InterlockedExchange(&status, DIAG_WAITING);
    char host[256], port_text[16], name[16];
    if (GetEnvironmentVariableA("ASMP_DIAG_SERVER", host, sizeof(host)) > 0) {
        DWORD host_length = GetEnvironmentVariableA("ASMP_DIAG_SERVER", host, sizeof(host));
        DWORD port_length = GetEnvironmentVariableA("ASMP_DIAG_PORT", port_text, sizeof(port_text));
        DWORD name_length = GetEnvironmentVariableA("ASMP_DIAG_NAME", name, sizeof(name));
        char* end = NULL;
        unsigned long port = port_length > 0 && port_length < sizeof(port_text) ? strtoul(port_text, &end, 10) : 0;
        if (!name_length) strcpy_s(name, sizeof(name), "SteamTester");
        if (host_length < sizeof(host) && name_length < sizeof(name) && port > 0 && port <= 65535 && end && !*end)
            network = state_client_create(host, (unsigned short)port, name, log);
        if (!network) fprintf(log, "# NET_ERROR invalid configuration or connection initialization failed\n");
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (GetFileAttributesW(stop_path) == INVALID_FILE_ATTRIBUTES) {
        FrameSample frames[64];
        unsigned int n = tick_hook_drain(frames, 64);
        for (unsigned int i = 0; i < n; ++i) log_frame(log, &frames[i], &last, &previous, &last_log);
        DWORD now = GetTickCount();
        if (network && n) {
            const FrameSample* frame = &frames[n - 1];
            const Snapshot* sample = &frame->snapshot;
            MpSteamState state = {0};
            if (frame->result == PROBE_OK) {
                state.active = 1; state.tick = (uint32_t)frame->tick;
                state.x = sample->x; state.y = sample->y; state.z = sample->z;
                state.health = sample->health; state.animation = sample->animation;
                state.direction = sample->direction; state.weapon_slot = sample->weapon_slot;
                state.current_ammo = sample->current_ammo;
                memcpy(state.stored_ammo, sample->stored_ammo, sizeof(state.stored_ammo));
            }
            state_client_publish(network, &state, frame->milliseconds);
        }
        state_client_update(network, now);
        if (now - last_stats >= 5000) { log_stats(log); state_client_stats(network); last_stats = now; }
        Sleep(20);
    }
    hook_result = tick_hook_stop();
    FrameSample frames[64];
    unsigned int n;
    while ((n = tick_hook_drain(frames, 64)) != 0)
        for (unsigned int i = 0; i < n; ++i) log_frame(log, &frames[i], &last, &previous, &last_log);
    fprintf(log, "# STOP requested by asmp-diag.stop; hook_restore_result=%d\n", hook_result);
    log_stats(log);
    state_client_stats(network);
    state_client_destroy(network);
    InterlockedExchange(&status, hook_result == TICK_HOOK_OK ? DIAG_STOPPED : DIAG_ERROR);
    fclose(log);
    return 0;
fail:
    state_client_destroy(network);
    if (hook_attempted) {
        enum TickHookResult restored = tick_hook_stop();
        if (log) fprintf(log, "# ERROR cleanup hook_restore_result=%d\n", restored);
    }
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
