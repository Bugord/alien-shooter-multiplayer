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
    unsigned int index;      /* Position in slots[]; wire monster_id is index + 1. */
    UnitPuppet* puppet;      /* Borrowed native UNIT for this monster (same index). */
    UnitCombat* fixture;     /* Private native copy for the host gallery or a client. */
    UnitDeath* death_visual; /* Corpse/gib presentation of this monster's death. */
    UnitDeath* armor_visual; /* Armor debris; may outlive the live copy and the kill. */
    enum MonsterRole role;
    enum MonsterSyncState state;
    volatile LONG stopping;    /* Set by the worker; the game thread then cleans up. */
    volatile LONG stopped;     /* Set by the game thread once nothing is left in the world. */
    int host_captured;         /* Host: a native monster is being watched. */
    UnitPuppetPose host_pose;  /* Last live pose, or the death pose after a death. */
    uint32_t presentation_vid; /* Client: VID of the live copy shown now; 0 = none. */

    /* The local world this slot belongs to. */
    struct
    {
        int entered;
        uintptr_t game;
        uint32_t epoch;
        uint32_t low;
        uint32_t high;
        DWORD settled_at;
    } world;

    /* Client: the host world and peer session this slot follows. */
    struct
    {
        uint32_t host_epoch;
        uint32_t id;
        uint32_t session;
    } authority;

    struct
    {
        DWORD at;
        DWORD delay;
    } retry;

    /* Event sequencing; staged host events wait here for the worker. */
    struct
    {
        uint32_t state_sequence; /* Host: last sent state packet. */
        uint32_t event_sequence; /* Host: last staged event. */
        uint32_t last_received;  /* Client: last accepted event. */
        uint32_t seen_attacks;
        uint32_t seen_deaths;
        MpMonsterEvent staged[MONSTER_EVENT_CAPACITY];
        unsigned int staged_count;
    } events;

    /* Current authoritative form of the monster. */
    struct
    {
        uint32_t vid;
        uint32_t revision;
        uint32_t phase;
        uint32_t from; /* Host: the VID before the last form change. */
    } form;

    struct
    {
        int remote_dead;
        int presented;
        unsigned int animation;
        DWORD at;
    } death;

    /* Armor debris; presented_revision is 0 while none is shown. */
    struct
    {
        uint32_t presented_revision;
        DWORD at;
    } armor;

    /* Client: a local hit shown before the host confirms it. */
    struct
    {
        enum PredictionState state;
        DWORD at;
        uint32_t sequence;
        uint32_t vid;
        uint32_t hits;
        int health;
        unsigned int animation;
    } prediction;

    /* Diagnostic counters only; nothing above depends on them. */
    struct
    {
        unsigned int attacks;
        unsigned int deaths;
        unsigned int forms;
        unsigned int dropped;
        unsigned int rejected;
        unsigned int predictions;
        unsigned int confirmations;
        unsigned int corrections;
        DWORD logged_at;
    } stats;
} MonsterSlot;
static MonsterSlot slots[MP_MONSTER_CAPACITY];
/* Each monster uses two death slots: index for its death, index + capacity for armor. */
_Static_assert(UNIT_DEATH_SLOT_COUNT >= 2u * MP_MONSTER_CAPACITY, "two death slots per monster");
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
        slots[i].death_visual = unit_death_slot(i);
        slots[i].armor_visual = unit_death_slot(MP_MONSTER_CAPACITY + i);
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
        slots[i].retry.delay = MP_RETRY_FIRST_MS;
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
    if (slots[0].state != MS_WAITING || slots[0].world.entered)
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
    event.sequence = ++slot->events.event_sequence;
    if (!event.sequence)
        event.sequence = ++slot->events.event_sequence;
    event.world_low = slot->world.low;
    event.world_high = slot->world.high;
    event.world_epoch = slot->world.epoch;
    event.monster_id = slot->index + 1u;
    event.tick = tick;
    event.kind = kind;
    event.x = pose->x;
    event.y = pose->y;
    event.z = pose->z;
    event.direction = pose->direction;
    event.death_animation = kind == MP_MONSTER_DEATH ? slot->death.animation : 0;
    event.vid = slot->form.vid;
    event.origin_vid = expected_vid(slot->index);
    event.form_revision = slot->form.revision;
    event.form_phase = slot->form.phase;
    event.from_vid = kind == MP_MONSTER_FORM ? slot->form.from : 0u;
    if (kind == MP_MONSTER_FORM && slot->form.phase == MP_MONSTER_FORM_ARMOR)
        event.death_animation = STEAM_ANIM_DEATH;
    if (slot->events.staged_count < MONSTER_EVENT_CAPACITY)
        slot->events.staged[slot->events.staged_count++] = event;
    else
        ++slot->stats.dropped;
}
static int cleanup(MonsterSlot* slot, uintptr_t game)
{
    if (!unit_puppet_release(slot->puppet, game))
        return 0;
    if ((slot->role == MONSTER_CLIENT || gallery) && !unit_combat_remove(slot->fixture, game))
        return 0;
    if (!unit_death_remove(slot->death_visual, game))
        return 0;
    if (!unit_death_remove(slot->armor_visual, game))
        return 0;
    slot->presentation_vid = 0;
    slot->prediction.hits = 0;
    slot->armor.presented_revision = 0;
    slot->prediction.state = PREDICTION_NONE;
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
    slot->events.seen_attacks = 0;
    slot->events.seen_deaths = 0;
    slot->armor.presented_revision = 0;
    slot->state = MS_BACKOFF;
    slot->retry.at = now + slot->retry.delay;
    slot->retry.delay = mp_retry_next_delay(slot->retry.delay);
}
static int same_world(const MonsterSlot* slot, const MpMonsterState* s)
{
    return s->world_low == slot->world.low && s->world_high == slot->world.high && s->world_epoch &&
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
        slot->prediction.hits = 0;
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
    if (slot->armor.presented_revision)
        return 1;
    int removed = unit_death_remove(slot->armor_visual, game);
    enum UnitResult played =
        removed ? unit_death_play_armor(slot->armor_visual, game, pose, source_vid) : UNIT_REFUSED;
    if (!removed)
    {
        slot->state = MS_ABANDONED;
        return 0;
    }
    if (played != UNIT_OK)
    {
        schedule_retry(slot, game, now);
        return 0;
    }
    slot->armor.presented_revision = revision;
    slot->armor.at = now;
    return 1;
}
static void present_death(MonsterSlot* slot, uintptr_t game, const UnitPuppetPose* pose,
                          unsigned int animation, DWORD now)
{
    if (!slot->death.remote_dead)
        ++slot->stats.deaths;
    slot->death.remote_dead = 1;
    slot->death.animation = animation;
    slot->host_pose = *pose;
    if (slot->death.presented)
    {
        slot->state = MS_DEAD;
        return;
    }
    /* Armor debris has its own bounded visual slot and may outlive this kill. */
    if (!cleanup_live(slot, game) || !unit_death_remove(slot->death_visual, game))
    {
        slot->state = MS_ABANDONED;
        return;
    }
    InterlockedExchange(&slot->stopped, 0);
    if (unit_death_play_type(slot->death_visual, game, pose, animation, slot->form.vid) != UNIT_OK)
    {
        schedule_retry(slot, game, now);
        return;
    }
    slot->death.presented = 1;
    slot->death.at = now;
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
        if (unit_combat_spawn_predictive(slot->fixture, &spawn, vid, pose) != UNIT_OK)
        {
            schedule_retry(slot, game, now);
            return 0;
        }
        UnitCombatSample fixture_sample = unit_combat_sample(slot->fixture, game, 1);
        if (fixture_sample.fault || !fixture_sample.sources_unchanged ||
            unit_puppet_attach_entity(slot->puppet, game, local->player, fixture_sample.root) !=
                1 ||
            unit_puppet_prediction(slot->puppet, game) != UNIT_OK)
        {
            schedule_retry(slot, game, now);
            return 0;
        }
        slot->presentation_vid = vid;
        slot->prediction.hits = 0;
        slot->retry.delay = MP_RETRY_FIRST_MS;
    }
    slot->state = MS_ACTIVE;
    if (unit_puppet_apply(slot->puppet, game, pose) != UNIT_OK ||
        (health > 0 && unit_puppet_health(slot->puppet, game, health) != UNIT_OK))
    {
        schedule_retry(slot, game, now);
        return 0;
    }
    return 1;
}
static void forget_prediction(MonsterSlot* slot)
{
    slot->prediction.state = PREDICTION_NONE;
    slot->prediction.vid = 0;
}
static int correct_prediction(MonsterSlot* slot, uintptr_t game)
{
    if (!cleanup(slot, game))
    {
        slot->state = MS_ABANDONED;
        return 0;
    }
    slot->presentation_vid = 0;
    slot->prediction.hits = 0;
    slot->armor.presented_revision = 0;
    slot->death.presented = 0;
    slot->state = MS_WAITING;
    forget_prediction(slot);
    ++slot->stats.corrections;
    return 1;
}
/* A local hit killed the client copy: replace it with a provisional death
   effect until the host confirms or corrects it. Returns 1 shown, 0 failed. */
