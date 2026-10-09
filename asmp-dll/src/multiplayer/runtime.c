#include <string.h>
#include "runtime.h"
#include "../game/world_hook.h"
#include "../game/ui.h"
enum RuntimeStage { ST_NONE, ST_BOUND, ST_SESSION, ST_COORDINATOR, ST_ACTION, ST_WORLD, ST_TICK };
enum NetworkState { NS_OFF, NS_CONNECTING, NS_CONNECTED };
static struct {
    volatile LONG state, game_thread;
    DWORD worker_thread;
    enum RuntimeStage stage;
    uintptr_t base;
    RuntimeConfig config;
    RuntimeBindings bindings;
    StateClient* client;
    enum NetworkState network;
    unsigned int generation, display_ticks;
    DWORD connecting_at, stats_at;
    RuntimeFrame latest;
    enum DisplayResult display, logged_display;
    enum SessionState logged_session;
} runtime;
static SRWLOCK frame_lock = SRWLOCK_INIT;
static void on_tick(void* game, LONG tick, DWORD now) {
    RuntimeFrame frame = {0}; frame.tick = tick; frame.milliseconds = now;
    runtime_game_tick(game, &frame);
}
enum RuntimeState runtime_state(void) { return (enum RuntimeState)InterlockedCompareExchange(&runtime.state, 0, 0); }
static int validate_profile(uintptr_t base) {
    static const unsigned char accessor[] = {0x55,0x8B,0xEC,0x8B,0x45,0x08,0x83,0xE0,0x03,
        0x8B,0x84,0x81,0x44,0x02,0x00,0x00,0x8B,0x40,0x10,0x5D,0xC2,0x04,0x00};
    static const unsigned char prefix[] = {0x55,0x8B,0xEC,0x6A,0xFF};
    __try {
        return !memcmp((void*)(base + STEAM_PLAYER_ACCESSOR_RVA), accessor, sizeof(accessor)) &&
            !memcmp((void*)(base + STEAM_GAME_TICK_RVA), prefix, sizeof(prefix)) &&
            *(uintptr_t*)(base + STEAM_GAME_VTABLE_RVA + STEAM_GAME_TICK_SLOT * sizeof(uintptr_t)) == base + STEAM_GAME_TICK_RVA;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
int runtime_start(uintptr_t base, const RuntimeConfig* config) {
    if (!validate_profile(base)) return 0;
    RuntimeBindings bindings = {actor_engine_bind, session_enable,
        action_hook_install, action_hook_stop, world_hook_install, world_hook_stop,
        world_hook_generation, tick_hook_install, tick_hook_stop,
        display_hook_install, display_hook_stop};
    return runtime_start_with_bindings(base, config, &bindings);
}
int runtime_start_with_bindings(uintptr_t base, const RuntimeConfig* config, const RuntimeBindings* bindings) {
    if (!base || !config || !bindings || !bindings->actor_bind || !bindings->session_enable || !bindings->action_install ||
        !bindings->action_stop || !bindings->world_install || !bindings->world_stop || !bindings->world_generation ||
        !bindings->tick_install || !bindings->tick_stop || !bindings->display_install || !bindings->display_stop ||
        (config->direct && (!memchr(config->host, 0, sizeof(config->host)) || !config->host[0] ||
         !memchr(config->name, 0, sizeof(config->name)) || !config->name[0] || !config->port)) ||
        runtime_state() != RT_OFF) return 0;
    memset(&runtime, 0, sizeof(runtime)); runtime.base = base; runtime.config = *config; runtime.bindings = *bindings;
    runtime.worker_thread = GetCurrentThreadId();
    runtime.display = runtime.logged_display = DISPLAY_PENDING;
    runtime.logged_session = SS_OFFLINE; runtime.display_ticks = 30;
    InterlockedExchange(&runtime.state, RT_STARTING);
    ActorEngine actor;
    if (!bindings->actor_bind(base, &actor)) goto fail;
    runtime.stage = ST_BOUND;
    if (!bindings->session_enable(base)) goto fail;
    runtime.stage = ST_SESSION;
    if (!multiplayer_initialize(base, &actor)) goto fail;
    runtime.stage = ST_COORDINATOR;
    if (config->direct && !session_direct(config->host, config->port, config->name)) goto fail;
    /* Attempted hook stages participate in rollback even if install fails. */
    runtime.stage = ST_ACTION;
    if (!bindings->action_install(base, multiplayer_is_replica, multiplayer_capture_shot)) goto fail;
    runtime.stage = ST_WORLD;
    if (!bindings->world_install(base)) goto fail;
    runtime.stage = ST_TICK;
    if (bindings->tick_install((void* volatile*)(base + STEAM_GAME_VTABLE_RVA + STEAM_GAME_TICK_SLOT * sizeof(uintptr_t)),
        (void*)(base + STEAM_GAME_TICK_RVA), on_tick) != SLOT_OK) goto fail;
    /* Display is configured PENDING; its first install runs on the game thread. */
    InterlockedExchange(&runtime.state, RT_RUNNING);
    if (config->log) fprintf(config->log, "# RUNTIME started session=1 actors=1 action=1 world=1 tick=1 display=pending\n");
    return 1;
fail:
    if (config->log) fprintf(config->log, "# RUNTIME startup failed stage=%d; rolling back\n", runtime.stage);
    runtime_stop(3000u); return 0;
}
void runtime_game_tick(void* game, RuntimeFrame* out) {
    enum RuntimeState state = runtime_state();
    if (state != RT_RUNNING && state != RT_STARTING && state != RT_STOPPING) return;
    LONG thread = (LONG)GetCurrentThreadId(), owner = InterlockedCompareExchange(&runtime.game_thread, thread, 0);
    if (owner && owner != thread) return;
    out->result = probe_read(runtime.base, &out->snapshot);
    if (out->snapshot.game != (uintptr_t)game) return;
    out->snapshot.world_load = runtime.bindings.world_generation();
    if (state != RT_STOPPING) {
        out->session_result = session_tick(&out->snapshot, out->result, out->milliseconds);
        if (out->session_result) { out->result = probe_read(runtime.base, &out->snapshot); out->snapshot.world_load = runtime.bindings.world_generation(); }
        if (runtime.display == DISPLAY_PENDING && ++runtime.display_ticks >= 30u) {
            runtime.display_ticks = 0; runtime.display = runtime.bindings.display_install(runtime.base, multiplayer_draw);
        }
    }
    out->session = session_state(); out->display = runtime.display;
    out->multiplayer = multiplayer_tick(&out->snapshot, out->result, out->milliseconds);
    if (TryAcquireSRWLockExclusive(&frame_lock)) { runtime.latest = *out; ReleaseSRWLockExclusive(&frame_lock); }
    if (runtime.config.observer) runtime.config.observer(out);
}
static void publish_frame(const RuntimeFrame* frame) {
    const Snapshot* sample = &frame->snapshot;
    MpState state = {0};
    if (frame->result == PROBE_OK && sample->in_level) {
        state.world_low = sample->world_low; state.world_high = sample->world_high; state.world_epoch = frame->multiplayer.world_epoch;
        state.active = sample->health > 0; state.tick = (uint32_t)frame->tick;
        state.x = sample->x; state.y = sample->y; state.z = sample->z; state.health = sample->health; state.max_health = sample->max_health > 0 ? sample->max_health : 0;
        state.animation = sample->animation; state.direction = sample->direction; state.weapon_slot = sample->weapon_slot;
        state.velocity = sample->velocity; state.moving = sample->moving;
        state.torso_direction = sample->torso_direction; state.torso_present = sample->torso_present; state.current_ammo = sample->current_ammo;
        memcpy(state.stored_ammo, sample->stored_ammo, sizeof(state.stored_ammo));
    }
    state_client_publish(runtime.client, &state, frame->milliseconds);
}
void runtime_worker_step(DWORD now) {
    if (runtime_state() != RT_RUNNING || GetCurrentThreadId() != runtime.worker_thread) return;
    ConnectionRequest command;
    if (session_take_request(&command)) {
        state_client_destroy(runtime.client); runtime.client = NULL; runtime.network = NS_OFF;
        runtime.generation = command.generation; runtime.connecting_at = now;
        if (command.connect) {
            runtime.client = state_client_create(command.host, command.port, command.name, runtime.config.log);
            if (runtime.client) runtime.network = NS_CONNECTING;
            else session_result(runtime.generation, 0, NULL);
        }
    }
    RuntimeFrame frame;
    AcquireSRWLockShared(&frame_lock); frame = runtime.latest; ReleaseSRWLockShared(&frame_lock);
    if (frame.tick) publish_frame(&frame);
    state_client_update(runtime.client, now);
    if (runtime.client && state_client_connected(runtime.client)) {
        /* Publish once per connection; the session keeps the result. */
        if (runtime.network != NS_CONNECTED) session_result(runtime.generation, 1, state_client_map(runtime.client));
        runtime.network = NS_CONNECTED;
    } else if (runtime.network == NS_CONNECTED || (runtime.network == NS_CONNECTING && now - runtime.connecting_at >= 10000u)) {
        session_result(runtime.generation, 0, NULL);
        state_client_destroy(runtime.client); runtime.client = NULL; runtime.network = NS_OFF;
    }
    PeerState peers[MP_MAX_PEERS];
    for (unsigned int id = 0; id < MP_MAX_PEERS; ++id) state_client_peer_info(runtime.client, id, now, &peers[id]);
    multiplayer_publish(peers, now);
    ShotEvent received;
    while (state_client_take_shot(runtime.client, &received)) multiplayer_receive_shot(&received, now);
    MpShot outgoing;
    while (multiplayer_take_local_shot(&outgoing)) state_client_send_shot(runtime.client, &outgoing, now);
    if (runtime.config.log && frame.display != runtime.logged_display) {
        fprintf(runtime.config.log, "# DISPLAY result=%d\n", frame.display); runtime.logged_display = frame.display;
    }
    if (runtime.config.log && frame.session != runtime.logged_session) {
        fprintf(runtime.config.log, "# SESSION state=%d\n", frame.session); runtime.logged_session = frame.session;
    }
    if (now - runtime.stats_at >= 5000u) { state_client_stats(runtime.client); runtime.stats_at = now; }
}
int runtime_stop(DWORD timeout_ms) {
    if (runtime_state() == RT_OFF) return 1;
    if (GetCurrentThreadId() != runtime.worker_thread) return 0;
    DWORD started = GetTickCount();
    InterlockedExchange(&runtime.state, RT_STOPPING);
    if (runtime.stage >= ST_SESSION) session_stop();
    if (runtime.stage >= ST_COORDINATOR) {
        multiplayer_request_stop();
        while (!multiplayer_stopped()) {
            if (GetTickCount() - started >= timeout_ms) {
                if (runtime.config.log) fprintf(runtime.config.log, "# RUNTIME cleanup timeout; game thread paused or native entities remain; hooks retained\n");
                state_client_destroy(runtime.client); runtime.client = NULL; runtime.network = NS_OFF; return 0;
            }
            Sleep(1);
        }
    }
    int display_ok = runtime.stage < ST_TICK || runtime.bindings.display_stop();
    int action_ok = runtime.stage < ST_ACTION || runtime.bindings.action_stop();
    int world_ok = runtime.stage < ST_WORLD || runtime.bindings.world_stop();
    enum SlotResult tick_result = runtime.stage < ST_TICK ? SLOT_OK : runtime.bindings.tick_stop();
    while (tick_result == SLOT_BUSY && GetTickCount() - started < timeout_ms) {
        Sleep(1); tick_result = runtime.bindings.tick_stop();
    }
    state_client_stats(runtime.client); state_client_destroy(runtime.client); runtime.client = NULL; runtime.network = NS_OFF;
    if (runtime.config.log) fprintf(runtime.config.log, "# HOOK_RESTORE action=%d display=%d world=%d tick=%d\n", action_ok, display_ok, world_ok, tick_result);
    if (!display_ok || !action_ok || !world_ok || tick_result != SLOT_OK) return 0;
    if (runtime.stage >= ST_COORDINATOR && !multiplayer_stop()) return 0;
    if (runtime.stage >= ST_SESSION) ui_stop();
    AcquireSRWLockExclusive(&frame_lock); memset(&runtime.latest, 0, sizeof(runtime.latest)); ReleaseSRWLockExclusive(&frame_lock);
    runtime.stage = ST_NONE; runtime.base = 0; InterlockedExchange(&runtime.game_thread, 0);
    InterlockedExchange(&runtime.state, RT_OFF); return 1;
}
