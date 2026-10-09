#include <string.h>
#include "multiplayer.h"
#include "pose_buffer.h"
#include "../game/ui.h"
#include "../game/display_hook.h"

typedef struct Remote {
    Actor actor;
    uint32_t session, epoch;
    uint32_t world_low, world_high, local_epoch;
    enum RemoteState state;
    DWORD entered_at, retry_at, last_pose;
    DWORD weapon_retry_at;
    unsigned int failures;
    /* Last slot armed natively and last slot reported as rejected. */
    int last_weapon, rejected_weapon;
    /* Received positions, played back a fixed delay behind the newest. */
    PoseBuffer pose;
} Remote;
typedef struct PendingShot { ShotEvent event; DWORD received; } PendingShot;
static uintptr_t base, local_game, local_player, armory_player;
static uint32_t armory_epoch;
static uint32_t local_low, local_high, world_epoch, map_started, world_load;
static ActorEngine engine;
static int enabled;
static volatile LONG stop_requested, stopped = 1;
static volatile LONG game_thread;
static Remote remote[MP_MAX_PEERS];
static SRWLOCK incoming_lock = SRWLOCK_INIT, outgoing_lock = SRWLOCK_INIT;
static PeerState incoming[MP_MAX_PEERS], current[MP_MAX_PEERS];
static DWORD published_at, current_at;
static PendingShot received[64], pending[64];
static unsigned int received_count, pending_count;
/* Game-thread staging: captures never depend on the outgoing lock. */
static MpShot staged[64];
static unsigned int staged_count;
static MpShot outgoing[64];
static unsigned int outgoing_head, outgoing_count;
static volatile LONG event_drops;

int multiplayer_initialize(uintptr_t image_base, const ActorEngine* api)
{
    if (enabled || !image_base || !api || !api->create || !api->destroy || !api->move ||
        !api->rotate || !api->action || !api->weapon || !api->health) return 0;
    engine = *api; base = image_base;
    memset(remote, 0, sizeof(remote));
    for (unsigned int i = 0; i < MP_MAX_PEERS; ++i) {
        actor_init(&remote[i].actor, &engine); remote[i].last_weapon = remote[i].rejected_weapon = -1;
    }
    local_game = local_player = armory_player = 0; armory_epoch = 0; local_low = local_high = world_epoch = map_started = world_load = 0;
    memset(incoming, 0, sizeof(incoming)); memset(current, 0, sizeof(current));
    published_at = current_at = received_count = pending_count = outgoing_head = outgoing_count = staged_count = 0;
    InterlockedExchange(&stop_requested, 0); InterlockedExchange(&game_thread, 0); InterlockedExchange(&event_drops, 0);
    enabled = 1; InterlockedExchange(&stopped, 0); return 1;
}
int multiplayer_enable(uintptr_t image_base)
{
    ActorEngine api;
    return actor_engine_bind(image_base, &api) && multiplayer_initialize(image_base, &api);
}
void multiplayer_publish(const PeerState peers[MP_MAX_PEERS], DWORD now)
{
    if (!enabled) return;
    AcquireSRWLockExclusive(&incoming_lock);
    memcpy(incoming, peers, sizeof(incoming)); published_at = now;
    ReleaseSRWLockExclusive(&incoming_lock);
}
void multiplayer_receive_shot(const ShotEvent* event, DWORD now)
{
    if (!enabled || !event || event->id >= MP_MAX_PEERS) return;
    AcquireSRWLockExclusive(&incoming_lock);
    if (received_count < 64) {
        received[received_count].event = *event; received[received_count++].received = now;
    } else InterlockedIncrement(&event_drops);
    ReleaseSRWLockExclusive(&incoming_lock);
}
void multiplayer_capture_shot(int x, int y, unsigned int weapon)
{
    if (GetCurrentThreadId() != (DWORD)InterlockedCompareExchange(&game_thread, 0, 0)) return;
    if (!enabled || !local_player || !world_epoch || weapon >= 10u ||
        InterlockedCompareExchange(&stop_requested, 0, 0)) return;
    Snapshot sample;
    if (probe_read(base, &sample) != PROBE_OK || !sample.in_level || sample.health <= 0 || sample.player != local_player ||
        sample.world_low != local_low || sample.world_high != local_high) return;
    if (staged_count < 64) {
        MpShot* shot = &staged[staged_count++];
        memset(shot, 0, sizeof(*shot)); shot->x = x; shot->y = y; shot->weapon = weapon;
        shot->world_low = local_low; shot->world_high = local_high; shot->world_epoch = world_epoch;
    } else InterlockedIncrement(&event_drops);
}
int multiplayer_take_local_shot(MpShot* out)
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
static void target_snapshot(Snapshot* out, const MpState* in)
{
    memset(out, 0, sizeof(*out));
    out->x = in->x; out->y = in->y; out->z = in->z; out->health = in->health;
    out->direction = in->direction; out->torso_direction = in->torso_direction;
    out->torso_present = in->torso_present; out->velocity = in->velocity; out->moving = in->moving;
    out->weapon_slot = in->weapon_slot; out->current_ammo = in->current_ammo;
}
static void abandon(Remote* r, uintptr_t game, ActorResult* update, int* cleanup_failed)
{
    /* D1: exactly one guarded cleanup attempt after a native fault. */
    if (r->state != RS_ABANDONED && r->actor.entity) {
        ActorResult cleanup = actor_remove(&r->actor, game);
        *cleanup_failed = cleanup.event == ACTOR_FAULT || r->actor.entity != 0;
    }
    r->state = RS_ABANDONED;
    update->event = ACTOR_FAULT; update->reason = ACTOR_REASON_EXCEPTION;
}
/* The shown position is interpolated between received samples on the sender's
   tick timeline (see pose_buffer.h), not pulled toward the latest packet. */