static int present_predicted_death(MonsterSlot* slot, uintptr_t game,
                                   const UnitPuppetSample* sample, DWORD now)
{
    slot->prediction.state = PREDICTION_DEATH;
    slot->prediction.animation = sample->death_animation;
    UnitPuppetPose pose = sample->death_pose;
    if (!cleanup_live(slot, game) || !unit_death_remove(slot->death_visual, game))
    {
        slot->state = MS_ABANDONED;
        return 0;
    }
    if (unit_death_play_type(slot->death_visual, game, &pose, sample->death_animation,
                             slot->prediction.vid) != UNIT_OK)
    {
        schedule_retry(slot, game, now);
        return 0;
    }
    slot->death.at = now;
    slot->host_pose = pose;
    slot->state = MS_PREDICTED_DEAD;
    return 1;
}

static int predict_hit(MonsterSlot* slot, uintptr_t game, const MpMonsterState* source, DWORD now)
{
    if (!slot->presentation_vid || slot->prediction.state == PREDICTION_DEATH)
        return 1;
    UnitPuppetSample sample = unit_puppet_sample(slot->puppet, game);
    if (sample.state == UNIT_PUPPET_FAULT)
    {
        schedule_retry(slot, game, now);
        return 0;
    }
    if (sample.predicted_hits == slot->prediction.hits)
        return 1;
    slot->prediction.hits = sample.predicted_hits;
    if (slot->prediction.state == PREDICTION_NONE)
    {
        slot->prediction.at = now;
        slot->prediction.sequence = source->sequence;
        slot->prediction.state = PREDICTION_HEALTH;
        ++slot->stats.predictions;
    }
    slot->prediction.vid = sample.vid ? sample.vid : slot->presentation_vid;
    slot->prediction.health = sample.health;
    if (sample.transitioning)
    {
        slot->prediction.state = PREDICTION_FORM;
        slot->prediction.vid = mp_monster_armor_successor(sample.vid);
        slot->prediction.health = 0; /* Native successor constructor supplies HP. */
    }
    else if (sample.captured_deaths)
    {
        return present_predicted_death(slot, game, &sample, now);
    }
    else if (sample.vid != slot->presentation_vid)
    {
        /* Native freeze already changed our private descriptor atomically. */
        slot->prediction.state = PREDICTION_FORM;
        slot->presentation_vid = sample.vid;
    }
    else if (slot->prediction.state == PREDICTION_FORM)
    {
        /* A later hit on the successor/frozen form needs its own HP result;
           merely receiving the form transition cannot acknowledge that hit. */
        slot->prediction.state = PREDICTION_HEALTH;
    }
    return 1;
}
/* Inputs of one slot tick, gathered once per frame by monster_sync_tick. */
typedef struct SlotTickInput
{
    const Snapshot* local;
    enum ProbeResult result;
    uint32_t epoch;
    uint32_t tick;
    DWORD now;
    const MonsterState* published;
    int local_authority;
    const ReceivedMonsterEvent* received;
    unsigned int received_count;
} SlotTickInput;

