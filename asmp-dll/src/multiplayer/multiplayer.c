/* Remote player coordinator: per peer it runs the explicit state machine (idle,
   waiting, spawning, spawned, backoff, abandoned), spawns and removes replica entities,
   applies interpolated poses and replays shots. Layer: multiplayer/ (applies received
   data through the game/ engine API). Native work only on the game thread; the worker
   hands data over through the incoming and outgoing locks. */

#include <string.h>
#include "multiplayer.h"
#include "pose_buffer.h"
#include "retry_policy.h"
#include "../game/ui.h"
#include "../game/display_hook.h"

/* A peer must wait this long in RS_WAITING before its actor is spawned. */
#define PEER_SPAWN_DELAY_MS 2000u
/* Peer states older than this are treated as absent. */
#define PEER_STATE_TIMEOUT_MS 1000u
/* A spawned actor must show its native torso within this time. */
#define TORSO_TIMEOUT_MS 3000u
/* A rejected weapon slot is tried again after this delay. */
#define WEAPON_RETRY_MS 1000u
/* Interval of the routine pose report while nothing else happens. */
#define POSE_REPORT_MS 1000u
/* Interval of re-showing the granted weapon icons. */
#define WEAPON_ICONS_REFRESH_MS 1000u
/* A received shot older than this is discarded instead of replayed. */
#define SHOT_MAX_AGE_MS 2500u
/* Capacity of each shot queue (received, pending, staged, outgoing). */
#define SHOT_QUEUE_CAPACITY 64u

typedef struct Remote
{
    Actor actor;
    enum RemoteState state;
    uint32_t session; /* Peer session the replica was spawned for. */
    uint32_t epoch;   /* Peer world epoch the replica was spawned for. */
    DWORD entered_at;
    DWORD last_pose;

    /* Local world the replica was spawned in. */
    struct
    {
        uint32_t low;
        uint32_t high;
        uint32_t local_epoch;
    } world;

    struct
    {
        DWORD at;
        unsigned int failures;
    } retry;

    /* Last slot armed natively and last slot reported as rejected. */
    struct
    {
        int last;
        int rejected;
        DWORD retry_at;
    } weapon;

    /* Received positions, played back a fixed delay behind the newest. */
    PoseBuffer pose;
} Remote;

typedef struct PendingShot
{
    ShotEvent event;
    DWORD received;
} PendingShot;

static uintptr_t base, local_game, local_player, armory_player;
static uint32_t armory_epoch;
static DWORD icons_at;
static uint32_t local_low, local_high, world_epoch, map_started, world_load;
static ActorEngine engine;
static int enabled;
static volatile LONG stop_requested, stopped = 1;
static volatile LONG game_thread;
static Remote remote[MP_MAX_PEERS];
static SRWLOCK incoming_lock = SRWLOCK_INIT, outgoing_lock = SRWLOCK_INIT;
static PeerState incoming[MP_MAX_PEERS], current[MP_MAX_PEERS];
static DWORD published_at, current_at;
static PendingShot received[SHOT_QUEUE_CAPACITY], pending[SHOT_QUEUE_CAPACITY];
static unsigned int received_count, pending_count;
/* Game-thread staging: captures never depend on the outgoing lock. */
static MpShot staged[SHOT_QUEUE_CAPACITY];
static unsigned int staged_count;
static MpShot outgoing[SHOT_QUEUE_CAPACITY];
static unsigned int outgoing_head, outgoing_count;
static volatile LONG event_drops;

/* Binds the engine functions and resets all peers. Returns 0 if already enabled or an
   engine function is missing. */
static int engine_complete(const ActorEngine* api)
{
    if (!api->create || !api->destroy)
        return 0;
    if (!api->move || !api->rotate)
        return 0;
    return api->action && api->weapon && api->health;
}

