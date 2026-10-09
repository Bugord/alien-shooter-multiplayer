#include <string.h>
#include "monster_sync.h"

#define MONSTER_EVENT_CAPACITY 64u
typedef struct ReceivedMonsterEvent { MonsterEvent message; DWORD received_at; } ReceivedMonsterEvent;
static struct {
    enum MonsterRole role;
    enum MonsterSyncState state;
    volatile LONG stopping, stopped, incoming_dropped;
    uintptr_t game;
    uint32_t local_epoch, low, high, host_epoch, authority_id, authority_session;
    uint32_t sequence, event_sequence, last_event, seen_attacks, seen_deaths;
    DWORD settled_at, retry_at, retry_delay, logged_at;
    unsigned int attacks, deaths, dropped, rejected;
    MpMonsterEvent staged[MONSTER_EVENT_CAPACITY];
    unsigned int staged_count;
    int world, host_captured, remote_dead;
} monsters;
static struct {
    MonsterState incoming;
    DWORD incoming_at;
    int local_authority;
    ReceivedMonsterEvent received[MONSTER_EVENT_CAPACITY];
    unsigned int received_count;
    MpMonsterState outgoing;
    int has_outgoing;
    MpMonsterEvent events[MONSTER_EVENT_CAPACITY];
    unsigned int head, count;
} handoff;
static SRWLOCK lock = SRWLOCK_INIT;

