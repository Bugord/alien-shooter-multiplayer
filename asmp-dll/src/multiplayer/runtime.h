#ifndef ASMP_RUNTIME_H
#define ASMP_RUNTIME_H
#include <stdio.h>
#include "multiplayer.h"
#include "session.h"
#include "../game/tick_hook.h"
#include "../game/action_hook.h"
#include "../game/display_hook.h"
enum RuntimeState { RT_OFF, RT_STARTING, RT_RUNNING, RT_STOPPING };
typedef struct RuntimeFrame {
    DWORD milliseconds;
    LONG tick;
    int session_result;
    enum SessionState session;
    enum DisplayResult display;
    enum ProbeResult result;
    Snapshot snapshot;
    MultiplayerFrame multiplayer;
} RuntimeFrame;
typedef void (*FrameObserver)(const RuntimeFrame*);
typedef struct RuntimeConfig {
    int direct;
    char host[16], name[16];
    unsigned short port;
    FILE* log; /* Worker only; remains open through stop. */
    FrameObserver observer; /* Game thread, must not block or perform I/O. */
} RuntimeConfig;
/* Bindings let lifecycle tests inject engine/hook failures without native calls.
   Production start always uses the validated Steam bindings. */
typedef struct RuntimeBindings {
    int (*actor_bind)(uintptr_t, ActorEngine*);
    int (*session_enable)(uintptr_t);
    int (*action_install)(uintptr_t, ReplicaFilter, ShotCallback);
    int (*action_stop)(void);
    int (*world_install)(uintptr_t);
    int (*world_stop)(void);
    uint32_t (*world_generation)(void);
    enum SlotResult (*tick_install)(void* volatile*, void*, TickCallback);
    enum SlotResult (*tick_stop)(void);
    enum DisplayResult (*display_install)(uintptr_t, DrawCallback);
    int (*display_stop)(void);
} RuntimeBindings;
/* Start, worker_step and stop belong to the same worker thread. Configuration
   is copied; the log must stay open until stop completes. */
int runtime_start(uintptr_t base, const RuntimeConfig*);
int runtime_start_with_bindings(uintptr_t base, const RuntimeConfig*, const RuntimeBindings*);
/* Game tick is driven by the production hook on the captured game thread.
   The caller supplies a zero-initialized frame with tick and milliseconds. */
void runtime_game_tick(void* game, RuntimeFrame* out);
void runtime_worker_step(DWORD now);
/* Timeout/foreign hooks/live leftovers report failure and retain STOPPING so a
   later stop can finish. Startup rollback uses this same path. */
int runtime_stop(DWORD timeout_ms);
enum RuntimeState runtime_state(void);
#endif
