#include <windows.h>
#include <stdio.h>
#include <share.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "profile.h"
#include "probe.h"
#include "tick_hook.h"
#include "../../asmp-dll/src/game/steam/steam_display_hook.h"
#include "../../asmp-dll/src/multiplayer/steam/steam_runtime.h"

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
    if (frame->actor.event != ACTOR_NONE) {
        static const char* events[] = {"none", "spawned", "removed", "lost", "rejected", "fault", "pose"};
        static const char* reasons[] = {"none", "vid-class", "no-child", "list-category", "factory-null",
            "unregistered", "entity-type", "local-player-changed", "exception", "weapon"};
        fprintf(log, "# DUMMY event=%s entity=%08lX category=%u updates=%u tick=%ld reason=%s source_class=%u\n",
            events[frame->actor.event], (unsigned long)frame->actor.entity,
            frame->actor.category, frame->actor.updates, frame->tick,
            reasons[frame->actor.reason], frame->actor.source_class);
        if (frame->actor.event == ACTOR_POSE)
            fprintf(log, "# DUMMY_POSE velocity=%.3f moving=%u native_animation=%u native_frame=%u torso=%u applied_torso=%u torso_present=%u\n",
                frame->actor.applied_velocity, frame->actor.applied_moving,
                frame->actor.native_animation, frame->actor.native_frame, sample->torso_direction,
                frame->actor.applied_torso, frame->actor.torso_present);
    }
    for (unsigned int i = 0; i < frame->multiplayer.count; ++i) {
        const ActorResult* a = &frame->multiplayer.remote[i].actor;
        fprintf(log, "# REMOTE_ACTOR id=%u event=%d reason=%d entity=%08lX updates=%u animation=%u frame=%u torso=%u state=%d cleanup_failed=%d\n",
            frame->multiplayer.remote[i].id, a->event, a->reason, (unsigned long)a->entity,
            a->updates, a->native_animation, a->native_frame, a->applied_torso,
            frame->multiplayer.remote[i].state, frame->multiplayer.remote[i].cleanup_failed);
    }
    if (frame->multiplayer.shots_applied || frame->multiplayer.shots_discarded)
        fprintf(log, "# SHOTS applied=%u discarded=%u\n", frame->multiplayer.shots_applied, frame->multiplayer.shots_discarded);
    if (frame->session_result)
        fprintf(log, "# SESSION map_load_result=%d\n", frame->session_result);
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
    fprintf(log, ",%ld,%.3f,%u,%u,%u\n", frame->tick, sample->velocity, sample->moving,
        sample->torso_present, sample->torso_direction);
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
    fprintf(log, "# DISPLAY_STATS installed=%d frames=%ld\n", steam_display_hook_ready(), steam_display_hook_frames());
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
    int runtime_attempted = 0;
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
    fprintf(log, ",tick,velocity,moving,torso_present,torso_direction\n");
    char dummy_option[8] = {0}, multiplayer_option[8] = {0};
    DWORD dummy_length = GetEnvironmentVariableA("ASMP_DIAG_DUMMY", dummy_option, sizeof(dummy_option));
    int dummy_enabled = dummy_length == 1 && dummy_option[0] == '1';
    DWORD multiplayer_length = GetEnvironmentVariableA("ASMP_DIAG_MULTIPLAYER", multiplayer_option, sizeof(multiplayer_option));
    char host[16] = {0}, port_text[16] = {0}, name[16] = {0};
    DWORD host_length = GetEnvironmentVariableA("ASMP_DIAG_SERVER", host, sizeof(host));
    int multiplayer_enabled = host_length || (multiplayer_length == 1 && multiplayer_option[0] == '1');
    if (dummy_enabled && multiplayer_enabled) { fprintf(log, "ERROR dummy and multiplayer are mutually exclusive\n"); goto fail; }
    if (multiplayer_enabled) {
        SteamRuntimeConfig config = {0}; config.log = log; config.observer = tick_hook_observe;
        tick_hook_prepare_observer();
        if (host_length) {
            DWORD port_length = GetEnvironmentVariableA("ASMP_DIAG_PORT", port_text, sizeof(port_text));
            DWORD name_length = GetEnvironmentVariableA("ASMP_DIAG_NAME", name, sizeof(name));
            char* end = NULL;
            unsigned long port = port_length && port_length < sizeof(port_text) ? strtoul(port_text, &end, 10) : 0;
            if (!name_length) strcpy_s(name, sizeof(name), "SteamTester");
            if (host_length >= sizeof(host) || name_length >= sizeof(name) || !port || port > 65535 || !end || *end) {
                fprintf(log, "ERROR invalid direct connection configuration\n"); goto fail;
            }
            config.direct = 1; config.port = (unsigned short)port;
            strcpy_s(config.host, sizeof(config.host), host); strcpy_s(config.name, sizeof(config.name), name);
        }
        runtime_attempted = 1;
        if (!steam_runtime_start(base, &config)) { fprintf(log, "ERROR Steam runtime startup failed\n"); goto fail; }
        fprintf(log, "# MULTIPLAYER enabled native_map menu names health_bars shots\n");
    }
    if (dummy_enabled) {
        if (!tick_hook_enable_dummy(base)) { fprintf(log, "REJECTED dummy actor engine signature mismatch\n"); goto fail; }
        fprintf(log, "# DUMMY enabled offset_x=80 lifetime_seconds=60\n");
    }
    enum TickHookResult hook_result = TICK_HOOK_OK;
    if (!multiplayer_enabled) {
        hook_attempted = 1;
        hook_result = tick_hook_install(
            (void* volatile*)(base + STEAM_GAME_VTABLE_RVA + STEAM_GAME_TICK_SLOT * sizeof(uintptr_t)),
            (void*)(base + STEAM_GAME_TICK_RVA), base);
        if (hook_result != TICK_HOOK_OK) {
            fprintf(log, "ERROR tick hook installation result=%d\n", hook_result);
            goto fail;
        }
        fprintf(log, "# TICK_HOOK installed slot=%u original=%08lX\n", STEAM_GAME_TICK_SLOT,
            (unsigned long)(base + STEAM_GAME_TICK_RVA));
    }
    InterlockedExchange(&status, DIAG_WAITING);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (GetFileAttributesW(stop_path) == INVALID_FILE_ATTRIBUTES) {
        FrameSample frames[64];
        unsigned int n = tick_hook_drain(frames, 64);
        for (unsigned int i = 0; i < n; ++i) log_frame(log, &frames[i], &last, &previous, &last_log);
        DWORD now = GetTickCount();
        if (runtime_attempted) steam_runtime_worker_step(now);
        if (now - last_stats >= 5000) { log_stats(log); last_stats = now; }
        Sleep(20);
    }
    int cleanup_ok = 1;
    if (runtime_attempted) cleanup_ok = steam_runtime_stop(5000u);
    else {
        tick_hook_request_dummy_stop();
        DWORD cleanup_started = GetTickCount();
        while (!tick_hook_dummy_stopped() && GetTickCount() - cleanup_started < 5000u) {
            FrameSample pending[64]; unsigned int available = tick_hook_drain(pending, 64);
            for (unsigned int i = 0; i < available; ++i) log_frame(log, &pending[i], &last, &previous, &last_log);
            Sleep(20);
        }
        if (!tick_hook_dummy_stopped()) { fprintf(log, "# DUMMY cleanup timeout; hook retained\n"); cleanup_ok = 0; }
        else { hook_result = tick_hook_stop(); cleanup_ok = hook_result == TICK_HOOK_OK; }
    }
    FrameSample frames[64];
    unsigned int n;
    while ((n = tick_hook_drain(frames, 64)) != 0)
        for (unsigned int i = 0; i < n; ++i) log_frame(log, &frames[i], &last, &previous, &last_log);
    fprintf(log, "# STOP requested by asmp-diag.stop; hook_restore_result=%d cleanup=%d\n", hook_result, cleanup_ok);
    log_stats(log);
    InterlockedExchange(&status, cleanup_ok ? DIAG_STOPPED : DIAG_ERROR);
    fclose(log);
    return 0;
fail:
    if (runtime_attempted) {
        int restored = steam_runtime_stop(5000u);
        if (log) fprintf(log, "# ERROR runtime cleanup=%d\n", restored);
    }
    if (hook_attempted) {
        enum TickHookResult restored = tick_hook_stop();
        if (log) fprintf(log, "# ERROR diagnostic hook cleanup=%d\n", restored);
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