int multiplayer_initialize(uintptr_t image_base, const ActorEngine* api)
{
    if (enabled || !image_base || !api || !engine_complete(api))
        return 0;

    engine = *api;
    base = image_base;
    memset(remote, 0, sizeof(remote));
    for (unsigned int i = 0; i < MP_MAX_PEERS; ++i)
    {
        actor_init(&remote[i].actor, &engine);
        remote[i].weapon.last = -1;
        remote[i].weapon.rejected = -1;
    }

    local_game = 0;
    local_player = 0;
    armory_player = 0;
    armory_epoch = 0;
    local_low = 0;
    local_high = 0;
    world_epoch = 0;
    map_started = 0;
    world_load = 0;

    memset(incoming, 0, sizeof(incoming));
    memset(current, 0, sizeof(current));
    published_at = 0;
    current_at = 0;
    received_count = 0;
    pending_count = 0;
    outgoing_head = 0;
    outgoing_count = 0;
    staged_count = 0;

    InterlockedExchange(&stop_requested, 0);
    InterlockedExchange(&game_thread, 0);
    InterlockedExchange(&event_drops, 0);
    enabled = 1;
    InterlockedExchange(&stopped, 0);
    return 1;
}

/* Binds the real engine and initializes. */
int multiplayer_enable(uintptr_t image_base)
{
    ActorEngine api;
    return actor_engine_bind(image_base, &api) && multiplayer_initialize(image_base, &api);
}

/* Worker: replaces the peer states the game thread will read next tick. */
void multiplayer_publish(const PeerState peers[MP_MAX_PEERS], DWORD now)
{
    if (!enabled)
        return;
    AcquireSRWLockExclusive(&incoming_lock);
    memcpy(incoming, peers, sizeof(incoming));
    published_at = now;
    ReleaseSRWLockExclusive(&incoming_lock);
}

/* Worker: queues a received shot for replay; a full queue is counted as dropped. */
void multiplayer_receive_shot(const ShotEvent* event, DWORD now)
{
    if (!enabled || !event || event->id >= MP_MAX_PEERS)
        return;
    AcquireSRWLockExclusive(&incoming_lock);
    if (received_count < SHOT_QUEUE_CAPACITY)
    {
        received[received_count].event = *event;
        received[received_count++].received = now;
    }
    else
        InterlockedIncrement(&event_drops);
    ReleaseSRWLockExclusive(&incoming_lock);
}

/* Game thread, from the action hook: stages a local shot. Never takes the outgoing lock;
   flush_local_shots moves it later. Ignored outside the current world. */
void multiplayer_capture_shot(int x, int y, unsigned int weapon)
{
    if (GetCurrentThreadId() != (DWORD)InterlockedCompareExchange(&game_thread, 0, 0))
        return;
    if (!enabled || !local_player || !world_epoch)
        return;
    if (weapon >= STEAM_WEAPON_SLOT_COUNT || InterlockedCompareExchange(&stop_requested, 0, 0))
        return;

    /* Only a shot by the living local player in the current world is sent. */
    Snapshot sample;
    if (probe_read(base, &sample) != PROBE_OK || !sample.in_level || sample.health <= 0)
        return;
    if (sample.player != local_player)
        return;
    if (sample.world_low != local_low || sample.world_high != local_high)
        return;

    if (staged_count < SHOT_QUEUE_CAPACITY)
    {
        MpShot* shot = &staged[staged_count++];
        memset(shot, 0, sizeof(*shot));
        shot->x = x;
        shot->y = y;
        shot->weapon = weapon;
        shot->world_low = local_low;
        shot->world_high = local_high;
        shot->world_epoch = world_epoch;
    }
    else
        InterlockedIncrement(&event_drops);
}

/* Worker: pops the next staged local shot. */
int multiplayer_take_local_shot(MpShot* out)
{
    int available = 0;
    AcquireSRWLockExclusive(&outgoing_lock);
    if (outgoing_count)
    {
        *out = outgoing[outgoing_head];
        outgoing_head = (outgoing_head + 1) % SHOT_QUEUE_CAPACITY;
        --outgoing_count;
        available = 1;
    }
    ReleaseSRWLockExclusive(&outgoing_lock);
    return available;
}

/* Converts a received MpState into the Snapshot that Actor applies. */
static void target_snapshot(Snapshot* out, const MpState* in)
{
    memset(out, 0, sizeof(*out));
    out->x = in->x;
    out->y = in->y;
    out->z = in->z;
    out->health = in->health;
    out->direction = in->direction;
    out->torso_direction = in->torso_direction;
    out->torso_present = in->torso_present;
    out->velocity = in->velocity;
    out->moving = in->moving;
    out->weapon_slot = in->weapon_slot;
    out->current_ammo = in->current_ammo;
}

/* Decision D1: one guarded removal after a native fault, then the peer stays abandoned
   until the world changes. */