static void interpolate_pose(Remote* r, const MpState* state, Snapshot* target, DWORD now)
{
    pose_push(&r->pose, state->sequence, state->tick, now, state->x, state->y, state->z);
    pose_sample(&r->pose, now, &target->x, &target->y, &target->z);
}
static void forget_weapon(Remote* r) { r->last_weapon = r->rejected_weapon = -1; r->weapon_retry_at = 0; }
static void report(MultiplayerFrame* frame, unsigned int id, const Remote* r, const ActorResult* update, int cleanup_failed)
{
    /* One entry per peer: a later event in the same tick replaces the earlier one. */
    unsigned int i = 0;
    while (i < frame->count && frame->remote[i].id != id) ++i;
    if (i == frame->count) ++frame->count;
    frame->remote[i].id = id; frame->remote[i].actor = *update;
    frame->remote[i].state = r->state; frame->remote[i].cleanup_failed = cleanup_failed;
}
static void backoff(Remote* r, DWORD now)
{
    DWORD delay = r->failures < 4u ? (2000u << r->failures) : 30000u;
    if (r->failures < 4u) ++r->failures;
    r->retry_at = now + delay; r->state = RS_BACKOFF;
}
MultiplayerFrame multiplayer_tick(const Snapshot* local, enum ProbeResult state, DWORD now)
{
    MultiplayerFrame result = {0};
    if (!enabled || InterlockedCompareExchange(&stopped, 0, 0)) return result;
    LONG thread = (LONG)GetCurrentThreadId();
    LONG owner = InterlockedCompareExchange(&game_thread, thread, 0);
    if (owner && owner != thread) return result;
    int stopping = InterlockedCompareExchange(&stop_requested, 0, 0) != 0;
    /* Death can temporarily remove the player. The probe still validates GAME
       and parses its map before returning NO_PLAYER/NO_ARMY. */
    int gameplay = (state == PROBE_OK || state == PROBE_NO_PLAYER || state == PROBE_NO_ARMY) && local->in_level;
    if (!gameplay) {
        local_game = local_player = 0; local_low = local_high = 0;
    }
    if (gameplay && (local->world_low != local_low || local->world_high != local_high || local->map_started != map_started || local->world_load != world_load)) {
        if (!++world_epoch) ++world_epoch;
        map_started = local->map_started;
        world_load = local->world_load;
        local_low = local->world_low; local_high = local->world_high;
    }
    if (gameplay) { local_game = local->game; local_player = state == PROBE_OK && local->health > 0 ? local->player : 0; }
    /* Every player owns every weapon: grant once per local player entity and world. */
    if (local_player && !stopping && (local_player != armory_player || world_epoch != armory_epoch)) {
        armory_player = local_player; armory_epoch = world_epoch;
        actor_grant_all_weapons(&engine, local_player);
    }
    if (!local_player) armory_player = 0;
    /* Drop captures from an old map before the worker can send them; the rest
       wait in the staging array until the outgoing lock is free. */
    if (!gameplay || stopping) staged_count = 0;
    else {
        unsigned int kept = 0;
        for (unsigned int i = 0; i < staged_count; ++i)
            if (staged[i].world_epoch == world_epoch) staged[kept++] = staged[i];
        staged_count = kept;
    }
    if (TryAcquireSRWLockExclusive(&outgoing_lock)) {
        if (!gameplay || stopping || (outgoing_count &&
            outgoing[outgoing_head].world_epoch != world_epoch)) outgoing_count = outgoing_head = 0;
        unsigned int moved = 0;
        while (moved < staged_count && outgoing_count < 64) {
            outgoing[(outgoing_head + outgoing_count++) % 64] = staged[moved++];
        }
        ReleaseSRWLockExclusive(&outgoing_lock);
        if (moved) { memmove(staged, staged + moved, (staged_count - moved) * sizeof(staged[0])); staged_count -= moved; }
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
    for (unsigned int id = 0; id < MP_MAX_PEERS; ++id) {
        Remote* r = &remote[id]; const PeerState* peer = &current[id];
        const MpState* s = &peer->state;
        int world_changed = r->world_low != local_low || r->world_high != local_high || r->local_epoch != world_epoch;
        if (r->state == RS_ABANDONED && world_changed && !actor_live(&r->actor, local->game)) {
            actor_init(&r->actor, &engine); r->state = RS_IDLE; r->failures = 0;
        }
        int wanted = gameplay && !stopping && peer->present && now - current_at <= 1000u &&
            s->active && s->health > 0 && s->world_epoch && s->world_low == local->world_low &&
            s->world_high == local->world_high;
        int changed = r->session != peer->session || r->epoch != s->world_epoch;
        Snapshot target; target_snapshot(&target, s);
        ActorResult update = {0}; int cleanup_failed = 0;
        if (r->state != RS_ABANDONED && (!wanted || changed || world_changed)) {
            update = actor_remove(&r->actor, local->game);
            if (update.event == ACTOR_FAULT) abandon(r, local->game, &update, &cleanup_failed);
            else { r->state = RS_IDLE; r->failures = 0; forget_weapon(r); }
        }
        if (r->state == RS_ABANDONED) {
            if (stopping && actor_live(&r->actor, local->game)) {
                any_live = 1; cleanup_failed = 1;
                update.event = ACTOR_FAULT; update.reason = ACTOR_REASON_EXCEPTION; update.entity = r->actor.entity;
            }
        } else if (wanted) {
            if ((r->state == RS_SPAWNING || r->state == RS_SPAWNED) && !actor_live(&r->actor, local->game)) {
                update.event = ACTOR_LOST; update.entity = r->actor.entity;
                r->actor.entity = 0; r->state = RS_IDLE; forget_weapon(r);
            }
            if (r->state == RS_IDLE) {
                r->session = peer->session; r->epoch = s->world_epoch;
                r->world_low = local_low; r->world_high = local_high; r->local_epoch = world_epoch;
                r->entered_at = now; r->state = RS_WAITING;
            } else if (r->state == RS_BACKOFF && (int32_t)(now - r->retry_at) >= 0) {
                r->state = RS_WAITING; r->entered_at = now - 2000u;
            }
            if (r->state == RS_WAITING && now - r->entered_at >= 2000u && state == PROBE_OK && local->player) {
                update = actor_spawn(&r->actor, base, local, &target);
                if (update.event == ACTOR_SPAWNED) { r->state = RS_SPAWNING; r->entered_at = r->last_pose = now; }
                else if (update.event == ACTOR_FAULT) abandon(r, local->game, &update, &cleanup_failed);
                else {
                    ActorResult cleanup = actor_remove(&r->actor, local->game);
                    if (cleanup.event == ACTOR_FAULT) abandon(r, local->game, &update, &cleanup_failed);
                    else backoff(r, now);
                }
            } else if (r->state == RS_SPAWNING) {
                int torso = actor_torso_ready(&r->actor);
                if (torso < 0) abandon(r, local->game, &update, &cleanup_failed);
                else if (torso) {
                    if (actor_set_army(&r->actor, local->army_index) != 1) abandon(r, local->game, &update, &cleanup_failed);
                    else { r->state = RS_SPAWNED; r->failures = 0; }
                } else if (now - r->entered_at >= 3000u) {
                    update = actor_remove(&r->actor, local->game);
                    if (update.event == ACTOR_FAULT) abandon(r, local->game, &update, &cleanup_failed);
                    else backoff(r, now);
                }
            }
            if (r->state == RS_SPAWNED) {
                /* A rejected arm is retried at most once per second and reported once per slot. */
                if (target.weapon_slot < 0) r->last_weapon = target.weapon_slot;
                else if (target.weapon_slot != r->last_weapon &&
                    (target.weapon_slot != r->rejected_weapon || (int32_t)(now - r->weapon_retry_at) >= 0)) {
                    int armed = actor_arm(&r->actor, target.weapon_slot);
                    if (armed < 0) abandon(r, local->game, &update, &cleanup_failed);
                    else if (armed) { r->last_weapon = target.weapon_slot; r->rejected_weapon = -1; }
                    else {
                        if (target.weapon_slot != r->rejected_weapon) {
                            update.event = ACTOR_REJECTED; update.reason = ACTOR_REASON_WEAPON; update.entity = r->actor.entity;
                        }
                        r->rejected_weapon = target.weapon_slot; r->weapon_retry_at = now + 1000u;
                    }
                }
                if (r->state == RS_SPAWNED) {
                    interpolate_pose(r, s, &target, now);
                    ActorResult pose = actor_apply(&r->actor, &target);
                    if (pose.event == ACTOR_FAULT) { update = pose; abandon(r, local->game, &update, &cleanup_failed); }
                    else if (pose.event == ACTOR_LOST) { update = pose; r->state = RS_IDLE; forget_weapon(r); }
                    else if (now - r->last_pose >= 1000u && update.event == ACTOR_NONE) { update = pose; r->last_pose = now; }
                }
            }
        }
        if (r->state != RS_SPAWNED) pose_reset(&r->pose);
        if (update.event != ACTOR_NONE) report(&result, id, r, &update, cleanup_failed);
        if (actor_live(&r->actor, local->game)) any_live = 1;
    }
    unsigned int keep = 0; unsigned int fired = 0;
    for (unsigned int i = 0; i < pending_count; ++i) {
        PendingShot p = pending[i]; ShotEvent* e = &p.event;
        Remote* r = &remote[e->id]; const MpState* s = &current[e->id].state;
        if (stopping || !gameplay || now - p.received > 2500u || r->state == RS_ABANDONED ||
            e->session != r->session || e->shot.world_epoch != r->epoch ||
            e->shot.world_low != local->world_low || e->shot.world_high != local->world_high ||
            !current[e->id].present || !s->active || s->health <= 0) {
            ++result.shots_discarded; continue;
        }
        if (!(fired & (1u << e->id)) && r->state == RS_SPAWNED) {
            int applied = actor_shoot(&r->actor, local->game, e->shot.x, e->shot.y, (int)e->shot.weapon);
            if (applied) {
                fired |= 1u << e->id;
                if (applied > 0) ++result.shots_applied;
                else {
                    ActorResult fault = {0}; int failed = 0; fault.entity = r->actor.entity;
                    abandon(r, local->game, &fault, &failed); report(&result, e->id, r, &fault, failed);
                    ++result.shots_discarded;
                }
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
int multiplayer_is_replica(uintptr_t entity) {
    if (GetCurrentThreadId() != (DWORD)InterlockedCompareExchange(&game_thread, 0, 0)) return 0;
    if (!enabled || !local_game) return 0;
    for (unsigned int i = 0; i < MP_MAX_PEERS; ++i)
        if (remote[i].actor.entity == entity && actor_live(&remote[i].actor, local_game)) return 1;
    return 0;
}
void multiplayer_draw(void) {
    if (GetCurrentThreadId() != (DWORD)InterlockedCompareExchange(&game_thread, 0, 0)) return;
    if (!enabled || !local_game || InterlockedCompareExchange(&stop_requested, 0, 0)) return;
    __try {
        if (*(uintptr_t*)(base + STEAM_GAME_PTR_RVA) != local_game) return;
        for (unsigned int i = 0; i < MP_MAX_PEERS; ++i)
            if (remote[i].state == RS_SPAWNED && actor_live(&remote[i].actor, local_game))
            {
                ui_health_bar(local_game, remote[i].actor.entity, current[i].state.health);
                char label[sizeof(current[i].name) + 1];
                memcpy(label, current[i].name, sizeof(current[i].name)); label[sizeof(current[i].name)] = 0;
                ui_name_label(local_game, remote[i].actor.entity, label);
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void multiplayer_request_stop(void) {
    InterlockedExchange(&stop_requested, 1);
    /* Startup rollback before the first tick cannot own any native entity. */
    if (!InterlockedCompareExchange(&game_thread, 0, 0)) InterlockedExchange(&stopped, 1);
}
int multiplayer_stopped(void) { return InterlockedCompareExchange(&stopped, 0, 0) != 0; }
int multiplayer_stop(void) {
    if (!multiplayer_stopped()) return 0;
    enabled = 0; base = local_game = local_player = 0; InterlockedExchange(&game_thread, 0);
    AcquireSRWLockExclusive(&incoming_lock);
    memset(incoming, 0, sizeof(incoming)); memset(current, 0, sizeof(current));
    received_count = pending_count = 0; published_at = current_at = 0;
    ReleaseSRWLockExclusive(&incoming_lock);
    AcquireSRWLockExclusive(&outgoing_lock); outgoing_count = outgoing_head = 0; ReleaseSRWLockExclusive(&outgoing_lock);
    staged_count = 0;
    return 1;
}
enum RemoteState multiplayer_remote_state(unsigned int id) { return id < MP_MAX_PEERS ? remote[id].state : RS_IDLE; }
