#include <string.h>
#include "monster_sync.h"
#include "retry_policy.h"

#define MONSTER_EVENT_CAPACITY 64u
#define MONSTER_PREDICTION_MS  750u
/* A new world must stay stable this long before monsters are created. */
#define MONSTER_SETTLE_MS 2000u
/* Death and armor-debris presentations are removed after this long. */
#define MONSTER_PRESENTATION_LIFETIME_MS 30000u
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
/* One synchronized monster. Every function below that works on a monster
   receives its slot explicitly; there is no "currently selected" slot. */
typedef struct MonsterSlot
{
    unsigned int index;  /* Position in slots[]; wire monster_id is index + 1. */
    UnitPuppet* puppet;  /* Borrowed native UNIT for this monster (same index). */
    UnitCombat* fixture; /* Private native copy for the host gallery or a client. */
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
static unsigned int slot_count = 1u;
static int gallery;
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
    if (slots[0].state != MS_OFF || (role != MONSTER_HOST && role != MONSTER_CLIENT) || !api)
        return 0;
    memset(slots, 0, sizeof(slots));
    memset(&handoff, 0, sizeof(handoff));
    for (unsigned int i = 0; i < MP_MONSTER_CAPACITY; ++i)
    {
        slots[i].index = i;
        slots[i].puppet = unit_puppet_slot(i);
        slots[i].fixture = unit_combat_slot(i);
    }
    slot_count = 1u;
    gallery = 0;
    incoming_dropped = 0;
    slots[0].role = role;
    slots[0].state = MS_STOPPING;
    slots[0].stopped = 1;
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
        slots[i].retry_delay = MP_RETRY_FIRST_MS;
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
static unsigned int expected_vid(unsigned int index)
{
    static const unsigned int types[] = {
        MONSTER_VID_SLUG, MONSTER_VID_MONSTER1,         MONSTER_VID_MONSTER2,
        MONSTER_VID_TANK, MONSTER_VID_MONSTER2_ARMORED, MONSTER_VID_TANK_ARMORED};
    return gallery ? types[index / 8u] : MONSTER_VID_MONSTER1;
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
static void stage_event(MonsterSlot* slot, unsigned int kind, uint32_t tick,
                        const UnitPuppetPose* pose)
{
    MpMonsterEvent event = {0};
    event.sequence = ++slot->event_sequence;
    if (!event.sequence)
        event.sequence = ++slot->event_sequence;
    event.world_low = slot->low;
    event.world_high = slot->high;
    event.world_epoch = slot->local_epoch;
    event.monster_id = slot->index + 1u;
    event.tick = tick;
    event.kind = kind;
    event.x = pose->x;
    event.y = pose->y;
    event.z = pose->z;
    event.direction = pose->direction;
    event.death_animation = kind == MP_MONSTER_DEATH ? slot->death_animation : 0;
    event.vid = slot->vid;
    event.origin_vid = expected_vid(slot->index);
    event.form_revision = slot->form_revision;
    event.form_phase = slot->form_phase;
    event.from_vid = kind == MP_MONSTER_FORM ? slot->form_from : 0u;
    if (kind == MP_MONSTER_FORM && slot->form_phase == MP_MONSTER_FORM_ARMOR)
        event.death_animation = STEAM_ANIM_DEATH;
    if (slot->staged_count < MONSTER_EVENT_CAPACITY)
        slot->staged[slot->staged_count++] = event;
    else
        ++slot->dropped;
}
static int cleanup(MonsterSlot* slot, uintptr_t game)
{
    if (!unit_puppet_release(slot->puppet, game))
        return 0;
    if ((slot->role == MONSTER_CLIENT || gallery) && !unit_combat_remove(slot->fixture, game))
        return 0;
    {
        if (!unit_death_remove(game))
            return 0;
        unit_death_select(slot->index + MP_MONSTER_CAPACITY);
        int armor = unit_death_remove(game);
        unit_death_select(slot->index);
        if (!armor)
            return 0;
    }
    slot->presentation_vid = slot->predicted_hits = slot->armor_presented = 0;
    slot->prediction = PREDICTION_NONE;
    return 1;
}
static void schedule_retry(MonsterSlot* slot, uintptr_t game, DWORD now)
{
    if (!cleanup(slot, game))
    {
        slot->state = MS_ABANDONED;
        return;
    }
    slot->host_captured = 0;
    slot->seen_attacks = slot->seen_deaths = 0;
    slot->armor_presented = 0;
    slot->state = MS_BACKOFF;
    slot->retry_at = now + slot->retry_delay;
    if (slot->retry_delay < MP_RETRY_MAX_MS)
    {
        slot->retry_delay *= 2u;
        if (slot->retry_delay > MP_RETRY_MAX_MS)
            slot->retry_delay = MP_RETRY_MAX_MS;
    }
}
static int same_world(const MonsterSlot* slot, const MpMonsterState* s)
{
    return s->world_low == slot->low && s->world_high == slot->high && s->world_epoch &&
           s->monster_id == slot->index + 1u && s->origin_vid == expected_vid(slot->index) &&
           mp_monster_form_valid(s->vid, s->origin_vid, s->form_revision, s->form_phase);
}
static int cleanup_live(MonsterSlot* slot, uintptr_t game)
{
    int removed =
        unit_puppet_release(slot->puppet, game) && unit_combat_remove(slot->fixture, game);
    if (removed)
    {
        slot->presentation_vid = 0;
        slot->predicted_hits = 0;
    }
    else
        slot->state = MS_ABANDONED;
    return removed;
}
static int present_armor(MonsterSlot* slot, uintptr_t game, const UnitPuppetPose* pose,
                         unsigned int source_vid, uint32_t revision, DWORD now)
{
    /* Each supported origin loses armor once; pending and successor revisions
       describe the same effect, including an earlier local prediction. */
    if (slot->armor_presented)
        return 1;
    unit_death_select(slot->index + MP_MONSTER_CAPACITY);
    int removed = unit_death_remove(game);
    int played = removed ? unit_death_play_armor(game, pose, source_vid) : 0;
    unit_death_select(slot->index);
    if (!removed)
    {
        slot->state = MS_ABANDONED;
        return 0;
    }
    if (played != 1)
    {
        schedule_retry(slot, game, now);
        return 0;
    }
    slot->armor_presented = revision;
    slot->armor_at = now;
    return 1;
}
static void present_death(MonsterSlot* slot, uintptr_t game, const UnitPuppetPose* pose,
                          unsigned int animation, DWORD now)
{
    if (!slot->remote_dead)
        ++slot->deaths;
    slot->remote_dead = 1;
    slot->death_animation = animation;
    slot->host_pose = *pose;
    if (slot->death_presented)
    {
        slot->state = MS_DEAD;
        return;
    }
    /* Armor debris has its own bounded visual slot and may outlive this kill. */
    if (!cleanup_live(slot, game) || !unit_death_remove(game))
    {
        slot->state = MS_ABANDONED;
        return;
    }
    InterlockedExchange(&slot->stopped, 0);
    if (unit_death_play_type(game, pose, animation, slot->vid) != 1)
    {
        schedule_retry(slot, game, now);
        return;
    }
    slot->death_presented = 1;
    slot->death_at = now;
    slot->state = MS_DEAD;
}
static int client_copy(MonsterSlot* slot, const Snapshot* local, unsigned int vid,
                       const UnitPuppetPose* pose, int health, DWORD now)
{
    uintptr_t game = local->game;
    if (slot->presentation_vid != vid)
    {
        if (!cleanup_live(slot, game))
        {
            slot->state = MS_ABANDONED;
            return 0;
        }
        Snapshot spawn = *local;
        spawn.health = 1;
        InterlockedExchange(&slot->stopped, 0);
        if (unit_combat_spawn_predictive(slot->fixture, &spawn, vid, pose) != 1)
        {
            schedule_retry(slot, game, now);
            return 0;
        }
        UnitCombatSample fixture_sample = unit_combat_sample(slot->fixture, game, 1);
        if (fixture_sample.fault || !fixture_sample.sources_unchanged ||
            unit_puppet_attach_entity(slot->puppet, game, local->player, fixture_sample.root) !=
                1 ||
            unit_puppet_prediction(slot->puppet, game) != 1)
        {
            schedule_retry(slot, game, now);
            return 0;
        }
        slot->presentation_vid = vid;
        slot->predicted_hits = 0;
        slot->retry_delay = MP_RETRY_FIRST_MS;
    }
    slot->state = MS_ACTIVE;
    if (unit_puppet_apply(slot->puppet, game, pose) != 1 ||
        (health > 0 && unit_puppet_health(slot->puppet, game, health) != 1))
    {
        schedule_retry(slot, game, now);
        return 0;
    }
    return 1;
}
static void forget_prediction(MonsterSlot* slot)
{
    slot->prediction = PREDICTION_NONE;
    slot->predicted_vid = 0;
}
static int correct_prediction(MonsterSlot* slot, uintptr_t game)
{
    if (!cleanup(slot, game))
    {
        slot->state = MS_ABANDONED;
        return 0;
    }
    slot->presentation_vid = slot->predicted_hits = slot->armor_presented = 0;
    slot->death_presented = 0;
    slot->state = MS_WAITING;
    forget_prediction(slot);
    ++slot->corrections;
    return 1;
}
static int predict_hit(MonsterSlot* slot, uintptr_t game, const MpMonsterState* source, DWORD now)
{
    if (!slot->presentation_vid || slot->prediction == PREDICTION_DEATH)
        return 1;
    UnitPuppetSample sample = unit_puppet_sample(slot->puppet, game);
    if (sample.state == UNIT_PUPPET_FAULT)
    {
        schedule_retry(slot, game, now);
        return 0;
    }
    if (sample.predicted_hits == slot->predicted_hits)
        return 1;
    slot->predicted_hits = sample.predicted_hits;
    if (slot->prediction == PREDICTION_NONE)
    {
        slot->predicted_at = now;
        slot->predicted_sequence = source->sequence;
        slot->prediction = PREDICTION_HEALTH;
        ++slot->predictions;
    }
    slot->predicted_vid = sample.vid ? sample.vid : slot->presentation_vid;
    slot->predicted_health = sample.health;
    if (sample.transitioning)
    {
        slot->prediction = PREDICTION_FORM;
        slot->predicted_vid = mp_monster_armor_successor(sample.vid);
        slot->predicted_health = 0; /* Native successor constructor supplies HP. */
    }
    else if (sample.captured_deaths)
    {
        slot->prediction = PREDICTION_DEATH;
        slot->predicted_animation = sample.death_animation;
        UnitPuppetPose pose = sample.death_pose;
        if (!cleanup_live(slot, game) || !unit_death_remove(game))
        {
            slot->state = MS_ABANDONED;
            return 0;
        }
        if (unit_death_play_type(game, &pose, sample.death_animation, slot->predicted_vid) != 1)
        {
            schedule_retry(slot, game, now);
            return 0;
        }
        slot->death_at = now;
        slot->host_pose = pose;
        slot->state = MS_PREDICTED_DEAD;
    }
    else if (sample.vid != slot->presentation_vid)
    {
        /* Native freeze already changed our private descriptor atomically. */
        slot->prediction = PREDICTION_FORM;
        slot->presentation_vid = sample.vid;
    }
    else if (slot->prediction == PREDICTION_FORM)
    {
        /* A later hit on the successor/frozen form needs its own HP result;
           merely receiving the form transition cannot acknowledge that hit. */
        slot->prediction = PREDICTION_HEALTH;
    }
    return 1;
}
static MonsterSyncFrame tick_slot(MonsterSlot* slot, const Snapshot* local, enum ProbeResult result,
                                  uint32_t epoch, uint32_t tick, DWORD now,
                                  const MonsterState* published, int local_authority,
                                  const ReceivedMonsterEvent* received, unsigned int received_count)
{
    MonsterSyncFrame frame = {0};
    frame.role = slot->role;
    if (slot->state == MS_OFF)
        return frame;
    MonsterState incoming = *published;
    unsigned int considered = 0;
    uintptr_t game = local ? local->game : 0;
    if (InterlockedCompareExchange(&slot->stopping, 0, 0))
    {
        slot->state = MS_STOPPING;
        frame.cleanup = cleanup(slot, game);
        if (frame.cleanup)
            InterlockedExchange(&slot->stopped, 1);
        goto report;
    }
    int valid_world =
        local && (result == PROBE_OK || result == PROBE_NO_PLAYER || result == PROBE_NO_ARMY) &&
        local->in_level && epoch;
    if (!valid_world || !slot->world || slot->local_epoch != epoch ||
        local->world_low != slot->low || local->world_high != slot->high)
    {
        if (!cleanup(slot, game))
        {
            slot->state = MS_ABANDONED;
            goto report;
        }
        slot->world = 0;
        slot->host_captured = 0;
        slot->staged_count = 0;
        slot->seen_attacks = slot->seen_deaths = slot->event_sequence = 0;
        slot->last_event = slot->host_epoch = slot->authority_session = 0;
        slot->remote_dead = slot->death_presented = 0;
        slot->vid = expected_vid(slot->index);
        slot->form_revision = 1u;
        slot->form_phase = MP_MONSTER_FORM_ACTIVE;
        slot->form_from = slot->vid;
        slot->armor_presented = 0;
        slot->death_animation = 0;
        memset(&slot->host_pose, 0, sizeof(slot->host_pose));
        slot->state = MS_WAITING;
        slot->retry_delay = MP_RETRY_FIRST_MS;
        if (!valid_world)
            goto report;
        slot->game = game;
        slot->local_epoch = epoch;
        slot->low = local->world_low;
        slot->high = local->world_high;
        slot->settled_at = now;
        slot->world = 1;
    }
    if (slot->state == MS_ABANDONED)
        goto report;
    if (slot->state == MS_BACKOFF && (int32_t)(now - slot->retry_at) < 0)
        goto report;
    if (slot->state == MS_BACKOFF)
        slot->state = MS_WAITING;
    if (now - slot->settled_at < MONSTER_SETTLE_MS)
        goto report;
    if (slot->role == MONSTER_HOST)
    {
        if (!local_authority)
        {
            if (!cleanup(slot, game))
                slot->state = MS_ABANDONED;
            else
            {
                slot->state = MS_WAITING;
                slot->host_captured = 0;
                slot->seen_attacks = slot->seen_deaths = 0;
            }
            goto report;
        }
        if (!slot->host_captured)
        {
            if (!local->player)
                goto report;
            int attached;
            if (gallery)
            {
                Snapshot spawn = *local;
                spawn.health = 1;
                UnitPuppetPose position = {local->x + 160.0f + (slot->index % 8u) * 80.0f,
                                           local->y - 150.0f + (slot->index / 8u) * 100.0f,
                                           local->z,
                                           0,
                                           128u,
                                           0};
                int created = unit_combat_spawn_type(slot->fixture, &spawn,
                                                     expected_vid(slot->index), &position, 1);
                if (created != 1)
                {
                    schedule_retry(slot, game, now);
                    goto report;
                }
                UnitCombatSample fixture_sample = unit_combat_sample(slot->fixture, game, 0);
                attached = fixture_sample.fault
                               ? -1
                               : unit_puppet_watch_stationary(slot->puppet, game, local->player,
                                                              fixture_sample.root);
            }
            else
                attached = unit_puppet_watch_first(slot->puppet, game, local->player);
            if (attached < 0)
            {
                schedule_retry(slot, game, now);
                goto report;
            }
            if (!attached)
                goto report;
            slot->host_captured = 1;
            slot->state = MS_ACTIVE;
            InterlockedExchange(&slot->stopped, 0);
        }
        frame.unit = unit_puppet_sample(slot->puppet, game);
        if (gallery)
        {
            UnitCombatSample fixture_sample = unit_combat_sample(slot->fixture, game, 0);
            if (fixture_sample.fault)
            {
                schedule_retry(slot, game, now);
                goto report;
            }
            if (!fixture_sample.transitioning && fixture_sample.root &&
                fixture_sample.root != frame.unit.entity && !slot->seen_deaths)
            {
                if (!unit_puppet_release(slot->puppet, game) ||
                    unit_puppet_watch_stationary(slot->puppet, game, local->player,
                                                 fixture_sample.root) != 1)
                {
                    schedule_retry(slot, game, now);
                    goto report;
                }
                slot->seen_attacks = slot->seen_deaths = 0;
                frame.unit = unit_puppet_sample(slot->puppet, game);
            }
            frame.unit.transitioning = fixture_sample.transitioning || frame.unit.transitioning;
            if (fixture_sample.current_vid)
                frame.unit.vid = fixture_sample.current_vid;
        }
        if (frame.unit.state == UNIT_PUPPET_FAULT)
        {
            schedule_retry(slot, game, now);
            goto report;
        }
        if (frame.unit.entity && !slot->seen_deaths)
        {
            slot->host_pose.x = frame.unit.x;
            slot->host_pose.y = frame.unit.y;
            slot->host_pose.z = frame.unit.z;
            slot->host_pose.direction = frame.unit.direction;
            slot->host_pose.velocity = frame.unit.velocity;
            slot->host_pose.moving = frame.unit.moving;
        }
        uint32_t current_vid = frame.unit.vid ? frame.unit.vid : slot->vid;
        uint32_t phase = frame.unit.transitioning ? MP_MONSTER_FORM_ARMOR : MP_MONSTER_FORM_ACTIVE;
        /* Medium armor already emits native debris. Large armor has no stock
           fragments, so give the host the same separate visual as its clients. */
        if ((current_vid == MONSTER_VID_TANK_ARMORED && phase == MP_MONSTER_FORM_ARMOR) ||
            (slot->vid == MONSTER_VID_TANK_ARMORED &&
             (current_vid == MONSTER_VID_TANK ||
              current_vid == MONSTER_VID_TANK + MONSTER_VID_FROZEN_DELTA)))
        {
            if (!present_armor(slot, game, &slot->host_pose, MONSTER_VID_TANK_ARMORED,
                               slot->form_revision, now))
                goto report;
        }
        if (current_vid != slot->vid || phase != slot->form_phase)
        {
            slot->form_from = slot->vid;
            slot->vid = current_vid;
            slot->form_phase = phase;
            ++slot->form_revision;
            ++slot->forms;
            stage_event(slot, MP_MONSTER_FORM, tick, &slot->host_pose);
        }
        slot->state = phase == MP_MONSTER_FORM_ARMOR ? MS_TRANSFORMING : MS_ACTIVE;
        /* Counters come from native action callbacks, not animation snapshots. */
        unsigned int count = frame.unit.captured_attacks - slot->seen_attacks;
        if (count > MONSTER_EVENT_CAPACITY)
        {
            slot->dropped += count - MONSTER_EVENT_CAPACITY;
            count = MONSTER_EVENT_CAPACITY;
        }
        for (unsigned int i = 0; i < count; ++i)
        {
            stage_event(slot, MP_MONSTER_ATTACK, tick, &slot->host_pose);
            ++slot->attacks;
        }
        slot->seen_attacks = frame.unit.captured_attacks;
        if (!phase && !slot->seen_deaths && (frame.unit.captured_deaths || !frame.unit.entity))
        {
            slot->death_animation = frame.unit.death_animation == STEAM_ANIM_GIB_DEATH
                                        ? STEAM_ANIM_GIB_DEATH
                                        : STEAM_ANIM_DEATH;
            if (frame.unit.death_animation)
                slot->host_pose = frame.unit.death_pose;
            slot->host_pose.velocity = 0;
            slot->host_pose.moving = 0;
            stage_event(slot, MP_MONSTER_DEATH, tick, &slot->host_pose);
            ++slot->deaths;
            slot->seen_deaths = frame.unit.captured_deaths ? frame.unit.captured_deaths : 1u;
        }
        MpMonsterState output = {0};
        output.sequence = ++slot->sequence;
        output.world_low = slot->low;
        output.world_high = slot->high;
        output.world_epoch = epoch;
        output.monster_id = slot->index + 1u;
        output.vid = slot->vid;
        output.tick = tick;
        output.origin_vid = expected_vid(slot->index);
        output.form_revision = slot->form_revision;
        output.form_phase = phase;
        output.alive = phase || (!slot->seen_deaths && frame.unit.entity &&
                                 frame.unit.animation < STEAM_ANIM_DEATH && frame.unit.health > 0);
        output.health = phase ? 1 : output.alive ? frame.unit.health : 0;
        output.x = slot->host_pose.x;
        output.y = slot->host_pose.y;
        output.z = slot->host_pose.z;
        output.velocity = slot->host_pose.velocity;
        output.moving = slot->host_pose.moving;
        output.direction = slot->host_pose.direction;
        if (phase)
        {
            output.velocity = 0;
            output.moving = 0;
        }
        output.death_animation = output.alive ? 0 : slot->death_animation;
        output.event_sequence = slot->event_sequence;
        if (TryAcquireSRWLockExclusive(&lock))
        {
            handoff.outgoing[slot->index] = output;
            handoff.has_outgoing[slot->index] = 1;
            unsigned int sent = 0;
            while (sent < slot->staged_count && handoff.count < MONSTER_EVENT_CAPACITY)
            {
                handoff.events[(handoff.head + handoff.count++) % MONSTER_EVENT_CAPACITY] =
                    slot->staged[sent++];
            }
            if (sent)
            {
                slot->staged_count -= sent;
                memmove(slot->staged, slot->staged + sent,
                        slot->staged_count * sizeof(*slot->staged));
            }
            ReleaseSRWLockExclusive(&lock);
        }
    }
    else
    {
        const MpMonsterState* source = &incoming.state;
        if (!incoming.present || !same_world(slot, source))
        {
            if (!cleanup(slot, game))
                slot->state = MS_ABANDONED;
            else
                slot->state = MS_WAITING;
            goto report;
        }
        if (slot->host_epoch != source->world_epoch ||
            slot->authority_id != incoming.authority_id ||
            slot->authority_session != incoming.authority_session)
        {
            if (!cleanup(slot, game))
            {
                slot->state = MS_ABANDONED;
                goto report;
            }
            slot->host_epoch = source->world_epoch;
            slot->authority_id = incoming.authority_id;
            slot->authority_session = incoming.authority_session;
            slot->state = MS_WAITING;
            slot->remote_dead = slot->death_presented = 0;
            slot->death_animation = 0;
            slot->vid = slot->form_revision = slot->form_phase = slot->armor_presented = 0;
            slot->last_event = 0;
        }
        /* A late old form must not replace a newer allocation or a tombstone.
           The transport also validates progression; retain this game-thread
           boundary for events and stale handoff snapshots already in flight. */
        if (slot->form_revision &&
            (source->form_revision < slot->form_revision ||
             (source->form_revision == slot->form_revision &&
              (source->vid != slot->vid || source->form_phase != slot->form_phase))))
        {
            ++slot->rejected;
            frame.unit = unit_puppet_sample(slot->puppet, game);
            goto report;
        }
        if (slot->remote_dead && source->alive)
            goto report;
        /* Death events can confirm a provisional effect before the next state
           snapshot. Use the same reconciliation path for both deliveries. */
        if (!slot->remote_dead && source->alive && !source->form_phase)
        {
            for (unsigned int i = 0; i < received_count; ++i)
            {
                const MonsterEvent* event = &received[i].message;
                const MpMonsterEvent* e = &event->event;
                if (e->kind != MP_MONSTER_DEATH || now - received[i].received_at > 250u ||
                    event->authority_id != incoming.authority_id ||
                    event->authority_session != incoming.authority_session ||
                    e->world_epoch != source->world_epoch || e->world_low != slot->low ||
                    e->world_high != slot->high || e->monster_id != slot->index + 1u ||
                    !mp_monster_event_matches(source, e) ||
                    !mp_sequence_newer(e->sequence, slot->last_event))
                    continue;
                slot->last_event = e->sequence;
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
        if (!slot->remote_dead && !predict_hit(slot, game, source, now))
            goto report;
        if (slot->prediction != PREDICTION_NONE)
        {
            if (!source->alive)
            {
                if (slot->prediction == PREDICTION_DEATH && slot->predicted_vid == source->vid &&
                    slot->predicted_animation == source->death_animation)
                {
                    /* Promote the existing provisional effect, without replaying it. */
                    slot->death_presented = 1;
                    ++slot->confirmations;
                }
                else if (!correct_prediction(slot, game))
                    goto report;
                forget_prediction(slot);
            }
            else if (slot->prediction != PREDICTION_DEATH && !source->form_phase &&
                     source->vid == slot->predicted_vid &&
                     mp_sequence_newer(source->sequence, slot->predicted_sequence) &&
                     ((slot->prediction == PREDICTION_FORM &&
                       source->form_revision > slot->form_revision) ||
                      (slot->predicted_health > 0 && source->health <= slot->predicted_health)))
            {
                forget_prediction(slot);
                ++slot->confirmations;
            }
            else if (source->form_revision > slot->form_revision && !source->form_phase &&
                     source->vid != slot->predicted_vid)
            {
                if (!correct_prediction(slot, game))
                    goto report;
            }
            else if (now - slot->predicted_at >= MONSTER_PREDICTION_MS &&
                     mp_sequence_newer(source->sequence, slot->predicted_sequence))
            {
                if (!correct_prediction(slot, game))
                    goto report;
            }
        }
        unsigned int previous_vid = slot->vid, previous_phase = slot->form_phase;
        if (source->form_revision != slot->form_revision)
        {
            slot->vid = source->vid;
            slot->form_revision = source->form_revision;
            slot->form_phase = source->form_phase;
            if (previous_vid)
                ++slot->forms;
            /* Recover an omitted armor-pending packet while the successor is
               already current. A late join starts at the current form. */
            if (previous_vid && !previous_phase &&
                ((previous_vid == MONSTER_VID_MONSTER2_ARMORED &&
                  (source->vid == MONSTER_VID_MONSTER2 ||
                   source->vid == MONSTER_VID_MONSTER2 + MONSTER_VID_FROZEN_DELTA)) ||
                 (previous_vid == MONSTER_VID_TANK_ARMORED &&
                  (source->vid == MONSTER_VID_TANK ||
                   source->vid == MONSTER_VID_TANK + MONSTER_VID_FROZEN_DELTA))))
            {
                UnitPuppetPose armor_pose = {source->x, source->y,         source->z,
                                             0,         source->direction, 0};
                if (!slot->armor_presented && (!cleanup_live(slot, game) ||
                                               !present_armor(slot, game, &armor_pose, previous_vid,
                                                              source->form_revision, now)))
                    goto report;
            }
        }
        if (!source->alive)
        {
            UnitPuppetPose pose = {source->x, source->y, source->z, 0, source->direction, 0};
            present_death(slot, game, &pose, source->death_animation, now);
            if (slot->state == MS_DEAD)
            {
                frame.visual = unit_death_sample(game);
                if (frame.visual.fault ||
                    (now - slot->death_at > MONSTER_PRESENTATION_LIFETIME_MS &&
                     !unit_death_remove(game)))
                    slot->state = MS_ABANDONED;
            }
            goto report;
        }
        if (slot->remote_dead)
            goto report;
        UnitPuppetPose pose = {source->x,        source->y,         source->z,
                               source->velocity, source->direction, source->moving};
        unsigned int view_vid = source->vid;
        int view_health = source->health;
        if (source->form_phase == MP_MONSTER_FORM_ARMOR)
        {
            view_vid = mp_monster_armor_successor(source->vid);
            view_health = 0;
            pose.velocity = 0;
            pose.moving = 0;
        }
        if (slot->prediction == PREDICTION_DEATH)
        {
            slot->state = MS_PREDICTED_DEAD;
            goto report;
        }
        if (slot->prediction != PREDICTION_NONE)
        {
            view_vid = slot->predicted_vid;
            view_health = slot->predicted_health;
            if (mp_monster_frozen(view_vid))
            {
                pose.velocity = 0;
                pose.moving = 0;
            }
        }
        if ((source->form_phase == MP_MONSTER_FORM_ARMOR ||
             (slot->prediction == PREDICTION_FORM &&
              ((slot->presentation_vid == MONSTER_VID_MONSTER2_ARMORED &&
                view_vid == MONSTER_VID_MONSTER2) ||
               (slot->presentation_vid == MONSTER_VID_TANK_ARMORED &&
                view_vid == MONSTER_VID_TANK)))) &&
            !slot->armor_presented)
        {
            unsigned int armored = source->origin_vid;
            if (!cleanup_live(slot, game) ||
                !present_armor(slot, game, &pose, armored, source->form_revision, now))
                goto report;
        }
        int spawning = !slot->presentation_vid;
        if (!client_copy(slot, local, view_vid, &pose, view_health, now))
            goto report;
        if (spawning)
            slot->last_event = source->event_sequence;
        if (slot->prediction != PREDICTION_NONE && !slot->predicted_health)
            slot->predicted_health = unit_puppet_sample(slot->puppet, game).health;
        if (source->form_phase == MP_MONSTER_FORM_ARMOR)
            slot->state = MS_TRANSFORMING;
        for (unsigned int i = 0; i < received_count; ++i)
        {
            ++considered;
            const MonsterEvent* event = &received[i].message;
            const MpMonsterEvent* e = &event->event;
            if (now - received[i].received_at > 250u || event->authority_id != slot->authority_id ||
                event->authority_session != slot->authority_session ||
                e->world_epoch != slot->host_epoch || e->world_low != slot->low ||
                e->world_high != slot->high || e->monster_id != slot->index + 1u ||
                e->vid != slot->vid || e->origin_vid != expected_vid(slot->index) ||
                e->form_revision != slot->form_revision || e->form_phase != slot->form_phase ||
                !mp_sequence_newer(e->sequence, slot->last_event))
            {
                ++slot->rejected;
                continue;
            }
            slot->last_event = e->sequence;
            /* Persistent state carries health and the complete current form.
               Form events are a deduplicated watermark; they never guess HP. */
            if (e->kind == MP_MONSTER_FORM)
                continue;
            if (e->kind == MP_MONSTER_DEATH)
            {
                UnitPuppetPose death_pose = {e->x, e->y, e->z, 0, e->direction, 0};
                if (slot->prediction != PREDICTION_NONE && !correct_prediction(slot, game))
                    break;
                present_death(slot, game, &death_pose, e->death_animation, now);
                break;
            }
            if (e->kind != MP_MONSTER_ATTACK)
            {
                ++slot->rejected;
                continue;
            }
            /* An old-form attack is still an acknowledged event, but its art
               must not replace a predicted frozen/successor presentation. */
            if (slot->presentation_vid != slot->vid || source->form_phase)
                continue;
            if (unit_puppet_replay(slot->puppet, game, e->sequence, STEAM_ANIM_ATTACK) != 1)
            {
                schedule_retry(slot, game, now);
                break;
            }
            ++slot->attacks;
        }
        frame.unit = unit_puppet_sample(slot->puppet, game);
        if (slot->state == MS_ACTIVE && frame.unit.state != UNIT_PUPPET_ACTIVE)
            schedule_retry(slot, game, now);
    }
report:
    slot->rejected += received_count - considered;
    if (slot->role == MONSTER_CLIENT)
        frame.visual = unit_death_sample(game);
    if (slot->armor_presented)
    {
        unit_death_select(slot->index + MP_MONSTER_CAPACITY);
        UnitDeathSample armor = unit_death_sample(game);
        if (armor.fault ||
            (now - slot->armor_at > MONSTER_PRESENTATION_LIFETIME_MS && !unit_death_remove(game)))
            slot->state = MS_ABANDONED;
        unit_death_select(slot->index);
        frame.visual.entities += armor.entities;
        frame.visual.gibs += armor.gibs;
        frame.visual.fault = frame.visual.fault || armor.fault;
    }
    frame.death_animation = slot->death_animation;
    frame.death_pose = slot->host_pose;
    frame.state = slot->state;
    frame.monster_id = slot->host_captured || slot->state == MS_ACTIVE || slot->remote_dead
                           ? slot->index + 1u
                           : 0u;
    frame.host_epoch = slot->role == MONSTER_HOST ? slot->local_epoch : slot->host_epoch;
    frame.attacks = slot->attacks;
    frame.deaths = slot->deaths;
    frame.forms = slot->forms;
    frame.vid = slot->vid;
    frame.form_revision = slot->form_revision;
    frame.form_phase = slot->form_phase;
    frame.dropped =
        slot->dropped + (unsigned int)InterlockedCompareExchange(&incoming_dropped, 0, 0);
    frame.rejected = slot->rejected;
    frame.predictions = slot->predictions;
    frame.confirmations = slot->confirmations;
    frame.corrections = slot->corrections;
    if (now - slot->logged_at >= 500u || frame.cleanup)
    {
        frame.event = 1;
        slot->logged_at = now;
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
    for (unsigned int index = 0; index < slot_count; ++index)
    {
        MonsterSlot* slot = &slots[index];
        unit_death_select(index);
        ReceivedMonsterEvent matching[MONSTER_EVENT_CAPACITY];
        unsigned int count = 0;
        for (unsigned int j = 0; j < received_count; ++j)
        {
            if (received[j].message.event.monster_id == index + 1u)
            {
                matching[count++] = received[j];
            }
        }
        MonsterSyncFrame frame = tick_slot(slot, local, result, epoch, tick, now, &incoming[index],
                                           local_authority, matching, count);
        if (!index)
            output = frame;
        cleanup_all = cleanup_all && frame.cleanup;
        MonsterGallerySample* sample = &samples[index];
        sample->id = index + 1u;
        sample->vid = frame.vid;
        sample->state = frame.state;
        sample->form_revision = frame.form_revision;
        sample->form_phase = frame.form_phase;
        sample->presentation_vid = slot->presentation_vid;
        sample->prediction = (unsigned int)slot->prediction;
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
        dead += frame.state == MS_DEAD || slot->seen_deaths != 0;
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
    slot_count = 1u;
    gallery = 0;
    return 1;
}
