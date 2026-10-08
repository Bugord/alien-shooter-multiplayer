#ifndef ASMP_STEAM_RUNTIME_H
#define ASMP_STEAM_RUNTIME_H
#include <stdio.h>
#include "steam_multiplayer.h"
#include "steam_session.h"
#include "../../game/steam/steam_tick_hook.h"
#include "../../game/steam/steam_action_hook.h"
#include "../../game/steam/steam_display_hook.h"
enum SteamRuntimeState { RT_OFF, RT_STARTING, RT_RUNNING, RT_STOPPING };
typedef struct SteamRuntimeFrame {
    DWORD milliseconds;
    LONG tick;
    int session_result;
    enum SteamSessionState session;
    enum SteamDisplayResult display;
    enum ProbeResult result;
    Snapshot snapshot;
    SteamMultiplayerFrame multiplayer;
} SteamRuntimeFrame;
typedef void (*SteamFrameObserver)(const SteamRuntimeFrame*);
typedef struct SteamRuntimeConfig {
    int direct;
    char host[16], name[16];
    unsigned short port;
    FILE* log; /* Worker only; remains open through stop. */
    SteamFrameObserver observer; /* Game thread, must not block or perform I/O. */
} SteamRuntimeConfig;
/* Bindings let lifecycle tests inject engine/hook failures without native calls.
   Production start always uses the validated Steam bindings. */
typedef struct SteamRuntimeBindings {
    int (*actor_bind)(uintptr_t, ActorEngine*);
    int (*session_enable)(uintptr_t);
    int (*action_install)(uintptr_t, SteamReplicaFilter, SteamShotCallback);
    int (*action_stop)(void);
    int (*world_install)(uintptr_t);
    int (*world_stop)(void);
    uint32_t (*world_generation)(void);
    enum SteamSlotResult (*tick_install)(void* volatile*, void*, SteamTickCallback);
    enum SteamSlotResult (*tick_stop)(void);
    enum SteamDisplayResult (*display_install)(uintptr_t, SteamDrawCallback);
    int (*display_stop)(void);
} SteamRuntimeBindings;
/* Start, worker_step and stop belong to the same worker thread. Configuration
   is copied; the log must stay open until stop completes. */
int steam_runtime_start(uintptr_t base, const SteamRuntimeConfig*);
int steam_runtime_start_with_bindings(uintptr_t base, const SteamRuntimeConfig*, const SteamRuntimeBindings*);
/* Game tick is driven by the production hook on the captured game thread.
   The caller supplies a zero-initialized frame with tick and milliseconds. */
void steam_runtime_game_tick(void* game, SteamRuntimeFrame* out);
void steam_runtime_worker_step(DWORD now);
/* Timeout/foreign hooks/live leftovers report failure and retain STOPPING so a
   later stop can finish. Startup rollback uses this same path. */
int steam_runtime_stop(DWORD timeout_ms);
enum SteamRuntimeState steam_runtime_state(void);
#endif