/* Result of one tick step: run the next step, or skip straight to the report. */
enum TickStep
{
    TICK_CONTINUE,
    TICK_REPORT
};

static uintptr_t input_game(const SlotTickInput* input)
{
    return input->local ? input->local->game : 0;
}

/* Removes everything when the coordinator asked this slot to stop. */
static void stop_slot(MonsterSlot* slot, uintptr_t game, MonsterSyncFrame* frame)
{
    slot->state = MS_STOPPING;
    frame->cleanup = cleanup(slot, game);
    if (frame->cleanup)
        InterlockedExchange(&slot->stopped, 1);
}

/* Forgets the previous world, and enters the current one, when the map key,
   world epoch or level state changed since the last tick. */
static enum TickStep reset_world(MonsterSlot* slot, const SlotTickInput* input)
{
    const Snapshot* local = input->local;
    enum ProbeResult result = input->result;
    uintptr_t game = input_game(input);
    int valid_world =
        local && (result == PROBE_OK || result == PROBE_NO_PLAYER || result == PROBE_NO_ARMY) &&
        local->in_level && input->epoch;
    if (valid_world && slot->world.entered && slot->world.epoch == input->epoch &&
        local->world_low == slot->world.low && local->world_high == slot->world.high)
        return TICK_CONTINUE;

    if (!cleanup(slot, game))
    {
        slot->state = MS_ABANDONED;
        return TICK_REPORT;
    }

    slot->world.entered = 0;
    slot->host_captured = 0;
    slot->events.staged_count = 0;
    slot->events.seen_attacks = 0;
    slot->events.seen_deaths = 0;
    slot->events.event_sequence = 0;
    slot->events.last_received = 0;
    slot->authority.host_epoch = 0;
    slot->authority.session = 0;
    slot->death.remote_dead = 0;
    slot->death.presented = 0;
    slot->form.vid = expected_vid(slot->index);
    slot->form.revision = 1u;
    slot->form.phase = MP_MONSTER_FORM_ACTIVE;
    slot->form.from = slot->form.vid;
    slot->armor.presented_revision = 0;
    slot->death.animation = 0;
    memset(&slot->host_pose, 0, sizeof(slot->host_pose));
    slot->state = MS_WAITING;
    slot->retry.delay = MP_RETRY_FIRST_MS;
    if (!valid_world)
        return TICK_REPORT;

    slot->world.game = game;
    slot->world.epoch = input->epoch;
    slot->world.low = local->world_low;
    slot->world.high = local->world_high;
    slot->world.settled_at = input->now;
    slot->world.entered = 1;
    return TICK_CONTINUE;
}

/* Holds an abandoned slot, a slot in retry backoff and a world that has not
   settled yet. A backoff that has elapsed goes back to waiting. */
static enum TickStep wait_for_retry(MonsterSlot* slot, DWORD now)
{
    if (slot->state == MS_ABANDONED)
        return TICK_REPORT;
    if (slot->state == MS_BACKOFF && (int32_t)(now - slot->retry.at) < 0)
        return TICK_REPORT;
    if (slot->state == MS_BACKOFF)
        slot->state = MS_WAITING;
    if (now - slot->world.settled_at < MONSTER_SETTLE_MS)
        return TICK_REPORT;
    return TICK_CONTINUE;
}

/* Host lost authority: remove the watched monster and wait for authority. */
static void release_host(MonsterSlot* slot, uintptr_t game)
{
    if (!cleanup(slot, game))
    {
        slot->state = MS_ABANDONED;
        return;
    }
    slot->state = MS_WAITING;
    slot->host_captured = 0;
    slot->events.seen_attacks = 0;
    slot->events.seen_deaths = 0;
}

/* Gallery host: spawns a stationary native monster of this slot's type in a
   grid next to the player and watches it. Returns UNIT_OK when watched. */
static enum UnitResult spawn_gallery_monster(MonsterSlot* slot, const Snapshot* local)
{
    uintptr_t game = local->game;
    Snapshot spawn = *local;
    spawn.health = 1;
    UnitPuppetPose position = {local->x + 160.0f + (slot->index % 8u) * 80.0f,
                               local->y - 150.0f + (slot->index / 8u) * 100.0f,
                               local->z,
                               0,
                               128u,
                               0};
    enum UnitResult created =
        unit_combat_spawn_type(slot->fixture, &spawn, expected_vid(slot->index), &position, 1);
    if (created != UNIT_OK)
        return UNIT_FAULT;

    UnitCombatSample fixture_sample = unit_combat_sample(slot->fixture, game, 0);
    if (fixture_sample.fault)
        return UNIT_FAULT;
    return unit_puppet_watch_stationary(slot->puppet, game, local->player, fixture_sample.root);
}

/* Starts watching a native monster on the host, once per world. */
static enum TickStep capture_host_monster(MonsterSlot* slot, const SlotTickInput* input)
{
    const Snapshot* local = input->local;
    uintptr_t game = input_game(input);
    if (slot->host_captured)
        return TICK_CONTINUE;
    if (!local->player)
        return TICK_REPORT;

    enum UnitResult attached;
    if (gallery)
        attached = spawn_gallery_monster(slot, local);
    else
        attached = unit_puppet_watch_first(slot->puppet, game, local->player);
    if (attached == UNIT_FAULT)
    {
        schedule_retry(slot, game, input->now);
        return TICK_REPORT;
    }
    if (attached == UNIT_REFUSED)
        return TICK_REPORT;

    slot->host_captured = 1;
    slot->state = MS_ACTIVE;
    InterlockedExchange(&slot->stopped, 0);
    return TICK_CONTINUE;
}

/* Gallery host: follows the fixture's root after a native form change created
   a new entity, and takes the fixture's form state into the sample. */