static void abandon(Remote* r, uintptr_t game, ActorResult* update, int* cleanup_failed)
{
    /* D1: exactly one guarded cleanup attempt after a native fault. */
    if (r->state != RS_ABANDONED && r->actor.entity)
    {
        ActorResult cleanup = actor_remove(&r->actor, game);
        *cleanup_failed = cleanup.event == ACTOR_FAULT || r->actor.entity != 0;
    }
    r->state = RS_ABANDONED;
    update->event = ACTOR_FAULT;
    update->reason = ACTOR_REASON_EXCEPTION;
}

/* The shown position is interpolated between received samples on the sender's
   tick timeline (see pose_buffer.h), not pulled toward the latest packet. */
static void interpolate_pose(Remote* r, const MpState* state, Snapshot* target, DWORD now)
{
    pose_push(&r->pose, state->sequence, state->tick, now, state->x, state->y, state->z);
    pose_sample(&r->pose, now, &target->x, &target->y, &target->z);
}

/* Clears the cached weapon so the next pose arms the replica again. */
static void forget_weapon(Remote* r)
{
    r->weapon.last = -1;
    r->weapon.rejected = -1;
    r->weapon.retry_at = 0;
}

/* Records this tick's result for peer `id`; a later event in the same tick replaces it. */
static void report(MultiplayerFrame* frame, unsigned int id, const Remote* r,
                   const ActorResult* update, int cleanup_failed)
{
    /* One entry per peer: a later event in the same tick replaces the earlier one. */
    unsigned int i = 0;
    while (i < frame->count && frame->remote[i].id != id)
        ++i;
    if (i == frame->count)
        ++frame->count;
    frame->remote[i].id = id;
    frame->remote[i].actor = *update;
    frame->remote[i].state = r->state;
    frame->remote[i].cleanup_failed = cleanup_failed;
}

/* Moves the peer to BACKOFF; the delay doubles from 2 s up to 30 s per failure. */
static void schedule_retry(Remote* r, DWORD now)
{
    DWORD delay = mp_retry_delay(r->retry.failures);
    /* Stop counting once the delay is capped, so the counter cannot overflow. */
    if (delay < MP_RETRY_MAX_MS)
        ++r->retry.failures;
    r->retry.at = now + delay;
    r->state = RS_BACKOFF;
}

/* What one tick knows about the local game, shared by every tick step. */
typedef struct LocalTick
{
    const Snapshot* local;
    enum ProbeResult probe;
    DWORD now;
    int gameplay; /* In a level; the player may be dead. */
    int stopping;
} LocalTick;

/* Working state of one peer during one tick: its received state, the target
   pose, and the result that will be reported for it. */
typedef struct RemoteTick
{
    const PeerState* peer;
    Snapshot target;
    ActorResult update;
    int cleanup_failed;
} RemoteTick;

/* Tracks the local world: a new map key, map start or world load begins a new
   world epoch. Returns 1 when the local player is in a level. */
static int update_local_world(const Snapshot* local, enum ProbeResult state)
{
    int gameplay = probe_in_level(local, state);
    if (!gameplay)
    {
        local_game = 0;
        local_player = 0;
        local_low = 0;
        local_high = 0;
        return 0;
    }

    int same_map = local->world_low == local_low && local->world_high == local_high;
    int same_load = local->map_started == map_started && local->world_load == world_load;
    if (!same_map || !same_load)
    {
        if (!++world_epoch)
            ++world_epoch;
        map_started = local->map_started;
        world_load = local->world_load;
        local_low = local->world_low;
        local_high = local->world_high;
    }
    local_game = local->game;
    local_player = 0;
    if (state == PROBE_OK && local->health > 0)
        local_player = local->player;
    return 1;
}

/* Every player owns every weapon: grant them once per local player entity and
   world, and keep the granted icons visible on the native weapon panel. */
static void refresh_local_armory(int stopping, DWORD now)
{
    if (local_player && !stopping && (local_player != armory_player || world_epoch != armory_epoch))
    {
        armory_player = local_player;
        armory_epoch = world_epoch;
        actor_grant_all_weapons(&engine, local_player);
    }
    /* The native weapon panel is built once per level; keep granted icons visible. */
    if (local_player && !stopping && (int32_t)(now - icons_at) >= 0)
    {
        ui_show_weapon_icons(local_game);
        icons_at = now + WEAPON_ICONS_REFRESH_MS;
    }
    if (!local_player)
        armory_player = 0;
}

