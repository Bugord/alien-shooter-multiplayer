#include <string.h>
#include "monster_sync.h"

#define MONSTER_EVENT_CAPACITY 64u
#define MONSTER_PREDICTION_MS  750u
enum PredictionState
{
    PREDICTION_NONE,
    PREDICTION_HEALTH,
    PREDICTION_FORM,
    PREDICTION_DEATH
};
typedef struct ReceivedMonsterEvent
{
    MonsterEvent message;
    DWORD received_at;
} ReceivedMonsterEvent;
typedef struct MonsterSlot
{
    enum MonsterRole role;
    enum MonsterSyncState state;
    volatile LONG stopping, stopped;
    uintptr_t game;
    uint32_t local_epoch, low, high, host_epoch, authority_id, authority_session;
    uint32_t sequence, event_sequence, last_event, seen_attacks, seen_deaths;
    DWORD settled_at, retry_at, retry_delay, logged_at, death_at;
    unsigned int attacks, deaths, dropped, rejected;
    MpMonsterEvent staged[MONSTER_EVENT_CAPACITY];
    unsigned int staged_count;
    int world, host_captured, remote_dead, death_presented;
    UnitPuppetPose host_pose;
    unsigned int death_animation;
    uint32_t vid, form_revision, form_phase, form_from;
    unsigned int forms;
    DWORD armor_at;
    uint32_t armor_presented;
    enum PredictionState prediction;
    DWORD predicted_at;
    uint32_t predicted_sequence, predicted_vid, presentation_vid, predicted_hits;
    int predicted_health;
    unsigned int predicted_animation, predictions, confirmations, corrections;
} MonsterSlot;
static MonsterSlot slots[MP_MONSTER_CAPACITY];
static unsigned int selected_slot, slot_count = 1u;
static int gallery;
#define monsters slots[selected_slot]
static struct
{
    MonsterState incoming[MP_MONSTER_CAPACITY];
    DWORD incoming_at[MP_MONSTER_CAPACITY];
    int local_authority;
    ReceivedMonsterEvent received[MONSTER_EVENT_CAPACITY];
    unsigned int received_count;
    MpMonsterState outgoing[MP_MONSTER_CAPACITY];
    int has_outgoing[MP_MONSTER_CAPACITY];
    unsigned int next_outgoing;
    MpMonsterEvent events[MONSTER_EVENT_CAPACITY];
    unsigned int head, count;
} handoff;
static SRWLOCK lock = SRWLOCK_INIT;
static volatile LONG incoming_dropped;