static enum TickStep sample_gallery_fixture(MonsterSlot* slot, const SlotTickInput* input,
                                            MonsterSyncFrame* frame)
{
    uintptr_t game = input_game(input);
    UnitCombatSample fixture_sample = unit_combat_sample(slot->fixture, game, 0);
    if (fixture_sample.fault)
    {
        schedule_retry(slot, game, input->now);
        return TICK_REPORT;
    }

    if (!fixture_sample.transitioning && fixture_sample.root &&
        fixture_sample.root != frame->unit.entity && !slot->events.seen_deaths)
    {
        if (!unit_puppet_release(slot->puppet, game) ||
            unit_puppet_watch_stationary(slot->puppet, game, input->local->player,
                                         fixture_sample.root) != UNIT_OK)
        {
            schedule_retry(slot, game, input->now);
            return TICK_REPORT;
        }
        slot->events.seen_attacks = 0;
        slot->events.seen_deaths = 0;
        frame->unit = unit_puppet_sample(slot->puppet, game);
    }

    frame->unit.transitioning = fixture_sample.transitioning || frame->unit.transitioning;
    if (fixture_sample.current_vid)
        frame->unit.vid = fixture_sample.current_vid;
    return TICK_CONTINUE;
}

/* Samples the watched native monster into frame->unit and keeps its live pose. */
static enum TickStep sample_host_monster(MonsterSlot* slot, const SlotTickInput* input,
                                         MonsterSyncFrame* frame)
{
    uintptr_t game = input_game(input);
    frame->unit = unit_puppet_sample(slot->puppet, game);
    if (gallery && sample_gallery_fixture(slot, input, frame) == TICK_REPORT)
        return TICK_REPORT;
    if (frame->unit.state == UNIT_PUPPET_FAULT)
    {
        schedule_retry(slot, game, input->now);
        return TICK_REPORT;
    }

    if (frame->unit.entity && !slot->events.seen_deaths)
    {
        slot->host_pose.x = frame->unit.x;
        slot->host_pose.y = frame->unit.y;
        slot->host_pose.z = frame->unit.z;
        slot->host_pose.direction = frame->unit.direction;
        slot->host_pose.velocity = frame->unit.velocity;
        slot->host_pose.moving = frame->unit.moving;
    }
    return TICK_CONTINUE;
}

/* Host: records a native form change (armor loss, freeze) as a new form
   revision and stages a form event. *phase receives the current form phase. */
static enum TickStep update_host_form(MonsterSlot* slot, const SlotTickInput* input,
                                      const MonsterSyncFrame* frame, uint32_t* phase)
{
    uintptr_t game = input_game(input);
    uint32_t current_vid = frame->unit.vid ? frame->unit.vid : slot->form.vid;
    *phase = frame->unit.transitioning ? MP_MONSTER_FORM_ARMOR : MP_MONSTER_FORM_ACTIVE;

    /* Medium armor already emits native debris. Large armor has no stock
       fragments, so give the host the same separate visual as its clients. */
    if ((current_vid == MONSTER_VID_TANK_ARMORED && *phase == MP_MONSTER_FORM_ARMOR) ||
        (slot->form.vid == MONSTER_VID_TANK_ARMORED &&
         (current_vid == MONSTER_VID_TANK ||
          current_vid == MONSTER_VID_TANK + MONSTER_VID_FROZEN_DELTA)))
    {
        if (!present_armor(slot, game, &slot->host_pose, MONSTER_VID_TANK_ARMORED,
                           slot->form.revision, input->now))
            return TICK_REPORT;
    }

    if (current_vid != slot->form.vid || *phase != slot->form.phase)
    {
        slot->form.from = slot->form.vid;
        slot->form.vid = current_vid;
        slot->form.phase = *phase;
        ++slot->form.revision;
        ++slot->stats.forms;
        stage_event(slot, MP_MONSTER_FORM, input->tick, &slot->host_pose);
    }
    slot->state = *phase == MP_MONSTER_FORM_ARMOR ? MS_TRANSFORMING : MS_ACTIVE;
    return TICK_CONTINUE;
}

/* Host: stages attack and death events from the native action counters. */
static void stage_host_events(MonsterSlot* slot, const MonsterSyncFrame* frame, uint32_t phase,
                              uint32_t tick)
{
    /* Counters come from native action callbacks, not animation snapshots. */
    unsigned int count = frame->unit.captured_attacks - slot->events.seen_attacks;
    if (count > MONSTER_EVENT_CAPACITY)
    {
        slot->stats.dropped += count - MONSTER_EVENT_CAPACITY;
        count = MONSTER_EVENT_CAPACITY;
    }
    for (unsigned int i = 0; i < count; ++i)
    {
        stage_event(slot, MP_MONSTER_ATTACK, tick, &slot->host_pose);
        ++slot->stats.attacks;
    }
    slot->events.seen_attacks = frame->unit.captured_attacks;

    if (!phase && !slot->events.seen_deaths && (frame->unit.captured_deaths || !frame->unit.entity))
    {
        slot->death.animation = frame->unit.death_animation == STEAM_ANIM_GIB_DEATH
                                    ? STEAM_ANIM_GIB_DEATH
                                    : STEAM_ANIM_DEATH;
        if (frame->unit.death_animation)
            slot->host_pose = frame->unit.death_pose;
        slot->host_pose.velocity = 0;
        slot->host_pose.moving = 0;
        stage_event(slot, MP_MONSTER_DEATH, tick, &slot->host_pose);
        ++slot->stats.deaths;
        slot->events.seen_deaths = frame->unit.captured_deaths ? frame->unit.captured_deaths : 1u;
    }
}

/* Host: builds the state packet that describes this monster now. */
static MpMonsterState build_host_state(MonsterSlot* slot, const SlotTickInput* input,
                                       const MonsterSyncFrame* frame, uint32_t phase)
{
    MpMonsterState output = {0};
    output.sequence = ++slot->events.state_sequence;
    output.world_low = slot->world.low;
    output.world_high = slot->world.high;
    output.world_epoch = input->epoch;
    output.monster_id = slot->index + 1u;
    output.vid = slot->form.vid;
    output.tick = input->tick;
    output.origin_vid = expected_vid(slot->index);
    output.form_revision = slot->form.revision;
    output.form_phase = phase;
    output.alive = phase || (!slot->events.seen_deaths && frame->unit.entity &&
                             frame->unit.animation < STEAM_ANIM_DEATH && frame->unit.health > 0);
    output.health = phase ? 1 : output.alive ? frame->unit.health : 0;
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
    output.death_animation = output.alive ? 0 : slot->death.animation;
    output.event_sequence = slot->events.event_sequence;
    return output;
}