/* Drop captures from an old map before the worker can send them; the rest
   wait in the staging array until the outgoing lock is free. */
static void flush_local_shots(int gameplay, int stopping)
{
    if (!gameplay || stopping)
        staged_count = 0;
    else
    {
        unsigned int kept = 0;
        for (unsigned int i = 0; i < staged_count; ++i)
        {
            if (staged[i].world_epoch == world_epoch)
                staged[kept++] = staged[i];
        }
        staged_count = kept;
    }

    if (!TryAcquireSRWLockExclusive(&outgoing_lock))
        return;
    int queue_from_old_world = outgoing_count && outgoing[outgoing_head].world_epoch != world_epoch;
    if (!gameplay || stopping || queue_from_old_world)
    {
        outgoing_count = 0;
        outgoing_head = 0;
    }
    unsigned int moved = 0;
    while (moved < staged_count && outgoing_count < SHOT_QUEUE_CAPACITY)
    {
        outgoing[(outgoing_head + outgoing_count++) % SHOT_QUEUE_CAPACITY] = staged[moved++];
    }
    ReleaseSRWLockExclusive(&outgoing_lock);
    if (moved)
    {
        memmove(staged, staged + moved, (staged_count - moved) * sizeof(staged[0]));
        staged_count -= moved;
    }
}

/* Takes the latest peer states and moves received shots to the pending queue.
   Skipped this tick when the worker holds the lock. */
static void take_peer_handoff(MultiplayerFrame* result)
{
    if (!TryAcquireSRWLockExclusive(&incoming_lock))
        return;
    memcpy(current, incoming, sizeof(current));
    current_at = published_at;
    for (unsigned int i = 0; i < received_count; ++i)
    {
        if (pending_count < SHOT_QUEUE_CAPACITY)
            pending[pending_count++] = received[i];
        else
            ++result->shots_discarded;
    }
    received_count = 0;
    ReleaseSRWLockExclusive(&incoming_lock);
}

/* True when the replica was made for another map or local world epoch. */
static int remote_world_changed(const Remote* r)
{
    if (r->world.low != local_low || r->world.high != local_high)
        return 1;
    return r->world.local_epoch != world_epoch;
}

/* True when this peer should have a replica in the local world now. */
static int remote_wanted(const LocalTick* tick, const PeerState* peer)
{
    const MpState* s = &peer->state;
    if (!tick->gameplay || tick->stopping)
        return 0;
    if (!peer->present || tick->now - current_at > PEER_STATE_TIMEOUT_MS)
        return 0;
    if (!s->active || s->health <= 0)
        return 0;
    if (!s->world_epoch)
        return 0;
    return s->world_low == tick->local->world_low && s->world_high == tick->local->world_high;
}

/* Removes the replica when the peer left, changed session or world. */
static void remove_unwanted(Remote* r, const LocalTick* tick, RemoteTick* rt)
{
    rt->update = actor_remove(&r->actor, tick->local->game);
    if (rt->update.event == ACTOR_FAULT)
    {
        abandon(r, tick->local->game, &rt->update, &rt->cleanup_failed);
        return;
    }
    r->state = RS_IDLE;
    r->retry.failures = 0;
    forget_weapon(r);
}

/* Moves an idle peer, or one whose retry delay elapsed, to waiting. */
static void enter_waiting(Remote* r, const LocalTick* tick, const PeerState* peer)
{
    DWORD now = tick->now;
    if (r->state == RS_IDLE)
    {
        r->session = peer->session;
        r->epoch = peer->state.world_epoch;
        r->world.low = local_low;
        r->world.high = local_high;
        r->world.local_epoch = world_epoch;
        r->entered_at = now;
        r->state = RS_WAITING;
    }
    else if (r->state == RS_BACKOFF && (int32_t)(now - r->retry.at) >= 0)
    {
        r->state = RS_WAITING;
        r->entered_at = now - PEER_SPAWN_DELAY_MS;
    }
}