int monster_sync_initialize(uintptr_t base, enum MonsterRole role, const UnitPuppetEngine* api,
                            ActorCreate create)
{
    if (monsters.state != MS_OFF || (role != MONSTER_HOST && role != MONSTER_CLIENT) || !api)
        return 0;
    memset(slots, 0, sizeof(slots));
    memset(&handoff, 0, sizeof(handoff));
    selected_slot = 0;
    slot_count = 1u;
    gallery = 0;
    incoming_dropped = 0;
    monsters.role = role;
    monsters.state = MS_STOPPING;
    monsters.stopped = 1;
    if (!unit_puppet_install(base, api))
        return 0;
    if (!unit_combat_init(base, create, api->destroy, api->animation))
        return 0;
    unit_puppet_set_form_observer(unit_combat_observe_entity);
    if (!unit_death_init(base, create))
        return 0;
    for (unsigned int i = 0; i < MP_MONSTER_CAPACITY; ++i)
    {
        slots[i].role = role;
        slots[i].state = MS_WAITING;
        slots[i].retry_delay = 2000u;
        slots[i].stopped = 1;
    }
    return 1;
}
int monster_sync_enable(uintptr_t base, enum MonsterRole role)
{
    UnitPuppetEngine api;
    ActorCreate create;
    if (!unit_puppet_bind(base, &api) || !unit_combat_bind(base, &create))
        return 0;
    return monster_sync_initialize(base, role, &api, create);
}
int monster_sync_gallery(int enabled)
{
    if (slots[0].state != MS_WAITING || slots[0].world)
        return 0;
    gallery = enabled != 0;
    slot_count = gallery ? 48u : 1u;
    return 1;
}
static unsigned int expected_vid(unsigned int slot)
{
    static const unsigned int types[] = {40u, 50u, 60u, 80u, 70u, 90u};
    return gallery ? types[slot / 8u] : 50u;
}
void monster_sync_publish_at(unsigned int slot, const MonsterState* source, int local_authority,
                             DWORD now)
{
    if (slot >= MP_MONSTER_CAPACITY)
        return;
    AcquireSRWLockExclusive(&lock);
    if (source)
        handoff.incoming[slot] = *source;
    else
        memset(&handoff.incoming[slot], 0, sizeof(handoff.incoming[slot]));
    handoff.incoming_at[slot] = now;
    handoff.local_authority = local_authority;
    ReleaseSRWLockExclusive(&lock);
}
void monster_sync_publish(const MonsterState* source, int local_authority, DWORD now)
{
    monster_sync_publish_at(0u, source, local_authority, now);
}
void monster_sync_receive(const MonsterEvent* event, DWORD now)
{
    AcquireSRWLockExclusive(&lock);
    if (event && handoff.received_count < MONSTER_EVENT_CAPACITY)
    {
        ReceivedMonsterEvent* slot = &handoff.received[handoff.received_count++];
        slot->message = *event;
        slot->received_at = now;
    }
    else if (event)
        InterlockedIncrement(&incoming_dropped);
    ReleaseSRWLockExclusive(&lock);
}
int monster_sync_take_state(MpMonsterState* out)
{
    int available = 0;
    AcquireSRWLockExclusive(&lock);
    for (unsigned int i = 0; i < slot_count; ++i)
    {
        unsigned int slot = handoff.next_outgoing++ % slot_count;
        if (handoff.has_outgoing[slot])
        {
            *out = handoff.outgoing[slot];
            handoff.has_outgoing[slot] = 0;
            available = 1;
            break;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return available;
}
int monster_sync_take_event(MpMonsterEvent* out)
{
    int available;
    AcquireSRWLockExclusive(&lock);
    available = handoff.count != 0;
    if (available)
    {
        *out = handoff.events[handoff.head];
        handoff.head = (handoff.head + 1u) % MONSTER_EVENT_CAPACITY;
        --handoff.count;
    }
    ReleaseSRWLockExclusive(&lock);
    return available;
}
static void stage(unsigned int kind, uint32_t tick, const UnitPuppetPose* pose)
{
    MpMonsterEvent event = {0};
    event.sequence = ++monsters.event_sequence;
    if (!event.sequence)
        event.sequence = ++monsters.event_sequence;
    event.world_low = monsters.low;
    event.world_high = monsters.high;
    event.world_epoch = monsters.local_epoch;
    event.monster_id = selected_slot + 1u;
    event.tick = tick;
    event.kind = kind;
    event.x = pose->x;
    event.y = pose->y;
    event.z = pose->z;
    event.direction = pose->direction;
    event.death_animation = kind == MP_MONSTER_DEATH ? monsters.death_animation : 0;
    event.vid = monsters.vid;
    event.origin_vid = expected_vid(selected_slot);
    event.form_revision = monsters.form_revision;
    event.form_phase = monsters.form_phase;
    event.from_vid = kind == MP_MONSTER_FORM ? monsters.form_from : 0u;
    if (kind == MP_MONSTER_FORM && monsters.form_phase == MP_MONSTER_FORM_ARMOR)
        event.death_animation = STEAM_ANIM_DEATH;
    if (monsters.staged_count < MONSTER_EVENT_CAPACITY)
        monsters.staged[monsters.staged_count++] = event;
    else
        ++monsters.dropped;
}
static int cleanup(uintptr_t game)
{
    if (!unit_puppet_release(game))
        return 0;
    if ((monsters.role == MONSTER_CLIENT || gallery) && !unit_combat_remove(game))
        return 0;
    {
        if (!unit_death_remove(game))
            return 0;
        unit_death_select(selected_slot + MP_MONSTER_CAPACITY);
        int armor = unit_death_remove(game);
        unit_death_select(selected_slot);
        if (!armor)
            return 0;
    }
    monsters.presentation_vid = monsters.predicted_hits = monsters.armor_presented = 0;
    monsters.prediction = PREDICTION_NONE;
    return 1;
}
static void backoff(uintptr_t game, DWORD now)
{
    if (!cleanup(game))
    {
        monsters.state = MS_ABANDONED;
        return;
    }
    monsters.host_captured = 0;
    monsters.seen_attacks = monsters.seen_deaths = 0;
    monsters.armor_presented = 0;
    monsters.state = MS_BACKOFF;
    monsters.retry_at = now + monsters.retry_delay;
    if (monsters.retry_delay < 30000u)
    {
        monsters.retry_delay *= 2u;
        if (monsters.retry_delay > 30000u)
            monsters.retry_delay = 30000u;
    }
}
static int same_world(const MpMonsterState* s)
{
    return s->world_low == monsters.low && s->world_high == monsters.high && s->world_epoch &&
           s->monster_id == selected_slot + 1u && s->origin_vid == expected_vid(selected_slot) &&
           mp_monster_form_valid(s->vid, s->origin_vid, s->form_revision, s->form_phase);
}
static int cleanup_live(uintptr_t game)
{
    int removed = unit_puppet_release(game) && unit_combat_remove(game);
    if (removed)
    {
        monsters.presentation_vid = 0;
        monsters.predicted_hits = 0;
    }
    else
        monsters.state = MS_ABANDONED;
    return removed;
}
static int present_armor(uintptr_t game, const UnitPuppetPose* pose, unsigned int source_vid,
                         uint32_t revision, DWORD now)
{
    /* Each supported origin loses armor once; pending and successor revisions
       describe the same effect, including an earlier local prediction. */
    if (monsters.armor_presented)
        return 1;
    unit_death_select(selected_slot + MP_MONSTER_CAPACITY);
    int removed = unit_death_remove(game);
    int played = removed ? unit_death_play_armor(game, pose, source_vid) : 0;
    unit_death_select(selected_slot);
    if (!removed)
    {
        monsters.state = MS_ABANDONED;
        return 0;
    }
    if (played != 1)
    {
        backoff(game, now);
        return 0;
    }
    monsters.armor_presented = revision;
    monsters.armor_at = now;
    return 1;
}
static void present_death(uintptr_t game, const UnitPuppetPose* pose, unsigned int animation,
                          DWORD now)
{
    if (!monsters.remote_dead)
        ++monsters.deaths;
    monsters.remote_dead = 1;
    monsters.death_animation = animation;
    monsters.host_pose = *pose;
    if (monsters.death_presented)
    {
        monsters.state = MS_DEAD;
        return;
    }
    /* Armor debris has its own bounded visual slot and may outlive this kill. */
    if (!cleanup_live(game) || !unit_death_remove(game))
    {
        monsters.state = MS_ABANDONED;
        return;
    }
    InterlockedExchange(&monsters.stopped, 0);
    if (unit_death_play_type(game, pose, animation, monsters.vid) != 1)
    {
        backoff(game, now);
        return;
    }
    monsters.death_presented = 1;
    monsters.death_at = now;
    monsters.state = MS_DEAD;
}
static int client_copy(const Snapshot* local, unsigned int vid, const UnitPuppetPose* pose,
                       int health, DWORD now)
{
    uintptr_t game = local->game;
    if (monsters.presentation_vid != vid)
    {
        if (!cleanup_live(game))
        {
            monsters.state = MS_ABANDONED;
            return 0;
        }
        Snapshot spawn = *local;
        spawn.health = 1;
        InterlockedExchange(&monsters.stopped, 0);
        if (unit_combat_spawn_predictive(&spawn, vid, pose) != 1)
        {
            backoff(game, now);
            return 0;
        }
        UnitCombatSample fixture = unit_combat_sample(game, 1);
        if (fixture.fault || !fixture.sources_unchanged ||
            unit_puppet_attach_entity(game, local->player, fixture.root) != 1 ||
            unit_puppet_prediction(game) != 1)
        {
            backoff(game, now);
            return 0;
        }
        monsters.presentation_vid = vid;
        monsters.predicted_hits = 0;
        monsters.retry_delay = 2000u;
    }
    monsters.state = MS_ACTIVE;
    if (unit_puppet_apply(game, pose) != 1 || (health > 0 && unit_puppet_health(game, health) != 1))
    {
        backoff(game, now);
        return 0;
    }
    return 1;
}
static void forget_prediction(void)
{
    monsters.prediction = PREDICTION_NONE;
    monsters.predicted_vid = 0;
}
static int correct_prediction(uintptr_t game)
{
    if (!cleanup(game))
    {
        monsters.state = MS_ABANDONED;
        return 0;
    }
    monsters.presentation_vid = monsters.predicted_hits = monsters.armor_presented = 0;
    monsters.death_presented = 0;
    monsters.state = MS_WAITING;
    forget_prediction();
    ++monsters.corrections;
    return 1;
}
static int predict_hit(uintptr_t game, const MpMonsterState* source, DWORD now)
{
    if (!monsters.presentation_vid || monsters.prediction == PREDICTION_DEATH)
        return 1;
    UnitPuppetSample sample = unit_puppet_sample(game);
    if (sample.state == UNIT_PUPPET_FAULT)
    {
        backoff(game, now);
        return 0;
    }
    if (sample.predicted_hits == monsters.predicted_hits)
        return 1;
    monsters.predicted_hits = sample.predicted_hits;
    if (monsters.prediction == PREDICTION_NONE)
    {
        monsters.predicted_at = now;
        monsters.predicted_sequence = source->sequence;
        monsters.prediction = PREDICTION_HEALTH;
        ++monsters.predictions;
    }
    monsters.predicted_vid = sample.vid ? sample.vid : monsters.presentation_vid;
    monsters.predicted_health = sample.health;
    if (sample.transitioning)
    {
        monsters.prediction = PREDICTION_FORM;
        monsters.predicted_vid = sample.vid == 70u ? 60u : 80u;
        monsters.predicted_health = 0; /* Native successor constructor supplies HP. */
    }
    else if (sample.captured_deaths)
    {
        monsters.prediction = PREDICTION_DEATH;
        monsters.predicted_animation = sample.death_animation;
        UnitPuppetPose pose = sample.death_pose;
        if (!cleanup_live(game) || !unit_death_remove(game))
        {
            monsters.state = MS_ABANDONED;
            return 0;
        }
        if (unit_death_play_type(game, &pose, sample.death_animation, monsters.predicted_vid) != 1)
        {
            backoff(game, now);
            return 0;
        }
        monsters.death_at = now;
        monsters.host_pose = pose;
        monsters.state = MS_PREDICTED_DEAD;
    }
    else if (sample.vid != monsters.presentation_vid)
    {
        /* Native freeze already changed our private descriptor atomically. */
        monsters.prediction = PREDICTION_FORM;
        monsters.presentation_vid = sample.vid;
    }
    else if (monsters.prediction == PREDICTION_FORM)
    {
        /* A later hit on the successor/frozen form needs its own HP result;
           merely receiving the form transition cannot acknowledge that hit. */
        monsters.prediction = PREDICTION_HEALTH;
    }
    return 1;
}
static MonsterSyncFrame tick_slot(const Snapshot* local, enum ProbeResult result, uint32_t epoch,
                                  uint32_t tick, DWORD now, const MonsterState* published,
                                  int local_authority, const ReceivedMonsterEvent* received,
                                  unsigned int received_count)
{
    MonsterSyncFrame frame = {0};
    frame.role = monsters.role;
    if (monsters.state == MS_OFF)
        return frame;
    MonsterState incoming = *published;
    unsigned int considered = 0;
    uintptr_t game = local ? local->game : 0;
    if (InterlockedCompareExchange(&monsters.stopping, 0, 0))
    {
        monsters.state = MS_STOPPING;
        frame.cleanup = cleanup(game);
        if (frame.cleanup)
            InterlockedExchange(&monsters.stopped, 1);
        goto report;
    }
    int valid_world =
        local && (result == PROBE_OK || result == PROBE_NO_PLAYER || result == PROBE_NO_ARMY) &&
        local->in_level && epoch;
    if (!valid_world || !monsters.world || monsters.local_epoch != epoch ||
        local->world_low != monsters.low || local->world_high != monsters.high)
    {
        if (!cleanup(game))
        {
            monsters.state = MS_ABANDONED;
            goto report;
        }
        monsters.world = 0;
        monsters.host_captured = 0;
        monsters.staged_count = 0;
        monsters.seen_attacks = monsters.seen_deaths = monsters.event_sequence = 0;
        monsters.last_event = monsters.host_epoch = monsters.authority_session = 0;
        monsters.remote_dead = monsters.death_presented = 0;
        monsters.vid = expected_vid(selected_slot);
        monsters.form_revision = 1u;
        monsters.form_phase = MP_MONSTER_FORM_ACTIVE;
        monsters.form_from = monsters.vid;
        monsters.armor_presented = 0;
        monsters.death_animation = 0;
        memset(&monsters.host_pose, 0, sizeof(monsters.host_pose));
        monsters.state = MS_WAITING;
        monsters.retry_delay = 2000u;
        if (!valid_world)
            goto report;
        monsters.game = game;
        monsters.local_epoch = epoch;
        monsters.low = local->world_low;
        monsters.high = local->world_high;
        monsters.settled_at = now;
        monsters.world = 1;
    }
    if (monsters.state == MS_ABANDONED)
        goto report;
    if (monsters.state == MS_BACKOFF && (int32_t)(now - monsters.retry_at) < 0)
        goto report;
    if (monsters.state == MS_BACKOFF)
        monsters.state = MS_WAITING;
    if (now - monsters.settled_at < 2000u)
        goto report;
    if (monsters.role == MONSTER_HOST)
    {
        if (!local_authority)
        {
            if (!cleanup(game))
                monsters.state = MS_ABANDONED;
            else
            {
                monsters.state = MS_WAITING;
                monsters.host_captured = 0;
                monsters.seen_attacks = monsters.seen_deaths = 0;
            }
            goto report;
        }
        if (!monsters.host_captured)
        {
            if (!local->player)
                goto report;
            int attached;
            if (gallery)
            {
                Snapshot spawn = *local;
                spawn.health = 1;
                UnitPuppetPose position = {local->x + 160.0f + (selected_slot % 8u) * 80.0f,
                                           local->y - 150.0f + (selected_slot / 8u) * 100.0f,
                                           local->z,
                                           0,
                                           128u,
                                           0};
                int created =
                    unit_combat_spawn_type(&spawn, expected_vid(selected_slot), &position, 1);
                if (created != 1)
                {
                    backoff(game, now);
                    goto report;
                }
                UnitCombatSample fixture = unit_combat_sample(game, 0);
                attached = fixture.fault
                               ? -1
                               : unit_puppet_watch_stationary(game, local->player, fixture.root);
            }
            else
                attached = unit_puppet_watch_first(game, local->player);
            if (attached < 0)
            {
                backoff(game, now);
                goto report;
            }
            if (!attached)
                goto report;
            monsters.host_captured = 1;
            monsters.state = MS_ACTIVE;
            InterlockedExchange(&monsters.stopped, 0);
        }
        frame.unit = unit_puppet_sample(game);
        if (gallery)
        {
            UnitCombatSample fixture = unit_combat_sample(game, 0);
            if (fixture.fault)
            {
                backoff(game, now);
                goto report;
            }
            if (!fixture.transitioning && fixture.root && fixture.root != frame.unit.entity &&
                !monsters.seen_deaths)
            {
                if (!unit_puppet_release(game) ||
                    unit_puppet_watch_stationary(game, local->player, fixture.root) != 1)
                {
                    backoff(game, now);
                    goto report;
                }
                monsters.seen_attacks = monsters.seen_deaths = 0;
                frame.unit = unit_puppet_sample(game);
            }
            frame.unit.transitioning = fixture.transitioning || frame.unit.transitioning;
            if (fixture.current_vid)
                frame.unit.vid = fixture.current_vid;
        }
        if (frame.unit.state == UNIT_PUPPET_FAULT)
        {
            backoff(game, now);
            goto report;
        }
        if (frame.unit.entity && !monsters.seen_deaths)
        {
            monsters.host_pose.x = frame.unit.x;
            monsters.host_pose.y = frame.unit.y;
            monsters.host_pose.z = frame.unit.z;
            monsters.host_pose.direction = frame.unit.direction;
            monsters.host_pose.velocity = frame.unit.velocity;
            monsters.host_pose.moving = frame.unit.moving;
        }
        uint32_t current_vid = frame.unit.vid ? frame.unit.vid : monsters.vid;
        uint32_t phase = frame.unit.transitioning ? MP_MONSTER_FORM_ARMOR : MP_MONSTER_FORM_ACTIVE;
        /* Medium armor already emits native debris. Large armor has no stock
           fragments, so give the host the same separate visual as its clients. */
        if ((current_vid == 90u && phase == MP_MONSTER_FORM_ARMOR) ||
            (monsters.vid == 90u && (current_vid == 80u || current_vid == 89u)))
        {
            if (!present_armor(game, &monsters.host_pose, 90u, monsters.form_revision, now))
                goto report;
        }
        if (current_vid != monsters.vid || phase != monsters.form_phase)
        {
            monsters.form_from = monsters.vid;
            monsters.vid = current_vid;
            monsters.form_phase = phase;
            ++monsters.form_revision;
            ++monsters.forms;
            stage(MP_MONSTER_FORM, tick, &monsters.host_pose);
        }
        monsters.state = phase == MP_MONSTER_FORM_ARMOR ? MS_TRANSFORMING : MS_ACTIVE;
        /* Counters come from native action callbacks, not animation snapshots. */
        unsigned int count = frame.unit.captured_attacks - monsters.seen_attacks;
        if (count > MONSTER_EVENT_CAPACITY)
        {
            monsters.dropped += count - MONSTER_EVENT_CAPACITY;
            count = MONSTER_EVENT_CAPACITY;
        }
        for (unsigned int i = 0; i < count; ++i)
        {
            stage(MP_MONSTER_ATTACK, tick, &monsters.host_pose);
            ++monsters.attacks;
        }
        monsters.seen_attacks = frame.unit.captured_attacks;
        if (!phase && !monsters.seen_deaths && (frame.unit.captured_deaths || !frame.unit.entity))
        {
            monsters.death_animation = frame.unit.death_animation == STEAM_ANIM_GIB_DEATH
                                           ? STEAM_ANIM_GIB_DEATH
                                           : STEAM_ANIM_DEATH;
            if (frame.unit.death_animation)
                monsters.host_pose = frame.unit.death_pose;
            monsters.host_pose.velocity = 0;
            monsters.host_pose.moving = 0;
            stage(MP_MONSTER_DEATH, tick, &monsters.host_pose);
            ++monsters.deaths;
            monsters.seen_deaths = frame.unit.captured_deaths ? frame.unit.captured_deaths : 1u;
        }
        MpMonsterState output = {0};
        output.sequence = ++monsters.sequence;
        output.world_low = monsters.low;
        output.world_high = monsters.high;
        output.world_epoch = epoch;
        output.monster_id = selected_slot + 1u;
        output.vid = monsters.vid;
        output.tick = tick;
        output.origin_vid = expected_vid(selected_slot);
        output.form_revision = monsters.form_revision;
        output.form_phase = phase;
        output.alive = phase || (!monsters.seen_deaths && frame.unit.entity &&
                                 frame.unit.animation < STEAM_ANIM_DEATH && frame.unit.health > 0);
        output.health = phase ? 1 : output.alive ? frame.unit.health : 0;
        output.x = monsters.host_pose.x;
        output.y = monsters.host_pose.y;
        output.z = monsters.host_pose.z;
        output.velocity = monsters.host_pose.velocity;
        output.moving = monsters.host_pose.moving;
        output.direction = monsters.host_pose.direction;
        if (phase)
        {
            output.velocity = 0;
            output.moving = 0;
        }
        output.death_animation = output.alive ? 0 : monsters.death_animation;
        output.event_sequence = monsters.event_sequence;
        if (TryAcquireSRWLockExclusive(&lock))
        {
            handoff.outgoing[selected_slot] = output;
            handoff.has_outgoing[selected_slot] = 1;
            unsigned int sent = 0;
            while (sent < monsters.staged_count && handoff.count < MONSTER_EVENT_CAPACITY)
            {
                handoff.events[(handoff.head + handoff.count++) % MONSTER_EVENT_CAPACITY] =
                    monsters.staged[sent++];
            }
            if (sent)
            {
                monsters.staged_count -= sent;
                memmove(monsters.staged, monsters.staged + sent,
                        monsters.staged_count * sizeof(*monsters.staged));
            }
            ReleaseSRWLockExclusive(&lock);
        }
    }
    else
    {
        const MpMonsterState* source = &incoming.state;
        if (!incoming.present || !same_world(source))
        {
            if (!cleanup(game))
                monsters.state = MS_ABANDONED;
            else
                monsters.state = MS_WAITING;
            goto report;
        }
        if (monsters.host_epoch != source->world_epoch ||
            monsters.authority_id != incoming.authority_id ||
            monsters.authority_session != incoming.authority_session)
        {
            if (!cleanup(game))
            {
                monsters.state = MS_ABANDONED;
                goto report;
            }
            monsters.host_epoch = source->world_epoch;
            monsters.authority_id = incoming.authority_id;
            monsters.authority_session = incoming.authority_session;
            monsters.state = MS_WAITING;
            monsters.remote_dead = monsters.death_presented = 0;
            monsters.death_animation = 0;
            monsters.vid = monsters.form_revision = monsters.form_phase = monsters.armor_presented =
                0;
            monsters.last_event = 0;
        }
        /* A late old form must not replace a newer allocation or a tombstone.
           The transport also validates progression; retain this game-thread
           boundary for events and stale handoff snapshots already in flight. */
        if (monsters.form_revision &&
            (source->form_revision < monsters.form_revision ||
             (source->form_revision == monsters.form_revision &&
              (source->vid != monsters.vid || source->form_phase != monsters.form_phase))))
        {
            ++monsters.rejected;
            frame.unit = unit_puppet_sample(game);
            goto report;
        }
        if (monsters.remote_dead && source->alive)
            goto report;
        /* Death events can confirm a provisional effect before the next state
           snapshot. Use the same reconciliation path for both deliveries. */
        if (!monsters.remote_dead && source->alive && !source->form_phase)
        {
            for (unsigned int i = 0; i < received_count; ++i)
            {
                const MonsterEvent* event = &received[i].message;
                const MpMonsterEvent* e = &event->event;
                if (e->kind != MP_MONSTER_DEATH || now - received[i].received_at > 250u ||
                    event->authority_id != incoming.authority_id ||
                    event->authority_session != incoming.authority_session ||
                    e->world_epoch != source->world_epoch || e->world_low != monsters.low ||
                    e->world_high != monsters.high || e->monster_id != selected_slot + 1u ||
                    !mp_monster_event_matches(source, e) ||
                    !mp_sequence_newer(e->sequence, monsters.last_event))
                    continue;
                monsters.last_event = e->sequence;
                ++considered;
                incoming.state.alive = 0;
                incoming.state.health = 0;
                incoming.state.death_animation = e->death_animation;
                incoming.state.x = e->x;
                incoming.state.y = e->y;
                incoming.state.z = e->z;
                incoming.state.direction = e->direction;
                break;
            }
        }
        if (!monsters.remote_dead && !predict_hit(game, source, now))
            goto report;
        if (monsters.prediction != PREDICTION_NONE)
        {
            if (!source->alive)
            {
                if (monsters.prediction == PREDICTION_DEATH &&
                    monsters.predicted_vid == source->vid &&
                    monsters.predicted_animation == source->death_animation)
                {
                    /* Promote the existing provisional effect, without replaying it. */
                    monsters.death_presented = 1;
                    ++monsters.confirmations;
                }
                else if (!correct_prediction(game))
                    goto report;
                forget_prediction();
            }
            else if (monsters.prediction != PREDICTION_DEATH && !source->form_phase &&
                     source->vid == monsters.predicted_vid &&
                     mp_sequence_newer(source->sequence, monsters.predicted_sequence) &&
                     ((monsters.prediction == PREDICTION_FORM &&
                       source->form_revision > monsters.form_revision) ||
                      (monsters.predicted_health > 0 &&
                       source->health <= monsters.predicted_health)))
            {
                forget_prediction();
                ++monsters.confirmations;
            }
            else if (source->form_revision > monsters.form_revision && !source->form_phase &&
                     source->vid != monsters.predicted_vid)
            {
                if (!correct_prediction(game))
                    goto report;
            }
            else if (now - monsters.predicted_at >= MONSTER_PREDICTION_MS &&
                     mp_sequence_newer(source->sequence, monsters.predicted_sequence))
            {
                if (!correct_prediction(game))
                    goto report;
            }
        }
        unsigned int previous_vid = monsters.vid, previous_phase = monsters.form_phase;
        if (source->form_revision != monsters.form_revision)
        {
            monsters.vid = source->vid;
            monsters.form_revision = source->form_revision;
            monsters.form_phase = source->form_phase;
            if (previous_vid)
                ++monsters.forms;
            /* Recover an omitted armor-pending packet while the successor is
               already current. A late join starts at the current form. */
            if (previous_vid && !previous_phase &&
                ((previous_vid == 70u && (source->vid == 60u || source->vid == 69u)) ||
                 (previous_vid == 90u && (source->vid == 80u || source->vid == 89u))))
            {
                UnitPuppetPose armor_pose = {source->x, source->y,         source->z,
                                             0,         source->direction, 0};
                if (!monsters.armor_presented &&
                    (!cleanup_live(game) ||
                     !present_armor(game, &armor_pose, previous_vid, source->form_revision, now)))
                    goto report;
            }
        }
        if (!source->alive)
        {
            UnitPuppetPose pose = {source->x, source->y, source->z, 0, source->direction, 0};
            present_death(game, &pose, source->death_animation, now);
            if (monsters.state == MS_DEAD)
            {
                frame.visual = unit_death_sample(game);
                if (frame.visual.fault ||
                    (now - monsters.death_at > 30000u && !unit_death_remove(game)))
                    monsters.state = MS_ABANDONED;
            }
            goto report;
        }
        if (monsters.remote_dead)
            goto report;
        UnitPuppetPose pose = {source->x,        source->y,         source->z,
                               source->velocity, source->direction, source->moving};
        unsigned int view_vid = source->vid;
        int view_health = source->health;
        if (source->form_phase == MP_MONSTER_FORM_ARMOR)
        {
            view_vid = source->vid == 70u ? 60u : 80u;
            view_health = 0;
            pose.velocity = 0;
            pose.moving = 0;
        }
        if (monsters.prediction == PREDICTION_DEATH)
        {
            monsters.state = MS_PREDICTED_DEAD;
            goto report;
        }
        if (monsters.prediction != PREDICTION_NONE)
        {
            view_vid = monsters.predicted_vid;
            view_health = monsters.predicted_health;
            if (mp_monster_frozen(view_vid))
            {
                pose.velocity = 0;
                pose.moving = 0;
            }
        }
        if ((source->form_phase == MP_MONSTER_FORM_ARMOR ||
             (monsters.prediction == PREDICTION_FORM &&
              ((monsters.presentation_vid == 70u && view_vid == 60u) ||
               (monsters.presentation_vid == 90u && view_vid == 80u)))) &&
            !monsters.armor_presented)
        {
            unsigned int armored = source->origin_vid;
            if (!cleanup_live(game) ||
                !present_armor(game, &pose, armored, source->form_revision, now))
                goto report;
        }
        int spawning = !monsters.presentation_vid;
        if (!client_copy(local, view_vid, &pose, view_health, now))
            goto report;
        if (spawning)
            monsters.last_event = source->event_sequence;
        if (monsters.prediction != PREDICTION_NONE && !monsters.predicted_health)
            monsters.predicted_health = unit_puppet_sample(game).health;
        if (source->form_phase == MP_MONSTER_FORM_ARMOR)
            monsters.state = MS_TRANSFORMING;
        for (unsigned int i = 0; i < received_count; ++i)
        {
            ++considered;
            const MonsterEvent* event = &received[i].message;
            const MpMonsterEvent* e = &event->event;
            if (now - received[i].received_at > 250u ||
                event->authority_id != monsters.authority_id ||
                event->authority_session != monsters.authority_session ||
                e->world_epoch != monsters.host_epoch || e->world_low != monsters.low ||
                e->world_high != monsters.high || e->monster_id != selected_slot + 1u ||
                e->vid != monsters.vid || e->origin_vid != expected_vid(selected_slot) ||
                e->form_revision != monsters.form_revision ||
                e->form_phase != monsters.form_phase ||
                !mp_sequence_newer(e->sequence, monsters.last_event))
            {
                ++monsters.rejected;
                continue;
            }
            monsters.last_event = e->sequence;
            /* Persistent state carries health and the complete current form.
               Form events are a deduplicated watermark; they never guess HP. */
            if (e->kind == MP_MONSTER_FORM)
                continue;
            if (e->kind == MP_MONSTER_DEATH)
            {
                UnitPuppetPose death_pose = {e->x, e->y, e->z, 0, e->direction, 0};
                if (monsters.prediction != PREDICTION_NONE && !correct_prediction(game))
                    break;
                present_death(game, &death_pose, e->death_animation, now);
                break;
            }
            if (e->kind != MP_MONSTER_ATTACK)
            {
                ++monsters.rejected;
                continue;
            }
            /* An old-form attack is still an acknowledged event, but its art
               must not replace a predicted frozen/successor presentation. */
            if (monsters.presentation_vid != monsters.vid || source->form_phase)
                continue;
            if (unit_puppet_replay(game, e->sequence, STEAM_ANIM_ATTACK) != 1)
            {
                backoff(game, now);
                break;
            }
            ++monsters.attacks;
        }
        frame.unit = unit_puppet_sample(game);
        if (monsters.state == MS_ACTIVE && frame.unit.state != UNIT_PUPPET_ACTIVE)
            backoff(game, now);
    }
report:
    monsters.rejected += received_count - considered;
    if (monsters.role == MONSTER_CLIENT)
        frame.visual = unit_death_sample(game);
    if (monsters.armor_presented)
    {
        unit_death_select(selected_slot + MP_MONSTER_CAPACITY);
        UnitDeathSample armor = unit_death_sample(game);
        if (armor.fault || (now - monsters.armor_at > 30000u && !unit_death_remove(game)))
            monsters.state = MS_ABANDONED;
        unit_death_select(selected_slot);
        frame.visual.entities += armor.entities;
        frame.visual.gibs += armor.gibs;
        frame.visual.fault = frame.visual.fault || armor.fault;
    }
    frame.death_animation = monsters.death_animation;
    frame.death_pose = monsters.host_pose;
    frame.state = monsters.state;
    frame.monster_id = monsters.host_captured || monsters.state == MS_ACTIVE || monsters.remote_dead
                           ? selected_slot + 1u
                           : 0u;
    frame.host_epoch = monsters.role == MONSTER_HOST ? monsters.local_epoch : monsters.host_epoch;
    frame.attacks = monsters.attacks;
    frame.deaths = monsters.deaths;
    frame.forms = monsters.forms;
    frame.vid = monsters.vid;
    frame.form_revision = monsters.form_revision;
    frame.form_phase = monsters.form_phase;
    frame.dropped =
        monsters.dropped + (unsigned int)InterlockedCompareExchange(&incoming_dropped, 0, 0);
    frame.rejected = monsters.rejected;
    frame.predictions = monsters.predictions;
    frame.confirmations = monsters.confirmations;
    frame.corrections = monsters.corrections;
    if (now - monsters.logged_at >= 500u || frame.cleanup)
    {
        frame.event = 1;
        monsters.logged_at = now;
    }
    return frame;
}
MonsterSyncFrame monster_sync_tick(const Snapshot* local, enum ProbeResult result, uint32_t epoch,
                                   uint32_t tick, DWORD now)
{
    MonsterSyncFrame output = {0};
    MonsterState incoming[MP_MONSTER_CAPACITY] = {0};
    ReceivedMonsterEvent received[MONSTER_EVENT_CAPACITY];
    unsigned int received_count;
    int local_authority;
    if (!TryAcquireSRWLockExclusive(&lock))
    {
        output.state = slots[0].state;
        return output;
    }
    for (unsigned int i = 0; i < slot_count; ++i)
        if (now - handoff.incoming_at[i] <= 1000u)
            incoming[i] = handoff.incoming[i];
    local_authority = handoff.local_authority;
    received_count = handoff.received_count;
    memcpy(received, handoff.received, received_count * sizeof(*received));
    handoff.received_count = 0;
    ReleaseSRWLockExclusive(&lock);
    MonsterGallerySample samples[MP_MONSTER_CAPACITY] = {0};
    unsigned int active = 0, dead = 0, faults = 0, deaths = 0, attacks = 0;
    unsigned int predictions = 0, confirmations = 0, corrections = 0;
    int cleanup_all = 1;
    for (selected_slot = 0; selected_slot < slot_count; ++selected_slot)
    {
        unit_puppet_select(selected_slot);
        unit_combat_select(selected_slot);
        unit_death_select(selected_slot);
        ReceivedMonsterEvent matching[MONSTER_EVENT_CAPACITY];
        unsigned int count = 0;
        for (unsigned int j = 0; j < received_count; ++j)
            if (received[j].message.event.monster_id == selected_slot + 1u)
                matching[count++] = received[j];
        MonsterSyncFrame frame =
            tick_slot(local, result, epoch, tick, now, &incoming[selected_slot], local_authority,
                      matching, count);
        if (!selected_slot)
            output = frame;
        cleanup_all = cleanup_all && frame.cleanup;
        MonsterGallerySample* sample = &samples[selected_slot];
        sample->id = selected_slot + 1u;
        sample->vid = frame.vid;
        sample->state = frame.state;
        sample->form_revision = frame.form_revision;
        sample->form_phase = frame.form_phase;
        sample->presentation_vid = slots[selected_slot].presentation_vid;
        sample->prediction = (unsigned int)slots[selected_slot].prediction;
        sample->entity = frame.unit.entity;
        sample->health = frame.unit.health;
        sample->animation = frame.unit.animation;
        sample->behavior = frame.unit.behavior;
        sample->death_animation = frame.death_animation;
        sample->x = frame.unit.entity ? frame.unit.x : frame.death_pose.x;
        sample->y = frame.unit.entity ? frame.unit.y : frame.death_pose.y;
        sample->effects = frame.visual.entities;
        sample->fault = frame.visual.fault;
        active +=
            (frame.state == MS_ACTIVE || frame.state == MS_TRANSFORMING) && frame.unit.entity != 0;
        dead += frame.state == MS_DEAD || slots[selected_slot].seen_deaths != 0;
        faults += frame.state == MS_ABANDONED || frame.state == MS_BACKOFF || frame.visual.fault;
        deaths += frame.deaths;
        attacks += frame.attacks;
        predictions += frame.predictions;
        confirmations += frame.confirmations;
        corrections += frame.corrections;
    }
    output.gallery_count = gallery ? slot_count : 0u;
    output.gallery_active = active;
    output.gallery_dead = dead;
    output.gallery_faults = faults;
    memcpy(output.gallery, samples, sizeof(samples));
    output.cleanup = cleanup_all;
    if (gallery)
    {
        output.deaths = deaths;
        output.attacks = attacks;
        output.predictions = predictions;
        output.confirmations = confirmations;
        output.corrections = corrections;
    }
    selected_slot = 0;
    unit_puppet_select(0);
    unit_combat_select(0);
    unit_death_select(0);
    return output;
}
void monster_sync_request_stop(void)
{
    for (unsigned int i = 0; i < slot_count; ++i)
        InterlockedExchange(&slots[i].stopping, 1);
}
int monster_sync_stopped(void)
{
    for (unsigned int i = 0; i < slot_count; ++i)
        if (slots[i].state != MS_OFF && !InterlockedCompareExchange(&slots[i].stopped, 0, 0))
            return 0;
    return 1;
}
int monster_sync_stop(void)
{
    if (slots[0].state == MS_OFF)
        return 1;
    if (!monster_sync_stopped())
        return 0;
    int fixture = unit_combat_stop();
    int visual = unit_death_stop();
    if (!fixture || !visual || !unit_puppet_stop())
        return 0;
    AcquireSRWLockExclusive(&lock);
    memset(&handoff, 0, sizeof(handoff));
    ReleaseSRWLockExclusive(&lock);
    memset(slots, 0, sizeof(slots));
    selected_slot = 0;
    slot_count = 1u;
    gallery = 0;
    return 1;
}