/* Host: hands the state and the staged events to the worker. Skips this frame
   when the worker holds the lock; staged events wait for the next frame. */
static void publish_host_state(MonsterSlot* slot, const MpMonsterState* output)
{
    if (!TryAcquireSRWLockExclusive(&lock))
        return;

    handoff.outgoing[slot->index] = *output;
    handoff.has_outgoing[slot->index] = 1;
    unsigned int sent = 0;
    while (sent < slot->events.staged_count && handoff.count < MONSTER_EVENT_CAPACITY)
    {
        handoff.events[(handoff.head + handoff.count++) % MONSTER_EVENT_CAPACITY] =
            slot->events.staged[sent++];
    }
    if (sent)
    {
        slot->events.staged_count -= sent;
        memmove(slot->events.staged, slot->events.staged + sent,
                slot->events.staged_count * sizeof(*slot->events.staged));
    }
    ReleaseSRWLockExclusive(&lock);
}

/* Host side of one tick: watch the native monster and publish its state. */
static void tick_host(MonsterSlot* slot, const SlotTickInput* input, MonsterSyncFrame* frame)
{
    if (!input->local_authority)
    {
        release_host(slot, input_game(input));
        return;
    }
    if (capture_host_monster(slot, input) == TICK_REPORT)
        return;
    if (sample_host_monster(slot, input, frame) == TICK_REPORT)
        return;

    uint32_t phase;
    if (update_host_form(slot, input, frame, &phase) == TICK_REPORT)
        return;
    stage_host_events(slot, frame, phase, input->tick);
    MpMonsterState output = build_host_state(slot, input, frame, phase);
    publish_host_state(slot, &output);
}

/* Client: starts over when the host changed its world epoch or the authority
   (peer or session) changed. */
static enum TickStep follow_authority(MonsterSlot* slot, uintptr_t game,
                                      const MonsterState* incoming)
{
    const MpMonsterState* source = &incoming->state;
    if (slot->authority.host_epoch == source->world_epoch &&
        slot->authority.id == incoming->authority_id &&
        slot->authority.session == incoming->authority_session)
        return TICK_CONTINUE;

    if (!cleanup(slot, game))
    {
        slot->state = MS_ABANDONED;
        return TICK_REPORT;
    }
    slot->authority.host_epoch = source->world_epoch;
    slot->authority.id = incoming->authority_id;
    slot->authority.session = incoming->authority_session;
    slot->state = MS_WAITING;
    slot->death.remote_dead = 0;
    slot->death.presented = 0;
    slot->death.animation = 0;
    slot->form.vid = 0;
    slot->form.revision = 0;
    slot->form.phase = 0;
    slot->armor.presented_revision = 0;
    slot->events.last_received = 0;
    return TICK_CONTINUE;
}

/* A late old form must not replace a newer allocation or a tombstone.
   The transport also validates progression; retain this game-thread
   boundary for events and stale handoff snapshots already in flight. */
static int form_is_stale(const MonsterSlot* slot, const MpMonsterState* source)
{
    if (!slot->form.revision)
        return 0;
    if (source->form_revision < slot->form.revision)
        return 1;
    return source->form_revision == slot->form.revision &&
           (source->vid != slot->form.vid || source->form_phase != slot->form.phase);
}

/* Death events can confirm a provisional effect before the next state
   snapshot. Folds the first matching death event into incoming so both
   deliveries use the same reconciliation path. */
static void fold_death_event(MonsterSlot* slot, const SlotTickInput* input, MonsterState* incoming,
                             unsigned int* considered)
{
    const MpMonsterState* source = &incoming->state;
    for (unsigned int i = 0; i < input->received_count; ++i)
    {
        const MonsterEvent* event = &input->received[i].message;
        const MpMonsterEvent* e = &event->event;
        if (e->kind != MP_MONSTER_DEATH || input->now - input->received[i].received_at > 250u ||
            event->authority_id != incoming->authority_id ||
            event->authority_session != incoming->authority_session ||
            e->world_epoch != source->world_epoch || e->world_low != slot->world.low ||
            e->world_high != slot->world.high || e->monster_id != slot->index + 1u ||
            !mp_monster_event_matches(source, e) ||
            !mp_sequence_newer(e->sequence, slot->events.last_received))
            continue;

        slot->events.last_received = e->sequence;
        ++*considered;
        incoming->state.alive = 0;
        incoming->state.health = 0;
        incoming->state.death_animation = e->death_animation;
        incoming->state.x = e->x;
        incoming->state.y = e->y;
        incoming->state.z = e->z;
        incoming->state.direction = e->direction;
        break;
    }
}

/* Client: confirms or corrects a local hit prediction against the host state. */
static enum TickStep reconcile_prediction(MonsterSlot* slot, uintptr_t game,
                                          const MpMonsterState* source, DWORD now)
{
    if (slot->prediction.state == PREDICTION_NONE)
        return TICK_CONTINUE;

    if (!source->alive)
    {
        if (slot->prediction.state == PREDICTION_DEATH && slot->prediction.vid == source->vid &&
            slot->prediction.animation == source->death_animation)
        {
            /* Promote the existing provisional effect, without replaying it. */
            slot->death.presented = 1;
            ++slot->stats.confirmations;
        }
        else if (!correct_prediction(slot, game))
            return TICK_REPORT;
        forget_prediction(slot);
    }
    else if (slot->prediction.state != PREDICTION_DEATH && !source->form_phase &&
             source->vid == slot->prediction.vid &&
             mp_sequence_newer(source->sequence, slot->prediction.sequence) &&
             ((slot->prediction.state == PREDICTION_FORM &&
               source->form_revision > slot->form.revision) ||
              (slot->prediction.health > 0 && source->health <= slot->prediction.health)))
    {
        forget_prediction(slot);
        ++slot->stats.confirmations;
    }
    else if (source->form_revision > slot->form.revision && !source->form_phase &&
             source->vid != slot->prediction.vid)
    {
        if (!correct_prediction(slot, game))
            return TICK_REPORT;
    }
    else if (now - slot->prediction.at >= MONSTER_PREDICTION_MS &&
             mp_sequence_newer(source->sequence, slot->prediction.sequence))
    {
        if (!correct_prediction(slot, game))
            return TICK_REPORT;
    }
    return TICK_CONTINUE;
}