/* Creates the replica through the native factory. */
static void spawn_remote(Remote* r, const LocalTick* tick, RemoteTick* rt)
{
    uintptr_t game = tick->local->game;
    rt->update = actor_spawn(&r->actor, base, tick->local, &rt->target);
    if (rt->update.event == ACTOR_SPAWNED)
    {
        r->state = RS_SPAWNING;
        r->entered_at = tick->now;
        r->last_pose = tick->now;
        return;
    }
    if (rt->update.event == ACTOR_FAULT)
    {
        abandon(r, game, &rt->update, &rt->cleanup_failed);
        return;
    }

    ActorResult cleanup = actor_remove(&r->actor, game);
    if (cleanup.event == ACTOR_FAULT)
        abandon(r, game, &rt->update, &rt->cleanup_failed);
    else
        schedule_retry(r, tick->now);
}

/* Waits for the native torso, then joins the local player's army. A torso
   that never appears leads to removal and a retry. */
static void wait_for_torso(Remote* r, const LocalTick* tick, RemoteTick* rt)
{
    uintptr_t game = tick->local->game;
    int torso = actor_torso_ready(&r->actor);
    if (torso < 0)
        abandon(r, game, &rt->update, &rt->cleanup_failed);
    else if (torso)
    {
        if (actor_set_army(&r->actor, tick->local->army_index) != 1)
            abandon(r, game, &rt->update, &rt->cleanup_failed);
        else
        {
            r->state = RS_SPAWNED;
            r->retry.failures = 0;
        }
    }
    else if (tick->now - r->entered_at >= TORSO_TIMEOUT_MS)
    {
        rt->update = actor_remove(&r->actor, game);
        if (rt->update.event == ACTOR_FAULT)
            abandon(r, game, &rt->update, &rt->cleanup_failed);
        else
            schedule_retry(r, tick->now);
    }
}

/* A rejected arm is retried at most once per second and reported once per slot. */
static void arm_remote(Remote* r, const LocalTick* tick, RemoteTick* rt)
{
    int slot = rt->target.weapon_slot;
    if (slot < 0)
    {
        r->weapon.last = slot;
        return;
    }
    if (slot == r->weapon.last)
        return;
    int retry_pending = (int32_t)(tick->now - r->weapon.retry_at) < 0;
    if (slot == r->weapon.rejected && retry_pending)
        return;

    int armed = actor_arm(&r->actor, slot);
    if (armed < 0)
        abandon(r, tick->local->game, &rt->update, &rt->cleanup_failed);
    else if (armed)
    {
        r->weapon.last = slot;
        r->weapon.rejected = -1;
    }
    else
    {
        if (slot != r->weapon.rejected)
        {
            rt->update.event = ACTOR_REJECTED;
            rt->update.reason = ACTOR_REASON_WEAPON;
            rt->update.entity = r->actor.entity;
        }
        r->weapon.rejected = slot;
        r->weapon.retry_at = tick->now + WEAPON_RETRY_MS;
    }
}

/* Applies the interpolated pose, weapon, health and ammo to the replica. */
static void apply_remote_pose(Remote* r, const LocalTick* tick, RemoteTick* rt)
{
    interpolate_pose(r, &rt->peer->state, &rt->target, tick->now);
    ActorResult pose = actor_apply(&r->actor, &rt->target);
    if (pose.event == ACTOR_FAULT)
    {
        rt->update = pose;
        abandon(r, tick->local->game, &rt->update, &rt->cleanup_failed);
    }
    else if (pose.event == ACTOR_LOST)
    {
        rt->update = pose;
        r->state = RS_IDLE;
        forget_weapon(r);
    }
    else if (tick->now - r->last_pose >= POSE_REPORT_MS && rt->update.event == ACTOR_NONE)
    {
        rt->update = pose;
        r->last_pose = tick->now;
    }
}

/* Drives a wanted peer through waiting, spawning and spawned. */
static void tick_wanted_remote(Remote* r, const LocalTick* tick, RemoteTick* rt)
{
    int has_entity = r->state == RS_SPAWNING || r->state == RS_SPAWNED;
    if (has_entity && !actor_live(&r->actor, tick->local->game))
    {
        rt->update.event = ACTOR_LOST;
        rt->update.entity = r->actor.entity;
        r->actor.entity = 0;
        r->state = RS_IDLE;
        forget_weapon(r);
    }
    enter_waiting(r, tick, rt->peer);

    int waited = tick->now - r->entered_at >= PEER_SPAWN_DELAY_MS;
    int local_ready = tick->probe == PROBE_OK && tick->local->player;
    if (r->state == RS_WAITING && waited && local_ready)
        spawn_remote(r, tick, rt);
    else if (r->state == RS_SPAWNING)
        wait_for_torso(r, tick, rt);

    if (r->state == RS_SPAWNED)
        arm_remote(r, tick, rt);
    if (r->state == RS_SPAWNED)
        apply_remote_pose(r, tick, rt);
}

