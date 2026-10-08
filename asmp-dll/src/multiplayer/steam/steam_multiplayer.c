#if defined(ASMP_STEAM_BUILD)
#include <string.h>
#include "steam_multiplayer.h"
#include "../../game/steam/steam_ui.h"
#include "../../game/steam/steam_display_hook.h"

typedef struct Remote {
    SteamActor actor;
    uint32_t session, epoch;
    int disabled;
} Remote;
typedef struct PendingShot { SteamShotEvent event; DWORD received; } PendingShot;
static uintptr_t base, local_game, local_player;
static uint32_t local_low, local_high, world_epoch;
static ActorEngine engine;
static int enabled;
static volatile LONG stop_requested, stopped = 1;
static volatile LONG game_thread;
static Remote remote[STEAM_MAX_PEERS];
static SRWLOCK incoming_lock = SRWLOCK_INIT, outgoing_lock = SRWLOCK_INIT;
static SteamPeerState incoming[STEAM_MAX_PEERS], current[STEAM_MAX_PEERS];
static DWORD published_at, current_at;
static PendingShot received[64], pending[64];
static unsigned int received_count, pending_count;
static MpSteamShot outgoing[64];
static unsigned int outgoing_head, outgoing_count;
static volatile LONG event_drops;

int steam_multiplayer_initialize(uintptr_t image_base, const ActorEngine* api)
{
    if (enabled || !image_base || !api || !api->create || !api->destroy || !api->move ||
        !api->rotate || !api->action || !api->weapon || !api->health) return 0;
    engine = *api; base = image_base;
    for (unsigned int i = 0; i < STEAM_MAX_PEERS; ++i) steam_actor_init(&remote[i].actor, &engine);
    enabled = 1; InterlockedExchange(&stopped, 0); return 1;
}
int steam_multiplayer_enable(uintptr_t image_base)
{
    ActorEngine api;
    return actor_engine_bind(image_base, &api) && steam_multiplayer_initialize(image_base, &api);
}
void steam_multiplayer_publish(const SteamPeerState peers[STEAM_MAX_PEERS], DWORD now)
{
    if (!enabled) return;
    AcquireSRWLockExclusive(&incoming_lock);
    memcpy(incoming, peers, sizeof(incoming)); published_at = now;
    ReleaseSRWLockExclusive(&incoming_lock);
}
void steam_multiplayer_receive_shot(const SteamShotEvent* event, DWORD now)
{
    if (!enabled || !event || event->id >= STEAM_MAX_PEERS) return;
    AcquireSRWLockExclusive(&incoming_lock);
    if (received_count < 64) {
        received[received_count].event = *event; received[received_count++].received = now;
    } else InterlockedIncrement(&event_drops);
    ReleaseSRWLockExclusive(&incoming_lock);
}
void steam_multiplayer_capture_shot(int x, int y, unsigned int weapon)
{
    if (GetCurrentThreadId() != (DWORD)InterlockedCompareExchange(&game_thread, 0, 0)) return;
    if (!enabled || !local_player || !world_epoch || weapon >= 10u ||
        InterlockedCompareExchange(&stop_requested, 0, 0)) return;
    Snapshot sample;
    if (probe_read(base, &sample) != PROBE_OK || sample.player != local_player ||
        sample.world_low != local_low || sample.world_high != local_high) return;
    if (!TryAcquireSRWLockExclusive(&outgoing_lock)) { InterlockedIncrement(&event_drops); return; }
    if (outgoing_count < 64) {
        MpSteamShot* shot = &outgoing[(outgoing_head + outgoing_count++) % 64];
        memset(shot, 0, sizeof(*shot)); shot->x = x; shot->y = y; shot->weapon = weapon;
        shot->world_low = local_low; shot->world_high = local_high; shot->world_epoch = world_epoch;
    } else InterlockedIncrement(&event_drops);
    ReleaseSRWLockExclusive(&outgoing_lock);
}
int steam_multiplayer_take_local_shot(MpSteamShot* out)
{
    int available = 0;
    AcquireSRWLockExclusive(&outgoing_lock);
    if (outgoing_count) {
        *out = outgoing[outgoing_head]; outgoing_head = (outgoing_head + 1) % 64;
        --outgoing_count; available = 1;
    }
    ReleaseSRWLockExclusive(&outgoing_lock);
    return available;
}
static void target_snapshot(Snapshot* out, const MpSteamState* in)
{
    memset(out, 0, sizeof(*out));
    out->x = in->x; out->y = in->y; out->z = in->z; out->health = in->health;
    out->direction = in->direction; out->torso_direction = in->torso_direction;
    out->torso_present = in->torso_present; out->velocity = in->velocity; out->moving = in->moving;
    out->weapon_slot = in->weapon_slot; out->current_ammo = in->current_ammo;
}
SteamMultiplayerFrame steam_multiplayer_tick(const Snapshot* local, enum ProbeResult state, DWORD now)
{
    SteamMultiplayerFrame result = {0};
    if (!enabled || InterlockedCompareExchange(&stopped, 0, 0)) return result;
    LONG thread = (LONG)GetCurrentThreadId();
    LONG owner = InterlockedCompareExchange(&game_thread, thread, 0);
    if (owner && owner != thread) return result;
    steam_display_hook_install(base, steam_multiplayer_draw);
    int stopping = InterlockedCompareExchange(&stop_requested, 0, 0) != 0;
    int gameplay = state == PROBE_OK && (local->world_low || local->world_high) && local->health > 0;
    if (!gameplay) {
        local_game = local_player = 0; local_low = local_high = 0;
    }
    if (gameplay && (local->game != local_game || local->player != local_player ||
        local->world_low != local_low || local->world_high != local_high)) {
        if (!++world_epoch) ++world_epoch;
        local_game = local->game; local_player = local->player;
        local_low = local->world_low; local_high = local->world_high;
    }
    /* Flush captures from an old map before the worker can send them. */
    if (TryAcquireSRWLockExclusive(&outgoing_lock)) {
        if (!gameplay || stopping || (outgoing_count &&
            outgoing[outgoing_head].world_epoch != world_epoch)) outgoing_count = outgoing_head = 0;
        ReleaseSRWLockExclusive(&outgoing_lock);
    }
    result.world_epoch = world_epoch;
    if (TryAcquireSRWLockExclusive(&incoming_lock)) {
        memcpy(current, incoming, sizeof(current));
        current_at = published_at;
        for (unsigned int i = 0; i < received_count; ++i) {
            if (pending_count < 64) pending[pending_count++] = received[i];
            else ++result.shots_discarded;
        }
        received_count = 0;
        ReleaseSRWLockExclusive(&incoming_lock);
    }
    int any_live = 0;
    for (unsigned int id = 0; id < STEAM_MAX_PEERS; ++id) {
        Remote* r = &remote[id]; const SteamPeerState* peer = &current[id];
        const MpSteamState* s = &peer->state;
        if (r->disabled == 1 && r->actor.game != local->game &&
            !steam_actor_live(&r->actor, local->game)) {
            steam_actor_init(&r->actor, &engine); r->disabled = 0;
        }
        if (r->disabled == 1) continue; /* Native exception: map owns remaining storage. */
        int wanted = !r->disabled && gameplay && !stopping && peer->present && now - current_at <= 1000u &&
            s->active && s->health > 0 && s->world_epoch && s->world_low == local->world_low &&
            s->world_high == local->world_high;
        int changed = r->session != peer->session || r->epoch != s->world_epoch;
        Snapshot target; target_snapshot(&target, s);
        ActorResult update = steam_actor_tick(&r->actor, base, local, state, &target,
            now, !wanted || changed, 1, 0.0f, 0);
        if (update.event != ACTOR_NONE) {
            result.remote[result.count].id = id;
            result.remote[result.count++].actor = update;
        }
        if (update.event == ACTOR_FAULT) r->disabled = 1;
        if (update.event == ACTOR_REJECTED) r->disabled = 2;
        if (r->disabled == 2 && !r->actor.entity) r->disabled = 1;
        if (!r->actor.entity && !r->disabled && (!wanted || changed || update.event == ACTOR_LOST)) {
            steam_actor_init(&r->actor, &engine); r->session = peer->session; r->epoch = s->world_epoch;
        }
        if (r->actor.entity && !r->disabled) {
            any_live = 1;
            if (steam_ui_name(&r->actor, peer->name) < 0) r->disabled = 1;
        }
    }
    unsigned int keep = 0; unsigned int fired = 0;
    for (unsigned int i = 0; i < pending_count; ++i) {
        PendingShot p = pending[i]; SteamShotEvent* e = &p.event;
        Remote* r = &remote[e->id]; const MpSteamState* s = &current[e->id].state;
        if (stopping || !gameplay || now - p.received > 2500u || r->disabled ||
            e->session != r->session || e->shot.world_epoch != r->epoch ||
            e->shot.world_low != local->world_low || e->shot.world_high != local->world_high ||
            !current[e->id].present || !s->active || s->health <= 0) {
            ++result.shots_discarded; continue;
        }
        if (!(fired & (1u << e->id)) && r->actor.prepared) {
            int applied = steam_actor_shoot(&r->actor, local->game, e->shot.x, e->shot.y, (int)e->shot.weapon);
            if (applied) {
                fired |= 1u << e->id;
                if (applied > 0) ++result.shots_applied;
                else { r->disabled = 1; ++result.shots_discarded; }
                continue;
            }
        }
        pending[keep++] = p;
    }
    pending_count = keep;
    result.shots_discarded += (unsigned int)InterlockedExchange(&event_drops, 0);
    if (stopping && !any_live) InterlockedExchange(&stopped, 1);
    return result;
}
int steam_multiplayer_is_replica(uintptr_t entity) {
    if (GetCurrentThreadId() != (DWORD)InterlockedCompareExchange(&game_thread, 0, 0)) return 0;
    if (!enabled || !local_game) return 0;
    for (unsigned int i = 0; i < STEAM_MAX_PEERS; ++i)
        if (remote[i].actor.entity == entity && steam_actor_live(&remote[i].actor, local_game)) return 1;
    return 0;
}
void steam_multiplayer_draw(void) {
    if (GetCurrentThreadId() != (DWORD)InterlockedCompareExchange(&game_thread, 0, 0)) return;
    if (!enabled || !local_game || InterlockedCompareExchange(&stop_requested, 0, 0)) return;
    __try {
        if (*(uintptr_t*)(base + STEAM_GAME_PTR_RVA) != local_game) return;
        for (unsigned int i = 0; i < STEAM_MAX_PEERS; ++i)
            if (!remote[i].disabled && remote[i].actor.prepared && steam_actor_live(&remote[i].actor, local_game))
                steam_ui_health_bar(local_game, remote[i].actor.entity, current[i].state.health);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void steam_multiplayer_request_stop(void) { InterlockedExchange(&stop_requested, 1); }
int steam_multiplayer_stopped(void) { return InterlockedCompareExchange(&stopped, 0, 0) != 0; }
#endif