/* True when an armored form lost its armor and became its successor form. */
static int lost_armor(unsigned int previous_vid, unsigned int vid)
{
    if (previous_vid == MONSTER_VID_MONSTER2_ARMORED)
        return vid == MONSTER_VID_MONSTER2 ||
               vid == MONSTER_VID_MONSTER2 + MONSTER_VID_FROZEN_DELTA;
    if (previous_vid == MONSTER_VID_TANK_ARMORED)
        return vid == MONSTER_VID_TANK || vid == MONSTER_VID_TANK + MONSTER_VID_FROZEN_DELTA;
    return 0;
}

/* Client: takes a new form revision from the host state. */
static enum TickStep apply_form_revision(MonsterSlot* slot, uintptr_t game,
                                         const MpMonsterState* source, DWORD now)
{
    unsigned int previous_vid = slot->form.vid;
    unsigned int previous_phase = slot->form.phase;
    if (source->form_revision == slot->form.revision)
        return TICK_CONTINUE;

    slot->form.vid = source->vid;
    slot->form.revision = source->form_revision;
    slot->form.phase = source->form_phase;
    if (previous_vid)
        ++slot->stats.forms;

    /* Recover an omitted armor-pending packet while the successor is
       already current. A late join starts at the current form. */
    if (previous_vid && !previous_phase && lost_armor(previous_vid, source->vid))
    {
        UnitPuppetPose armor_pose = {source->x, source->y, source->z, 0, source->direction, 0};
        if (!slot->armor.presented_revision &&
            (!cleanup_live(slot, game) ||
             !present_armor(slot, game, &armor_pose, previous_vid, source->form_revision, now)))
            return TICK_REPORT;
    }
    return TICK_CONTINUE;
}

/* Client: the host reports this monster dead; show (or keep) its death. */
static void show_remote_death(MonsterSlot* slot, uintptr_t game, const MpMonsterState* source,
                              MonsterSyncFrame* frame, DWORD now)
{
    UnitPuppetPose pose = {source->x, source->y, source->z, 0, source->direction, 0};
    present_death(slot, game, &pose, source->death_animation, now);
    if (slot->state != MS_DEAD)
        return;

    frame->visual = unit_death_sample(slot->death_visual, game);
    if (frame->visual.fault || (now - slot->death.at > MONSTER_PRESENTATION_LIFETIME_MS &&
                                !unit_death_remove(slot->death_visual, game)))
        slot->state = MS_ABANDONED;
}

/* True when a predicted form change is an armored monster losing its armor. */
static int predicted_armor_loss(const MonsterSlot* slot, unsigned int view_vid)
{
    if (slot->prediction.state != PREDICTION_FORM)
        return 0;
    return (slot->presentation_vid == MONSTER_VID_MONSTER2_ARMORED &&
            view_vid == MONSTER_VID_MONSTER2) ||
           (slot->presentation_vid == MONSTER_VID_TANK_ARMORED && view_vid == MONSTER_VID_TANK);
}

/* Client: shows the live monster copy in the form, pose and health that the
   host state (or a pending local prediction) describes. */
static enum TickStep show_client_copy(MonsterSlot* slot, const SlotTickInput* input,
                                      const MpMonsterState* source)
{
    uintptr_t game = input_game(input);
    DWORD now = input->now;
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
    if (slot->prediction.state == PREDICTION_DEATH)
    {
        slot->state = MS_PREDICTED_DEAD;
        return TICK_REPORT;
    }
    if (slot->prediction.state != PREDICTION_NONE)
    {
        view_vid = slot->prediction.vid;
        view_health = slot->prediction.health;
        if (mp_monster_frozen(view_vid))
        {
            pose.velocity = 0;
            pose.moving = 0;
        }
    }

    if ((source->form_phase == MP_MONSTER_FORM_ARMOR || predicted_armor_loss(slot, view_vid)) &&
        !slot->armor.presented_revision)
    {
        unsigned int armored = source->origin_vid;
        if (!cleanup_live(slot, game) ||
            !present_armor(slot, game, &pose, armored, source->form_revision, now))
            return TICK_REPORT;
    }

    int spawning = !slot->presentation_vid;
    if (!client_copy(slot, input->local, view_vid, &pose, view_health, now))
        return TICK_REPORT;
    if (spawning)
        slot->events.last_received = source->event_sequence;
    if (slot->prediction.state != PREDICTION_NONE && !slot->prediction.health)
        slot->prediction.health = unit_puppet_sample(slot->puppet, game).health;
    if (source->form_phase == MP_MONSTER_FORM_ARMOR)
        slot->state = MS_TRANSFORMING;
    return TICK_CONTINUE;
}

/* True when a received event belongs to this monster's current authority,
   world and form, and is newer than the last accepted event. */
static int event_matches_slot(const MonsterSlot* slot, const ReceivedMonsterEvent* received,
                              DWORD now)
{
    const MonsterEvent* event = &received->message;
    const MpMonsterEvent* e = &event->event;
    return now - received->received_at <= 250u && event->authority_id == slot->authority.id &&
           event->authority_session == slot->authority.session &&
           e->world_epoch == slot->authority.host_epoch && e->world_low == slot->world.low &&
           e->world_high == slot->world.high && e->monster_id == slot->index + 1u &&
           e->vid == slot->form.vid && e->origin_vid == expected_vid(slot->index) &&
           e->form_revision == slot->form.revision && e->form_phase == slot->form.phase &&
           mp_sequence_newer(e->sequence, slot->events.last_received);
}