/* One peer's replica for one tick. Returns 1 while its entity is live. */
static int tick_remote(unsigned int id, const LocalTick* tick, MultiplayerFrame* result)
{
    Remote* r = &remote[id];
    uintptr_t game = tick->local->game;
    RemoteTick rt = {0};
    rt.peer = &current[id];
    int world_changed = remote_world_changed(r);
    if (r->state == RS_ABANDONED && world_changed && !actor_live(&r->actor, game))
    {
        actor_init(&r->actor, &engine);
        r->state = RS_IDLE;
        r->retry.failures = 0;
    }

    int wanted = remote_wanted(tick, rt.peer);
    int changed = r->session != rt.peer->session || r->epoch != rt.peer->state.world_epoch;
    target_snapshot(&rt.target, &rt.peer->state);
    if (r->state != RS_ABANDONED && (!wanted || changed || world_changed))
        remove_unwanted(r, tick, &rt);

    int live = 0;
    if (r->state == RS_ABANDONED)
    {
        if (tick->stopping && actor_live(&r->actor, game))
        {
            live = 1;
            rt.cleanup_failed = 1;
            rt.update.event = ACTOR_FAULT;
            rt.update.reason = ACTOR_REASON_EXCEPTION;
            rt.update.entity = r->actor.entity;
        }
    }
    else if (wanted)
        tick_wanted_remote(r, tick, &rt);

    if (r->state != RS_SPAWNED)
        pose_reset(&r->pose);
    if (rt.update.event != ACTOR_NONE)
        report(result, id, r, &rt.update, rt.cleanup_failed);
    if (actor_live(&r->actor, game))
        live = 1;
    return live;
}

/* True when a pending shot no longer belongs to a live, current peer. */
static int shot_is_stale(const PendingShot* p, const LocalTick* tick)
{
    const ShotEvent* e = &p->event;
    const Remote* r = &remote[e->id];
    const MpState* s = &current[e->id].state;
    if (tick->stopping || !tick->gameplay)
        return 1;
    if (tick->now - p->received > SHOT_MAX_AGE_MS)
        return 1;
    if (r->state == RS_ABANDONED || e->session != r->session)
        return 1;
    if (e->shot.world_epoch != r->epoch)
        return 1;
    if (e->shot.world_low != tick->local->world_low ||
        e->shot.world_high != tick->local->world_high)
        return 1;
    return !current[e->id].present || !s->active || s->health <= 0;
}

/* Replays at most one pending shot per peer through the native attack action.
   Shots for a peer that is not spawned yet stay pending. */
static void replay_pending_shots(const LocalTick* tick, MultiplayerFrame* result)
{
    unsigned int keep = 0;
    unsigned int fired = 0;
    for (unsigned int i = 0; i < pending_count; ++i)
    {
        PendingShot p = pending[i];
        ShotEvent* e = &p.event;
        Remote* r = &remote[e->id];
        if (shot_is_stale(&p, tick))
        {
            ++result->shots_discarded;
            continue;
        }
        if (!(fired & (1u << e->id)) && r->state == RS_SPAWNED)
        {
            int applied = actor_shoot(&r->actor, tick->local->game, e->shot.x, e->shot.y,
                                      (int)e->shot.weapon);
            if (applied)
            {
                fired |= 1u << e->id;
                if (applied > 0)
                    ++result->shots_applied;
                else
                {
                    ActorResult fault = {0};
                    int failed = 0;
                    fault.entity = r->actor.entity;
                    abandon(r, tick->local->game, &fault, &failed);
                    report(result, e->id, r, &fault, failed);
                    ++result->shots_discarded;
                }
                continue;
            }
        }
        pending[keep++] = p;
    }
    pending_count = keep;
}

/* One game-thread tick: updates the local world, flushes local shots, takes the peer
   handoff, drives every peer's replica and replays pending shots. While stopping it removes
   the replicas and sets `stopped` once none is left. */