int monster_sync_initialize(uintptr_t base, enum MonsterRole role, const UnitPuppetEngine* api, ActorCreate create) {
    if (monsters.state != MS_OFF || (role != MONSTER_HOST && role != MONSTER_CLIENT) || !api) return 0;
    memset(&monsters, 0, sizeof(monsters)); memset(&handoff, 0, sizeof(handoff));
    monsters.role = role; monsters.state = MS_STOPPING; monsters.stopped = 1;
    if (!unit_puppet_install(base, api)) return 0;
    if (role == MONSTER_CLIENT && !unit_combat_init(base, create, api->destroy, api->animation)) return 0;
    monsters.state = MS_WAITING; monsters.retry_delay = 2000u; return 1;
}
int monster_sync_enable(uintptr_t base, enum MonsterRole role) {
    UnitPuppetEngine api; ActorCreate create;
    if (!unit_puppet_bind(base, &api) || !unit_combat_bind(base, &create)) return 0;
    return monster_sync_initialize(base, role, &api, create);
}
void monster_sync_publish(const MonsterState* source, int local_authority, DWORD now) {
    AcquireSRWLockExclusive(&lock);
    if (source) handoff.incoming = *source; else memset(&handoff.incoming, 0, sizeof(handoff.incoming));
    handoff.incoming_at = now; handoff.local_authority = local_authority;
    ReleaseSRWLockExclusive(&lock);
}
void monster_sync_receive(const MonsterEvent* event, DWORD now) {
    AcquireSRWLockExclusive(&lock);
    if (event && handoff.received_count < MONSTER_EVENT_CAPACITY) {
        ReceivedMonsterEvent* slot = &handoff.received[handoff.received_count++]; slot->message = *event; slot->received_at = now;
    } else if (event) InterlockedIncrement(&monsters.incoming_dropped);
    ReleaseSRWLockExclusive(&lock);
}
int monster_sync_take_state(MpMonsterState* out) {
    int available;
    AcquireSRWLockExclusive(&lock); available = handoff.has_outgoing;
    if (available) { *out = handoff.outgoing; handoff.has_outgoing = 0; }
    ReleaseSRWLockExclusive(&lock); return available;
}
int monster_sync_take_event(MpMonsterEvent* out) {
    int available;
    AcquireSRWLockExclusive(&lock); available = handoff.count != 0;
    if (available) { *out = handoff.events[handoff.head]; handoff.head = (handoff.head + 1u) % MONSTER_EVENT_CAPACITY; --handoff.count; }
    ReleaseSRWLockExclusive(&lock); return available;
}
static void stage(unsigned int kind, uint32_t tick) {
    MpMonsterEvent event = {0}; event.sequence = ++monsters.event_sequence;
    if (!event.sequence) event.sequence = ++monsters.event_sequence;
    event.world_low = monsters.low; event.world_high = monsters.high; event.world_epoch = monsters.local_epoch;
    event.monster_id = 1; event.tick = tick; event.kind = kind;
    if (monsters.staged_count < MONSTER_EVENT_CAPACITY) monsters.staged[monsters.staged_count++] = event;
    else ++monsters.dropped;
}
static int cleanup(uintptr_t game) {
    if (!unit_puppet_release(game)) return 0;
    if (monsters.role == MONSTER_CLIENT && !unit_combat_remove(game)) return 0;
    return 1;
}
static void backoff(uintptr_t game, DWORD now) {
    if (!cleanup(game)) { monsters.state = MS_ABANDONED; return; }
    monsters.host_captured = 0; monsters.seen_attacks = monsters.seen_deaths = 0;
    monsters.state = MS_BACKOFF; monsters.retry_at = now + monsters.retry_delay;
    if (monsters.retry_delay < 30000u) {
        monsters.retry_delay *= 2u; if (monsters.retry_delay > 30000u) monsters.retry_delay = 30000u;
    }
}
static int same_world(const MpMonsterState* s) {
    return s->world_low == monsters.low && s->world_high == monsters.high && s->world_epoch && s->monster_id == 1u && s->vid == 50u;
}
MonsterSyncFrame monster_sync_tick(const Snapshot* local, enum ProbeResult result, uint32_t epoch, uint32_t tick, DWORD now) {
    MonsterSyncFrame frame = {0}; frame.role = monsters.role;
    if (monsters.state == MS_OFF) return frame;
    MonsterState incoming = {0}; ReceivedMonsterEvent received[MONSTER_EVENT_CAPACITY];
    unsigned int received_count = 0, considered = 0; int local_authority = 0;
    if (TryAcquireSRWLockExclusive(&lock)) {
        if (now - handoff.incoming_at <= 1000u) { incoming = handoff.incoming; local_authority = handoff.local_authority; }
        received_count = handoff.received_count;
        memcpy(received, handoff.received, received_count * sizeof(*received)); handoff.received_count = 0;
        ReleaseSRWLockExclusive(&lock);
    } else {
        /* Keep the last valid entity briefly; lock contention is not peer loss. */
        frame.state = monsters.state; return frame;
    }
    uintptr_t game = local ? local->game : 0;
    if (InterlockedCompareExchange(&monsters.stopping, 0, 0)) {
        monsters.state = MS_STOPPING; frame.cleanup = cleanup(game);
        if (frame.cleanup) InterlockedExchange(&monsters.stopped, 1);
        goto report;
    }
    int valid_world = local && (result == PROBE_OK || result == PROBE_NO_PLAYER || result == PROBE_NO_ARMY) && local->in_level && epoch;
    if (!valid_world || !monsters.world || monsters.local_epoch != epoch ||
        local->world_low != monsters.low || local->world_high != monsters.high) {
        if (!cleanup(game)) { monsters.state = MS_ABANDONED; goto report; }
        monsters.world = 0; monsters.host_captured = 0; monsters.staged_count = 0;
        monsters.seen_attacks = monsters.seen_deaths = monsters.event_sequence = 0;
        monsters.last_event = monsters.host_epoch = monsters.authority_session = 0;
        monsters.remote_dead = 0;
        monsters.state = MS_WAITING; monsters.retry_delay = 2000u;
        if (!valid_world) goto report;
        monsters.game = game; monsters.local_epoch = epoch; monsters.low = local->world_low; monsters.high = local->world_high;
        monsters.settled_at = now; monsters.world = 1;
    }
    if (monsters.state == MS_ABANDONED) goto report;
    if (monsters.state == MS_BACKOFF && (int32_t)(now - monsters.retry_at) < 0) goto report;
    if (monsters.state == MS_BACKOFF) monsters.state = MS_WAITING;
    if (now - monsters.settled_at < 2000u) goto report;
    if (monsters.role == MONSTER_HOST) {
        if (!local_authority) {
            if (!cleanup(game)) monsters.state = MS_ABANDONED;
            else { monsters.state = MS_WAITING; monsters.host_captured = 0; monsters.seen_attacks = monsters.seen_deaths = 0; }
            goto report;
        }
        if (!monsters.host_captured) {
            if (!local->player) goto report;
            int attached = unit_puppet_watch_first(game, local->player);
            if (attached < 0) { backoff(game, now); goto report; }
            if (!attached) goto report;
            monsters.host_captured = 1; monsters.state = MS_ACTIVE;
            InterlockedExchange(&monsters.stopped, 0);
        }
        frame.unit = unit_puppet_sample(game);
        if (frame.unit.state == UNIT_PUPPET_FAULT) { backoff(game, now); goto report; }
        /* Counters come from native action callbacks, not animation snapshots. */
        unsigned int count = frame.unit.captured_attacks - monsters.seen_attacks;
        if (count > MONSTER_EVENT_CAPACITY) { monsters.dropped += count - MONSTER_EVENT_CAPACITY; count = MONSTER_EVENT_CAPACITY; }
        for (unsigned int i = 0; i < count; ++i) { stage(MP_MONSTER_ATTACK, tick); ++monsters.attacks; }
        monsters.seen_attacks = frame.unit.captured_attacks;
        if (frame.unit.captured_deaths != monsters.seen_deaths || (!frame.unit.entity && !monsters.seen_deaths)) {
            stage(MP_MONSTER_DEATH, tick); ++monsters.deaths;
            monsters.seen_deaths = frame.unit.captured_deaths ? frame.unit.captured_deaths : 1u;
        }
        MpMonsterState output = {0}; output.sequence = ++monsters.sequence;
        output.world_low = monsters.low; output.world_high = monsters.high; output.world_epoch = epoch;
        output.monster_id = 1; output.vid = 50; output.tick = tick;
        output.alive = frame.unit.entity && frame.unit.animation < STEAM_ANIM_DEATH && frame.unit.health > 0;
        output.health = output.alive ? frame.unit.health : 0;
        output.x = frame.unit.x; output.y = frame.unit.y; output.z = frame.unit.z;
        output.velocity = frame.unit.velocity; output.moving = frame.unit.moving; output.direction = frame.unit.direction;
        output.event_sequence = monsters.event_sequence;
        if (TryAcquireSRWLockExclusive(&lock)) {
            handoff.outgoing = output; handoff.has_outgoing = 1;
            unsigned int sent = 0;
            while (sent < monsters.staged_count && handoff.count < MONSTER_EVENT_CAPACITY) {
                handoff.events[(handoff.head + handoff.count++) % MONSTER_EVENT_CAPACITY] = monsters.staged[sent++];
            }
            if (sent) { monsters.staged_count -= sent; memmove(monsters.staged, monsters.staged + sent, monsters.staged_count * sizeof(*monsters.staged)); }
            ReleaseSRWLockExclusive(&lock);
        }
    } else {
        const MpMonsterState* source = &incoming.state;
        if (!incoming.present || !same_world(source)) {
            if (!cleanup(game)) monsters.state = MS_ABANDONED; else monsters.state = MS_WAITING;
            goto report;
        }
        if (monsters.host_epoch != source->world_epoch || monsters.authority_id != incoming.authority_id ||
            monsters.authority_session != incoming.authority_session) {
            if (!cleanup(game)) { monsters.state = MS_ABANDONED; goto report; }
            monsters.host_epoch = source->world_epoch; monsters.authority_id = incoming.authority_id;
            monsters.authority_session = incoming.authority_session; monsters.state = MS_WAITING;
            monsters.remote_dead = 0;
        }
        if (!source->alive) {
            monsters.remote_dead = 1;
            if (monsters.state == MS_ACTIVE) ++monsters.deaths;
            if (!cleanup(game)) monsters.state = MS_ABANDONED; else monsters.state = MS_WAITING;
            goto report;
        }
        if (monsters.remote_dead) goto report;
        if (monsters.state != MS_ACTIVE) {
            Snapshot spawn = *local; spawn.health = 1; /* World presence is separate from local life. */
            InterlockedExchange(&monsters.stopped, 0);
            if (unit_combat_spawn(&spawn) != 1) { backoff(game, now); goto report; }
            UnitCombatSample fixture = unit_combat_sample(game, 1);
            if (fixture.fault || !fixture.sources_unchanged || unit_puppet_attach_entity(game, local->player, fixture.root) != 1) {
                backoff(game, now); goto report;
            }
            monsters.last_event = source->event_sequence; monsters.state = MS_ACTIVE; monsters.retry_delay = 2000u;
        }
        UnitPuppetPose pose = {source->x, source->y, source->z, source->velocity, source->direction, source->moving};
        if (unit_puppet_apply(game, &pose) != 1 || unit_puppet_health(game, source->health) != 1) { backoff(game, now); goto report; }
        for (unsigned int i = 0; i < received_count; ++i) {
            ++considered;
            const MonsterEvent* event = &received[i].message; const MpMonsterEvent* e = &event->event;
            if (now - received[i].received_at > 250u || event->authority_id != monsters.authority_id ||
                event->authority_session != monsters.authority_session || e->world_epoch != monsters.host_epoch ||
                e->world_low != monsters.low || e->world_high != monsters.high || e->monster_id != 1u ||
                !mp_sequence_newer(e->sequence, monsters.last_event)) { ++monsters.rejected; continue; }
            monsters.last_event = e->sequence;
            if (e->kind == MP_MONSTER_DEATH) {
                monsters.remote_dead = 1;
                ++monsters.deaths;
                if (!cleanup(game)) monsters.state = MS_ABANDONED; else monsters.state = MS_WAITING;
                break;
            }
            if (e->kind != MP_MONSTER_ATTACK) { ++monsters.rejected; continue; }
            if (unit_puppet_replay(game, e->sequence, STEAM_ANIM_ATTACK) != 1) { backoff(game, now); break; }
            ++monsters.attacks;
        }
        frame.unit = unit_puppet_sample(game);
        if (monsters.state == MS_ACTIVE && frame.unit.state != UNIT_PUPPET_ACTIVE) backoff(game, now);
    }
report:
    monsters.rejected += received_count - considered;
    frame.state = monsters.state; frame.monster_id = monsters.host_captured || monsters.state == MS_ACTIVE ? 1u : 0u;
    frame.host_epoch = monsters.role == MONSTER_HOST ? monsters.local_epoch : monsters.host_epoch;
    frame.attacks = monsters.attacks; frame.deaths = monsters.deaths;
    frame.dropped = monsters.dropped + (unsigned int)InterlockedCompareExchange(&monsters.incoming_dropped, 0, 0);
    frame.rejected = monsters.rejected;
    if (now - monsters.logged_at >= 500u || frame.cleanup) { frame.event = 1; monsters.logged_at = now; }
    return frame;
}
void monster_sync_request_stop(void) { InterlockedExchange(&monsters.stopping, 1); }
int monster_sync_stopped(void) { return monsters.state == MS_OFF || InterlockedCompareExchange(&monsters.stopped, 0, 0); }
int monster_sync_stop(void) {
    if (monsters.state == MS_OFF) return 1;
    if (!monster_sync_stopped()) return 0;
    int fixture = monsters.role != MONSTER_CLIENT || unit_combat_stop();
    if (!fixture || !unit_puppet_stop()) return 0;
    AcquireSRWLockExclusive(&lock); memset(&handoff, 0, sizeof(handoff)); ReleaseSRWLockExclusive(&lock);
    monsters.state = MS_OFF; monsters.role = MONSTER_DISABLED; return 1;
}