/* Client: replays received attack events and presents a received death.
   Every received event counts as considered. */
static void replay_events(MonsterSlot* slot, const SlotTickInput* input,
                          const MpMonsterState* source, unsigned int* considered)
{
    uintptr_t game = input_game(input);
    DWORD now = input->now;
    for (unsigned int i = 0; i < input->received_count; ++i)
    {
        ++*considered;
        if (!event_matches_slot(slot, &input->received[i], now))
        {
            ++slot->stats.rejected;
            continue;
        }

        const MpMonsterEvent* e = &input->received[i].message.event;
        slot->events.last_received = e->sequence;
        /* Persistent state carries health and the complete current form.
           Form events are a deduplicated watermark; they never guess HP. */
        if (e->kind == MP_MONSTER_FORM)
            continue;
        if (e->kind == MP_MONSTER_DEATH)
        {
            UnitPuppetPose death_pose = {e->x, e->y, e->z, 0, e->direction, 0};
            if (slot->prediction.state != PREDICTION_NONE && !correct_prediction(slot, game))
                break;
            present_death(slot, game, &death_pose, e->death_animation, now);
            break;
        }
        if (e->kind != MP_MONSTER_ATTACK)
        {
            ++slot->stats.rejected;
            continue;
        }

        /* An old-form attack is still an acknowledged event, but its art
           must not replace a predicted frozen/successor presentation. */
        if (slot->presentation_vid != slot->form.vid || source->form_phase)
            continue;
        if (unit_puppet_replay(slot->puppet, game, e->sequence, STEAM_ANIM_ATTACK) != UNIT_OK)
        {
            schedule_retry(slot, game, now);
            break;
        }
        ++slot->stats.attacks;
    }
}

/* Client side of one tick: follow the host's state and events for this monster. */
static void tick_client(MonsterSlot* slot, const SlotTickInput* input, MonsterSyncFrame* frame,
                        unsigned int* considered)
{
    uintptr_t game = input_game(input);
    DWORD now = input->now;
    MonsterState incoming = *input->published;
    const MpMonsterState* source = &incoming.state;
    if (!incoming.present || !same_world(slot, source))
    {
        if (!cleanup(slot, game))
            slot->state = MS_ABANDONED;
        else
            slot->state = MS_WAITING;
        return;
    }
    if (follow_authority(slot, game, &incoming) == TICK_REPORT)
        return;
    if (form_is_stale(slot, source))
    {
        ++slot->stats.rejected;
        frame->unit = unit_puppet_sample(slot->puppet, game);
        return;
    }
    if (slot->death.remote_dead && source->alive)
        return;

    if (!slot->death.remote_dead && source->alive && !source->form_phase)
        fold_death_event(slot, input, &incoming, considered);
    if (!slot->death.remote_dead && !predict_hit(slot, game, source, now))
        return;
    if (reconcile_prediction(slot, game, source, now) == TICK_REPORT)
        return;
    if (apply_form_revision(slot, game, source, now) == TICK_REPORT)
        return;
    if (!source->alive)
    {
        show_remote_death(slot, game, source, frame, now);
        return;
    }
    if (slot->death.remote_dead)
        return;
    if (show_client_copy(slot, input, source) == TICK_REPORT)
        return;

    replay_events(slot, input, source, considered);
    frame->unit = unit_puppet_sample(slot->puppet, game);
    if (slot->state == MS_ACTIVE && frame->unit.state != UNIT_PUPPET_ACTIVE)
        schedule_retry(slot, game, now);
}

/* Adds the armor-debris presentation to the frame and expires it. */
static void report_armor(MonsterSlot* slot, uintptr_t game, DWORD now, MonsterSyncFrame* frame)
{
    if (!slot->armor.presented_revision)
        return;

    UnitDeathSample armor = unit_death_sample(slot->armor_visual, game);
    int expired = now - slot->armor.at > MONSTER_PRESENTATION_LIFETIME_MS;
    if (armor.fault || (expired && !unit_death_remove(slot->armor_visual, game)))
        slot->state = MS_ABANDONED;
    frame->visual.entities += armor.entities;
    frame->visual.gibs += armor.gibs;
    frame->visual.fault = frame->visual.fault || armor.fault;
}

/* Fills the frame report from the slot after every tick step that ran. */
static void report_frame(MonsterSlot* slot, const SlotTickInput* input, MonsterSyncFrame* frame,
                         unsigned int considered)
{
    uintptr_t game = input_game(input);
    DWORD now = input->now;
    slot->stats.rejected += input->received_count - considered;
    if (slot->role == MONSTER_CLIENT)
        frame->visual = unit_death_sample(slot->death_visual, game);
    report_armor(slot, game, now, frame);

    frame->death_animation = slot->death.animation;
    frame->death_pose = slot->host_pose;
    frame->state = slot->state;
    frame->monster_id = slot->host_captured || slot->state == MS_ACTIVE || slot->death.remote_dead
                            ? slot->index + 1u
                            : 0u;
    frame->host_epoch = slot->role == MONSTER_HOST ? slot->world.epoch : slot->authority.host_epoch;
    frame->attacks = slot->stats.attacks;
    frame->deaths = slot->stats.deaths;
    frame->forms = slot->stats.forms;
    frame->vid = slot->form.vid;
    frame->form_revision = slot->form.revision;
    frame->form_phase = slot->form.phase;
    frame->dropped =
        slot->stats.dropped + (unsigned int)InterlockedCompareExchange(&incoming_dropped, 0, 0);
    frame->rejected = slot->stats.rejected;
    frame->predictions = slot->stats.predictions;
    frame->confirmations = slot->stats.confirmations;
    frame->corrections = slot->stats.corrections;
    if (now - slot->stats.logged_at >= 500u || frame->cleanup)
    {
        frame->event = 1;
        slot->stats.logged_at = now;
    }
}