MultiplayerFrame multiplayer_tick(const Snapshot* local, enum ProbeResult state, DWORD now)
{
    MultiplayerFrame result = {0};
    if (!enabled || InterlockedCompareExchange(&stopped, 0, 0))
        return result;
    LONG thread = (LONG)GetCurrentThreadId();
    LONG owner = InterlockedCompareExchange(&game_thread, thread, 0);
    if (owner && owner != thread)
        return result;

    LocalTick tick = {local, state, now, 0, 0};
    tick.stopping = InterlockedCompareExchange(&stop_requested, 0, 0) != 0;
    tick.gameplay = update_local_world(local, state);
    refresh_local_armory(tick.stopping, now);
    flush_local_shots(tick.gameplay, tick.stopping);
    result.world_epoch = world_epoch;
    take_peer_handoff(&result);

    int any_live = 0;
    for (unsigned int id = 0; id < MP_MAX_PEERS; ++id)
    {
        if (tick_remote(id, &tick, &result))
            any_live = 1;
    }
    replay_pending_shots(&tick, &result);
    result.shots_discarded += (unsigned int)InterlockedExchange(&event_drops, 0);
    if (tick.stopping && !any_live)
        InterlockedExchange(&stopped, 1);
    return result;
}

/* Action hook filter: true if `entity` is one of our live replicas (game thread only). */
int multiplayer_is_replica(uintptr_t entity)
{
    if (GetCurrentThreadId() != (DWORD)InterlockedCompareExchange(&game_thread, 0, 0))
        return 0;
    if (!enabled || !local_game)
        return 0;
    for (unsigned int i = 0; i < MP_MAX_PEERS; ++i)
        if (remote[i].actor.entity == entity && actor_live(&remote[i].actor, local_game))
            return 1;
    return 0;
}

/* EndScene hook: draws health bars and names of spawned replicas (render thread, ASCII). */
void multiplayer_draw(void)
{
    if (GetCurrentThreadId() != (DWORD)InterlockedCompareExchange(&game_thread, 0, 0))
        return;
    if (!enabled || !local_game || InterlockedCompareExchange(&stop_requested, 0, 0))
        return;
    __try
    {
        if (*(uintptr_t*)(base + STEAM_GAME_PTR_RVA) != local_game)
            return;
        for (unsigned int i = 0; i < MP_MAX_PEERS; ++i)
            if (remote[i].state == RS_SPAWNED && actor_live(&remote[i].actor, local_game))
            {
                ui_health_bar(local_game, remote[i].actor.entity, current[i].state.health,
                              current[i].state.max_health);
                char label[sizeof(current[i].name) + 1];
                memcpy(label, current[i].name, sizeof(current[i].name));
                label[sizeof(current[i].name)] = 0;
                ui_name_label(local_game, remote[i].actor.entity, label);
            }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

/* Asks the next game tick to remove all replicas; before the first tick it stops at once. */
void multiplayer_request_stop(void)
{
    InterlockedExchange(&stop_requested, 1);
    /* Startup rollback before the first tick cannot own any native entity. */
    if (!InterlockedCompareExchange(&game_thread, 0, 0))
        InterlockedExchange(&stopped, 1);
}

/* True once every replica is gone and the game thread acknowledged. */
int multiplayer_stopped(void)
{
    return InterlockedCompareExchange(&stopped, 0, 0) != 0;
}

/* Clears the module state; refuses until multiplayer_stopped. */
int multiplayer_stop(void)
{
    if (!multiplayer_stopped())
        return 0;
    enabled = 0;
    base = local_game = local_player = 0;
    InterlockedExchange(&game_thread, 0);
    AcquireSRWLockExclusive(&incoming_lock);
    memset(incoming, 0, sizeof(incoming));
    memset(current, 0, sizeof(current));
    received_count = pending_count = 0;
    published_at = current_at = 0;
    ReleaseSRWLockExclusive(&incoming_lock);
    AcquireSRWLockExclusive(&outgoing_lock);
    outgoing_count = outgoing_head = 0;
    ReleaseSRWLockExclusive(&outgoing_lock);
    staged_count = 0;
    return 1;
}

/* State of peer `id`, for logs and tests. */
enum RemoteState multiplayer_remote_state(unsigned int id)
{
    return id < MP_MAX_PEERS ? remote[id].state : RS_IDLE;
}