/* Runs the tick steps in order until one of them asks for the report. */
static void run_tick_steps(MonsterSlot* slot, const SlotTickInput* input, MonsterSyncFrame* frame,
                           unsigned int* considered)
{
    if (InterlockedCompareExchange(&slot->stopping, 0, 0))
    {
        stop_slot(slot, input_game(input), frame);
        return;
    }
    if (reset_world(slot, input) == TICK_REPORT)
        return;
    if (wait_for_retry(slot, input->now) == TICK_REPORT)
        return;

    if (slot->role == MONSTER_HOST)
        tick_host(slot, input, frame);
    else
        tick_client(slot, input, frame, considered);
}

/* One game-thread tick of one monster slot. */
static MonsterSyncFrame tick_slot(MonsterSlot* slot, const SlotTickInput* input)
{
    MonsterSyncFrame frame = {0};
    frame.role = slot->role;
    if (slot->state == MS_OFF)
        return frame;

    unsigned int considered = 0;
    run_tick_steps(slot, input, &frame, &considered);
    report_frame(slot, input, &frame, considered);
    return frame;
}
/* Inputs that monster_sync_tick takes from the worker handoff in one lock. */
typedef struct TickHandoff
{
    MonsterState incoming[MP_MONSTER_CAPACITY];
    int local_authority;
    ReceivedMonsterEvent received[MONSTER_EVENT_CAPACITY];
    unsigned int received_count;
} TickHandoff;

/* Gallery totals summed over every slot of one tick. */
typedef struct GalleryTotals
{
    unsigned int active;
    unsigned int dead;
    unsigned int faults;
    unsigned int deaths;
    unsigned int attacks;
    unsigned int predictions;
    unsigned int confirmations;
    unsigned int corrections;
} GalleryTotals;

/* Copies fresh incoming states and drains received events. Returns 0 when the
   worker holds the lock; the game thread never waits for it. */
static int take_tick_handoff(TickHandoff* out, DWORD now)
{
    if (!TryAcquireSRWLockExclusive(&lock))
        return 0;
    for (unsigned int i = 0; i < slot_count; ++i)
    {
        if (now - handoff.incoming_at[i] <= 1000u)
            out->incoming[i] = handoff.incoming[i];
    }
    out->local_authority = handoff.local_authority;
    out->received_count = handoff.received_count;
    memcpy(out->received, handoff.received, out->received_count * sizeof(*out->received));
    handoff.received_count = 0;
    ReleaseSRWLockExclusive(&lock);
    return 1;
}

/* Copies the received events addressed to one monster. Returns their count. */
static unsigned int select_slot_events(const TickHandoff* taken, unsigned int index,
                                       ReceivedMonsterEvent* matching)
{
    unsigned int count = 0;
    for (unsigned int j = 0; j < taken->received_count; ++j)
    {
        if (taken->received[j].message.event.monster_id == index + 1u)
            matching[count++] = taken->received[j];
    }
    return count;
}

static void fill_gallery_sample(MonsterGallerySample* sample, const MonsterSlot* slot,
                                const MonsterSyncFrame* frame)
{
    sample->id = slot->index + 1u;
    sample->vid = frame->vid;
    sample->state = frame->state;
    sample->form_revision = frame->form_revision;
    sample->form_phase = frame->form_phase;
    sample->presentation_vid = slot->presentation_vid;
    sample->prediction = (unsigned int)slot->prediction.state;
    sample->entity = frame->unit.entity;
    sample->health = frame->unit.health;
    sample->animation = frame->unit.animation;
    sample->behavior = frame->unit.behavior;
    sample->death_animation = frame->death_animation;
    sample->x = frame->unit.entity ? frame->unit.x : frame->death_pose.x;
    sample->y = frame->unit.entity ? frame->unit.y : frame->death_pose.y;
    sample->effects = frame->visual.entities;
    sample->fault = frame->visual.fault;
}

static void add_gallery_totals(GalleryTotals* totals, const MonsterSlot* slot,
                               const MonsterSyncFrame* frame)
{
    totals->active +=
        (frame->state == MS_ACTIVE || frame->state == MS_TRANSFORMING) && frame->unit.entity != 0;
    totals->dead += frame->state == MS_DEAD || slot->events.seen_deaths != 0;
    totals->faults +=
        frame->state == MS_ABANDONED || frame->state == MS_BACKOFF || frame->visual.fault;
    totals->deaths += frame->deaths;
    totals->attacks += frame->attacks;
    totals->predictions += frame->predictions;
    totals->confirmations += frame->confirmations;
    totals->corrections += frame->corrections;
}

MonsterSyncFrame monster_sync_tick(const Snapshot* local, enum ProbeResult result, uint32_t epoch,
                                   uint32_t tick, DWORD now)
{
    MonsterSyncFrame output = {0};
    TickHandoff taken = {0};
    if (!take_tick_handoff(&taken, now))
    {
        output.state = slots[0].state;
        return output;
    }

    MonsterGallerySample samples[MP_MONSTER_CAPACITY] = {0};
    GalleryTotals totals = {0};
    int cleanup_all = 1;
    for (unsigned int index = 0; index < slot_count; ++index)
    {
        MonsterSlot* slot = &slots[index];
        ReceivedMonsterEvent matching[MONSTER_EVENT_CAPACITY];
        unsigned int count = select_slot_events(&taken, index, matching);
        SlotTickInput input = {
            local,    result, epoch, tick, now, &taken.incoming[index], taken.local_authority,
            matching, count};
        MonsterSyncFrame frame = tick_slot(slot, &input);
        if (!index)
            output = frame;
        cleanup_all = cleanup_all && frame.cleanup;
        fill_gallery_sample(&samples[index], slot, &frame);
        add_gallery_totals(&totals, slot, &frame);
    }

    output.gallery_count = gallery ? slot_count : 0u;
    output.gallery_active = totals.active;
    output.gallery_dead = totals.dead;
    output.gallery_faults = totals.faults;
    memcpy(output.gallery, samples, sizeof(samples));
    output.cleanup = cleanup_all;
    if (gallery)
    {
        output.deaths = totals.deaths;
        output.attacks = totals.attacks;
        output.predictions = totals.predictions;
        output.confirmations = totals.confirmations;
        output.corrections = totals.corrections;
    }
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
