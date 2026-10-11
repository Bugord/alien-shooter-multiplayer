#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../asmp-dll/src/multiplayer/monster_sync.h"
#define CHECK(c)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(c))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

/* The coordinator is real. These substitutes model native ownership and
   callbacks, so failures exercise orchestration without allocating engine objects. */
static Snapshot local;
static MonsterState remote;
typedef struct MockSlot
{
    UnitPuppetSample unit;
    UnitPuppetPose pose, death_pose;
    uint32_t replay_sequence;
    int f_selected, f_owned, f_removal_abandoned, f_last_health, f_spawn_health, f_visual_owned;
    unsigned int f_spawns, f_watches, f_attaches, f_applies, f_health_writes, f_replays;
    unsigned int f_removals, f_native_removals, f_releases, f_visual_plays, f_visual_animation,
        f_visual_removes;
    unsigned int f_visual_vid, f_spawn_vid;
    int f_spawn_host;
    uintptr_t f_root;
} MockSlot;
static MockSlot mock_slots[MP_MONSTER_CAPACITY];
static MockSlot armor_slots[MP_MONSTER_CAPACITY];
static unsigned int mock_slot;
static int armor_selected;
#define visual_slot       (*(armor_selected ? &armor_slots[mock_slot] : &mock_slots[mock_slot]))
#define native            (mock_slots[mock_slot].unit)
#define last_pose         (mock_slots[mock_slot].pose)
#define last_replay       (mock_slots[mock_slot].replay_sequence)
#define selected          (mock_slots[mock_slot].f_selected)
#define owned             (mock_slots[mock_slot].f_owned)
#define removal_abandoned (mock_slots[mock_slot].f_removal_abandoned)
#define spawns            (mock_slots[mock_slot].f_spawns)
#define watches           (mock_slots[mock_slot].f_watches)
#define attaches          (mock_slots[mock_slot].f_attaches)
#define applies           (mock_slots[mock_slot].f_applies)
#define health_writes     (mock_slots[mock_slot].f_health_writes)
#define replays           (mock_slots[mock_slot].f_replays)
#define removals          (mock_slots[mock_slot].f_removals)
#define native_removals   (mock_slots[mock_slot].f_native_removals)
#define releases          (mock_slots[mock_slot].f_releases)
#define last_health       (mock_slots[mock_slot].f_last_health)
#define spawn_health      (mock_slots[mock_slot].f_spawn_health)
#define visual_owned      (visual_slot.f_visual_owned)
#define visual_plays      (visual_slot.f_visual_plays)
#define visual_animation  (visual_slot.f_visual_animation)
#define visual_removes    (visual_slot.f_visual_removes)
#define visual_pose       (visual_slot.death_pose)
#define visual_vid        (visual_slot.f_visual_vid)
#define spawn_vid         (mock_slots[mock_slot].f_spawn_vid)
#define spawn_host        (mock_slots[mock_slot].f_spawn_host)
static uint32_t local_epoch;
static int installed;
static int fail_install, fail_init, fail_spawn, fail_apply, fail_remove, fail_release;
static int fail_visual, fail_visual_remove;
static unsigned int unhooks, fixture_stops;
static DWORD clock_now;
int unit_puppet_select(unsigned int slot)
{
    if (slot >= MP_MONSTER_CAPACITY)
        return 0;
    mock_slot = slot;
    return 1;
}
int unit_combat_select(unsigned int slot)
{
    return unit_puppet_select(slot);
}
int unit_death_select(unsigned int slot)
{
    if (slot >= MP_MONSTER_CAPACITY * 2u)
        return 0;
    armor_selected = slot >= MP_MONSTER_CAPACITY;
    mock_slot = slot % MP_MONSTER_CAPACITY;
    return 1;
}
int unit_death_init(uintptr_t base, ActorCreate create)
{
    CHECK(base == 1u && create);
    return 1;
}
void unit_puppet_set_form_observer(UnitFormObserver observer)
{
    CHECK(observer);
}
int unit_combat_observe_entity(uintptr_t game, uintptr_t entity, uintptr_t* private_vid,
                               unsigned int* vid, unsigned int* revision, int* transitioning,
                               unsigned int requested_vid)
{
    (void)game;
    (void)entity;
    (void)private_vid;
    (void)vid;
    (void)revision;
    (void)transitioning;
    (void)requested_vid;
    return 0;
}
int unit_death_play(uintptr_t game, const UnitPuppetPose* pose, unsigned int animation)
{
    CHECK(game == local.game && !visual_owned && (armor_selected || (!owned && !selected)));
    ++visual_plays;
    visual_pose = *pose;
    visual_animation = animation;
    visual_owned = 1;
    return fail_visual ? -1 : 1;
}
UnitDeathSample unit_death_sample(uintptr_t game)
{
    CHECK(game == local.game);
    UnitDeathSample s = {0};
    s.entities = visual_owned ? 2u : 0u;
    return s;
}
int unit_death_remove(uintptr_t game)
{
    CHECK(game == local.game);
    ++visual_removes;
    if (fail_visual_remove && visual_owned)
        return 0;
    visual_owned = 0;
    return 1;
}
int unit_death_play_type(uintptr_t game, const UnitPuppetPose* pose, unsigned int animation,
                         unsigned int vid)
{
    visual_vid = vid;
    return unit_death_play(game, pose, animation);
}
int unit_death_play_armor(uintptr_t game, const UnitPuppetPose* pose, unsigned int vid)
{
    CHECK(vid == 70u || vid == 90u);
    CHECK((!owned && !selected) || (owned && selected == 1 && vid == 90u));
    return unit_death_play_type(game, pose, 15u, vid);
}
int unit_death_stop(void)
{
    for (unsigned int i = 0; i < MP_MONSTER_CAPACITY; ++i)
        CHECK(!mock_slots[i].f_visual_owned && !armor_slots[i].f_visual_owned);
    return 1;
}
static void* __fastcall dummy_create(void* g, void* u, void* v, float x, float y, float z, int d,
                                     void* p)
{
    (void)g;
    (void)u;
    (void)v;
    (void)x;
    (void)y;
    (void)z;
    (void)d;
    (void)p;
    return NULL;
}
static void* __fastcall dummy_destroy(void* e, void* u, int r)
{
    (void)u;
    (void)r;
    return e;
}
static void __fastcall dummy_animation(void* e, void* u, unsigned int a)
{
    (void)e;
    (void)u;
    (void)a;
}
static UnitPuppetEngine api(void)
{
    UnitPuppetEngine out = {0};
    out.destroy = dummy_destroy;
    out.animation = dummy_animation;
    return out;
}
int unit_puppet_bind(uintptr_t base, UnitPuppetEngine* out)
{
    CHECK(base == 1u);
    *out = api();
    return 1;
}
int unit_combat_bind(uintptr_t base, ActorCreate* out)
{
    CHECK(base == 1u);
    *out = dummy_create;
    return 1;
}
int unit_puppet_install(uintptr_t base, const UnitPuppetEngine* engine)
{
    CHECK(base == 1u && engine && !installed);
    if (fail_install)
        return 0;
    installed = 1;
    return 1;
}
int unit_combat_init(uintptr_t base, ActorCreate create, ActorDestroy destroy,
                     UnitAnimation animation)
{
    CHECK(base == 1u && create && destroy && animation);
    return !fail_init;
}
int unit_puppet_watch_first(uintptr_t game, uintptr_t player)
{
    CHECK(game == local.game && player == local.player && installed && !selected);
    ++watches;
    selected = 1;
    native.state = UNIT_PUPPET_ACTIVE;
    native.entity = 100u + mock_slot * 2u;
    native.captured_attacks = native.captured_deaths = 0;
    return 1;
}
int unit_puppet_attach_entity(uintptr_t game, uintptr_t player, uintptr_t entity)
{
    CHECK(game == local.game && player == local.player && entity == mock_slots[mock_slot].f_root &&
          owned && !selected);
    native.predicted_hits = native.captured_attacks = native.captured_deaths =
        native.captured_forms = 0;
    native.predictive = native.transitioning = 0;
    native.death_animation = 0;
    last_replay = 0;
    ++attaches;
    selected = 2;
    native.state = UNIT_PUPPET_ACTIVE;
    native.entity = entity;
    return 1;
}
int unit_puppet_prediction(uintptr_t game)
{
    CHECK(game == local.game && selected == 2 && owned);
    native.predictive = 1;
    return 1;
}
int unit_combat_spawn(const Snapshot* spawn)
{
    CHECK(spawn && spawn->game == local.game && !owned);
    ++spawns;
    owned = 1;
    mock_slots[mock_slot].f_root = 100u + mock_slot * 2u;
    spawn_health = spawn->health;
    return fail_spawn ? -1 : 1;
}
int unit_combat_spawn_type(const Snapshot* spawn, unsigned int vid, const UnitPuppetPose* pose,
                           int host)
{
    CHECK(pose);
    spawn_vid = vid;
    spawn_host = host;
    memset(&native, 0, sizeof(native));
    native.vid = vid;
    native.animation = STEAM_ANIM_IDLE;
    native.health = 88;
    int result = unit_combat_spawn(spawn);
    native.x = pose->x;
    native.y = pose->y;
    native.z = pose->z;
    native.velocity = pose->velocity;
    native.direction = pose->direction;
    native.moving = pose->moving;
    return result;
}
int unit_combat_spawn_predictive(const Snapshot* spawn, unsigned int vid,
                                 const UnitPuppetPose* pose)
{
    return unit_combat_spawn_type(spawn, vid, pose, 0);
}
int unit_puppet_watch_stationary(uintptr_t game, uintptr_t player, uintptr_t entity)
{
    CHECK(owned && entity == mock_slots[mock_slot].f_root && spawn_host);
    int result = unit_puppet_watch_first(game, player);
    native.entity = entity;
    native.behavior = native.moving = 0;
    native.velocity = 0;
    return result;
}
UnitCombatSample unit_combat_sample(uintptr_t game, int compare_sources)
{
    CHECK(game == local.game && (compare_sources == 0 || compare_sources == 1));
    UnitCombatSample s = {0};
    s.root = owned ? mock_slots[mock_slot].f_root : 0;
    s.child = owned ? 101u + mock_slot * 2u : 0;
    s.private_entities = owned ? 2u : 0;
    s.sources_unchanged = 1;
    s.current_vid = native.vid;
    s.transitioning = native.transitioning;
    return s;
}
int unit_puppet_apply(uintptr_t game, const UnitPuppetPose* pose)
{
    CHECK(game == local.game && selected == 2 && pose);
    ++applies;
    last_pose = *pose;
    if (fail_apply)
        return -1;
    native.x = pose->x;
    native.y = pose->y;
    native.z = pose->z;
    native.velocity = pose->velocity;
    native.direction = pose->direction;
    native.moving = pose->moving;
    return 1;
}
int unit_puppet_health(uintptr_t game, int value)
{
    CHECK(game == local.game && selected == 2);
    ++health_writes;
    last_health = value;
    native.health = value;
    return 1;
}
int unit_puppet_replay(uintptr_t game, uint32_t sequence, unsigned int animation)
{
    CHECK(game == local.game && selected == 2 && animation == STEAM_ANIM_ATTACK);
    CHECK(!last_replay || mp_sequence_newer(sequence, last_replay));
    last_replay = sequence;
    ++replays;
    return 1;
}
UnitPuppetSample unit_puppet_sample(uintptr_t game)
{
    CHECK(game == local.game);
    return native;
}
int unit_puppet_release(uintptr_t game)
{
    CHECK(game == local.game);
    ++releases;
    if (fail_release)
        return 0;
    selected = 0;
    native.state = UNIT_PUPPET_READY;
    native.entity = 0;
    last_replay = 0;
    return 1;
}
int unit_combat_remove(uintptr_t game)
{
    CHECK(game == local.game);
    ++removals;
    if (!owned)
    {
        removal_abandoned = 0;
        return 1;
    }
    CHECK(!selected); /* Release all borrowed hook references before deletion. */
    if (removal_abandoned)
        return 0;
    ++native_removals;
    if (fail_remove)
    {
        removal_abandoned = 1;
        return 0;
    }
    owned = 0;
    return 1;
}
int unit_combat_stop(void)
{
    ++fixture_stops;
    for (unsigned int i = 0; i < MP_MONSTER_CAPACITY; ++i)
        CHECK(!mock_slots[i].f_owned);
    return 1;
}
int unit_puppet_stop(void)
{
    ++unhooks;
    for (unsigned int i = 0; i < MP_MONSTER_CAPACITY; ++i)
        CHECK(!mock_slots[i].f_selected);
    installed = 0;
    return 1;
}

static void defaults(void)
{
    mock_slot = 0;
    armor_selected = 0;
    memset(mock_slots, 0, sizeof(mock_slots));
    memset(armor_slots, 0, sizeof(armor_slots));
    memset(&local, 0, sizeof(local));
    memset(&remote, 0, sizeof(remote));
    memset(&last_pose, 0, sizeof(last_pose));
    local.game = 10u;
    local.player = 20u;
    local.in_level = 1;
    local.health = 100;
    local.world_low = 77u;
    local.world_high = 88u;
    local_epoch = 5u;
    remote.present = 1;
    remote.authority_id = 2u;
    remote.authority_session = 9u;
    remote.state.world_low = local.world_low;
    remote.state.world_high = local.world_high;
    remote.state.world_epoch = 12u;
    remote.state.monster_id = 1u;
    remote.state.vid = 50u;
    remote.state.origin_vid = 50u;
    remote.state.form_revision = 1u;
    remote.state.sequence = 1u;
    remote.state.alive = 1u;
    remote.state.health = 70;
    remote.state.x = 900.0f;
    remote.state.y = 300.0f;
    remote.state.z = 20.0f;
    remote.state.velocity = 0.03f;
    remote.state.direction = 64u;
    remote.state.moving = 1u;
    native.state = UNIT_PUPPET_READY;
    native.vid = 50u;
    native.health = 88;
    native.behavior = 3u;
    native.goal = 333u;
    native.x = 50.0f;
    native.y = 60.0f;
    native.z = 0;
    native.velocity = 0.02f;
    native.moving = 1u;
    for (unsigned int i = 1; i < MP_MONSTER_CAPACITY; ++i)
        mock_slots[i].unit = native;
    installed = selected = owned = removal_abandoned = 0;
    fail_install = fail_init = fail_spawn = fail_apply = fail_remove = fail_release = 0;
    spawns = watches = attaches = applies = health_writes = replays = 0;
    removals = native_removals = releases = unhooks = fixture_stops = 0;
    last_health = spawn_health = 0;
    last_replay = 0;
    clock_now = 0;
    visual_owned = fail_visual = fail_visual_remove = 0;
    visual_plays = visual_animation = visual_removes = 0;
    memset(&visual_pose, 0, sizeof(visual_pose));
}
static MonsterSyncFrame step_result(DWORD now, int authority, enum ProbeResult result)
{
    clock_now = now;
    monster_sync_publish(&remote, authority, now);
    return monster_sync_tick(&local, result, local_epoch, now / 10u, now);
}
static MonsterSyncFrame step(DWORD now, int authority)
{
    return step_result(now, authority, PROBE_OK);
}
static MonsterSyncFrame begin(enum MonsterRole role)
{
    defaults();
    CHECK(monster_sync_enable(1u, role));
    CHECK(step(0u, role == MONSTER_HOST).state == MS_WAITING);
    CHECK(!spawns && !watches);
    return step(2000u, role == MONSTER_HOST);
}
static void finish(void)
{
    monster_sync_request_stop();
    MonsterSyncFrame f = monster_sync_tick(&local, PROBE_OK, local_epoch, 0, clock_now + 1u);
    CHECK(f.cleanup && monster_sync_stopped() && monster_sync_stop());
    CHECK(!installed && !owned && !selected);
}
static MonsterEvent event_for(uint32_t sequence, unsigned int kind)
{
    MonsterEvent e = {0};
    e.authority_id = remote.authority_id;
    e.authority_session = remote.authority_session;
    e.event.sequence = sequence;
    e.event.kind = kind;
    e.event.monster_id = 1u;
    e.event.vid = remote.state.vid;
    e.event.origin_vid = remote.state.origin_vid;
    e.event.form_revision = remote.state.form_revision;
    e.event.form_phase = remote.state.form_phase;
    e.event.world_low = remote.state.world_low;
    e.event.world_high = remote.state.world_high;
    e.event.world_epoch = remote.state.world_epoch;
    e.event.x = remote.state.x;
    e.event.y = remote.state.y;
    e.event.z = remote.state.z;
    e.event.direction = remote.state.direction;
    e.event.death_animation = kind == MP_MONSTER_DEATH ? STEAM_ANIM_GIB_DEATH : 0;
    return e;
}
static void receive(uint32_t sequence, unsigned int kind, DWORD now)
{
    MonsterEvent e = event_for(sequence, kind);
    monster_sync_receive(&e, now);
}
static MonsterState gallery_remote[48];
static unsigned int gallery_type(unsigned int slot)
{
    static const unsigned int types[] = {40u, 50u, 60u, 80u, 70u, 90u};
    return types[slot / 8u];
}
static void gallery_defaults(void)
{
    defaults();
    for (unsigned int i = 0; i < 48u; ++i)
    {
        gallery_remote[i] = remote;
        gallery_remote[i].state.monster_id = i + 1u;
        gallery_remote[i].state.vid = gallery_type(i);
        gallery_remote[i].state.origin_vid = gallery_type(i);
        gallery_remote[i].state.x = 1000.0f + i * 80.0f;
        gallery_remote[i].state.y = 200.0f + (i / 8u) * 100.0f;
        gallery_remote[i].state.health = 50 + (int)i;
    }
}
static MonsterSyncFrame gallery_step(DWORD now, int authority)
{
    clock_now = now;
    for (unsigned int i = 0; i < 48u; ++i)
        monster_sync_publish_at(i, &gallery_remote[i], authority, now);
    return monster_sync_tick(&local, PROBE_OK, local_epoch, now / 10u, now);
}
static void gallery_checks(void)
{
    gallery_defaults();
    CHECK(monster_sync_enable(1u, MONSTER_CLIENT) && monster_sync_gallery(1));
    MonsterSyncFrame f = gallery_step(0u, 0);
    CHECK(f.gallery_count == 48u && !f.gallery_active);
    f = gallery_step(2000u, 0);
    CHECK(f.gallery_active == 48u && !f.gallery_dead && !f.gallery_faults);
    CHECK(!monster_sync_gallery(0)); /* Configuration cannot change live ownership. */
    for (unsigned int i = 0; i < 48u; ++i)
    {
        CHECK(mock_slots[i].f_spawns == 1u && mock_slots[i].f_attaches == 1u);
        CHECK(mock_slots[i].f_spawn_vid == gallery_type(i) && !mock_slots[i].f_spawn_host);
        CHECK(mock_slots[i].pose.x == gallery_remote[i].state.x);
        CHECK(mock_slots[i].f_last_health == 50 + (int)i);
        CHECK(f.gallery[i].id == i + 1u && f.gallery[i].vid == gallery_type(i));
    }
    /* Eight event deaths race alive states; eight other slots recover from
       persistent dead states. Events and state tombstones remain independent. */
    for (unsigned int i = 0; i < 8u; ++i)
    {
        MonsterEvent event = event_for(1u, MP_MONSTER_DEATH);
        event.event.monster_id = i + 1u;
        event.event.x = 5000.0f + i;
        event.event.vid = event.event.origin_vid = gallery_type(i);
        event.event.death_animation = i & 1u ? 16u : 15u;
        monster_sync_receive(&event, 2010u);
    }
    for (unsigned int i = 8u; i < 16u; ++i)
    {
        gallery_remote[i].state.alive = 0;
        gallery_remote[i].state.health = 0;
        gallery_remote[i].state.death_animation = i & 1u ? 16u : 15u;
    }
    f = gallery_step(2010u, 0);
    CHECK(f.gallery_active == 32u && f.gallery_dead == 16u && f.deaths == 16u && !f.gallery_faults);
    for (unsigned int i = 0; i < 16u; ++i)
    {
        CHECK(mock_slots[i].f_visual_plays == 1u && !mock_slots[i].f_owned);
        CHECK(mock_slots[i].f_visual_vid == gallery_type(i));
        CHECK(mock_slots[i].f_visual_animation == (i & 1u ? 16u : 15u));
        if (i < 8u)
            CHECK(mock_slots[i].death_pose.x == 5000.0f + i);
        else
            CHECK(mock_slots[i].death_pose.x == gallery_remote[i].state.x);
    }
    f = gallery_step(2020u, 0);
    CHECK(f.gallery_dead == 16u && f.gallery_active == 32u);
    for (unsigned int i = 0; i < 16u; ++i)
        CHECK(mock_slots[i].f_visual_plays == 1u && mock_slots[i].f_spawns == 1u);
    /* Expiry cleans live replicas and dead visual ownership for every slot. */
    clock_now = 3021u;
    f = monster_sync_tick(&local, PROBE_OK, local_epoch, 302u, clock_now);
    CHECK(!f.gallery_active && !f.gallery_faults);
    for (unsigned int i = 0; i < 48u; ++i)
        CHECK(!mock_slots[i].f_owned && !mock_slots[i].f_selected && !mock_slots[i].f_visual_owned);
    local_epoch++;
    for (unsigned int i = 0; i < 48u; ++i)
    {
        gallery_remote[i].state.world_epoch++;
        gallery_remote[i].state.alive = 1;
        gallery_remote[i].state.health = 50 + (int)i;
        gallery_remote[i].state.death_animation = 0;
    }
    CHECK(!gallery_step(3030u, 0).gallery_active);
    f = gallery_step(5030u, 0);
    CHECK(f.gallery_active == 48u && !f.gallery_dead);
    for (unsigned int i = 0; i < 48u; ++i)
        CHECK(mock_slots[i].f_spawns == 2u);
    finish();
    for (unsigned int i = 0; i < 48u; ++i)
        CHECK(!mock_slots[i].f_owned && !mock_slots[i].f_selected && !mock_slots[i].f_visual_owned);

    /* Host fixtures are stationary but report native death variants and exact
       poses per monster. Round-robin state draining must deliver every ID. */
    gallery_defaults();
    CHECK(monster_sync_enable(1u, MONSTER_HOST) && monster_sync_gallery(1));
    CHECK(!gallery_step(0u, 1).gallery_active);
    f = gallery_step(2000u, 1);
    CHECK(f.gallery_active == 48u && !f.gallery_faults);
    MpMonsterState state;
    uint64_t seen = 0;
    while (monster_sync_take_state(&state))
    {
        CHECK(state.monster_id >= 1u && state.monster_id <= 48u);
        uint64_t bit = UINT64_C(1) << (state.monster_id - 1u);
        CHECK(!(seen & bit));
        seen |= bit;
        CHECK(state.vid == gallery_type(state.monster_id - 1u) && state.alive && !state.velocity &&
              !state.moving);
    }
    CHECK(seen == UINT64_C(0xFFFFFFFFFFFF));
    for (unsigned int i = 0; i < 48u; ++i)
        CHECK(mock_slots[i].f_spawns == 1u && mock_slots[i].f_watches == 1u &&
              mock_slots[i].f_spawn_host);
    const unsigned int killed[] = {7u, 15u, 23u, 31u};
    for (unsigned int j = 0; j < 4u; ++j)
    {
        unsigned int i = killed[j];
        MockSlot* slot = &mock_slots[i];
        slot->unit.captured_deaths = 1u;
        slot->unit.death_animation = j & 1u ? 16u : 15u;
        slot->unit.death_pose.x = 777.0f + i;
        slot->unit.death_pose.y = 888.0f + i;
        slot->unit.death_pose.direction = 90u + i;
        slot->unit.animation = slot->unit.death_animation;
        slot->unit.health = 0;
        slot->unit.entity = 0;
        slot->unit.state = UNIT_PUPPET_LOST;
        slot->f_owned = 0; /* The engine deleted its native root. */
    }
    f = gallery_step(2010u, 1);
    CHECK(f.gallery_active == 44u && f.gallery_dead == 4u && f.deaths == 4u);
    MpMonsterEvent event;
    unsigned int event_count = 0;
    while (monster_sync_take_event(&event))
    {
        CHECK(event_count < 4u);
        unsigned int i = event.monster_id - 1u;
        CHECK(i == killed[event_count] && event.sequence == 1u && event.kind == MP_MONSTER_DEATH);
        CHECK(event.x == 777.0f + i && event.y == 888.0f + i && event.direction == 90u + i);
        CHECK(event.death_animation == (event_count & 1u ? 16u : 15u));
        ++event_count;
    }
    CHECK(event_count == 4u && gallery_step(2020u, 1).deaths == 4u &&
          !monster_sync_take_event(&event));
    local_epoch++;
    CHECK(!gallery_step(2030u, 1).gallery_active);
    for (unsigned int i = 0; i < 48u; ++i)
        CHECK(!mock_slots[i].f_owned && !mock_slots[i].f_selected);
    CHECK(gallery_step(4030u, 1).gallery_active == 48u);
    finish();
}
static void form_checks(void)
{
    const unsigned int affected[] = {0u, 8u, 16u, 24u, 32u, 40u};
    const unsigned int frozen[] = {49u, 59u, 69u, 89u, 79u, 99u};
    gallery_defaults();
    CHECK(monster_sync_enable(1u, MONSTER_CLIENT) && monster_sync_gallery(1));
    gallery_step(0u, 0);
    CHECK(gallery_step(2000u, 0).gallery_active == 48u);
    for (unsigned int j = 0; j < 6u; ++j)
    {
        MpMonsterState* s = &gallery_remote[affected[j]].state;
        s->vid = frozen[j];
        s->form_revision = 2u;
        s->sequence++;
        s->health = 30;
        s->velocity = 0;
        s->moving = 0;
    }
    MonsterSyncFrame f = gallery_step(2010u, 0);
    CHECK(f.gallery_active == 48u && !f.deaths && !f.gallery_faults);
    for (unsigned int j = 0; j < 6u; ++j)
    {
        unsigned int i = affected[j];
        CHECK(f.gallery[i].vid == frozen[j] && f.gallery[i].form_revision == 2u);
        CHECK(mock_slots[i].f_spawns == 2u && mock_slots[i].f_spawn_vid == frozen[j] &&
              !mock_slots[i].f_visual_plays);
    }
    /* A stale form and a death event for the previous allocation cannot kill
       or recreate an already frozen replica. The current snapshot wins. */
    gallery_remote[0].state.vid = 40u;
    gallery_remote[0].state.form_revision = 1u;
    MonsterEvent stale = event_for(10u, MP_MONSTER_DEATH);
    stale.event.vid = stale.event.origin_vid = 40u;
    stale.event.form_revision = 1u;
    monster_sync_receive(&stale, 2020u);
    f = gallery_step(2020u, 0);
    CHECK(f.gallery[0].vid == 49u && mock_slots[0].f_spawns == 2u && !f.deaths);
    gallery_remote[0].state.vid = 49u;
    gallery_remote[0].state.form_revision = 2u;
    gallery_remote[0].state.alive = 0;
    gallery_remote[0].state.health = 0;
    gallery_remote[0].state.death_animation = 15u;
    f = gallery_step(2030u, 0);
    CHECK(f.deaths == 1u && f.gallery_dead == 1u && !f.gallery_faults);
    CHECK(mock_slots[0].f_visual_vid == 49u && mock_slots[0].f_visual_plays == 1u);
    CHECK(gallery_step(2040u, 0).deaths == 1u && mock_slots[0].f_visual_plays == 1u);
    finish();

    gallery_defaults();
    CHECK(monster_sync_enable(1u, MONSTER_CLIENT) && monster_sync_gallery(1));
    gallery_step(0u, 0);
    gallery_step(2000u, 0);
    for (unsigned int i = 32u; i <= 40u; i += 8u)
    {
        MpMonsterState* s = &gallery_remote[i].state;
        s->form_revision = 2u;
        s->form_phase = MP_MONSTER_FORM_ARMOR;
        s->health = 1;
    }
    f = gallery_step(2010u, 0);
    CHECK(f.gallery_active == 48u && !f.deaths && !f.gallery_dead && !f.gallery_faults);
    CHECK(f.gallery[32].state == MS_TRANSFORMING && f.gallery[40].state == MS_TRANSFORMING);
    CHECK(armor_slots[32].f_visual_plays == 1u && mock_slots[32].f_owned && mock_slots[40].f_owned);
    CHECK(f.gallery[32].presentation_vid == 60u && f.gallery[40].presentation_vid == 80u);
    gallery_step(2020u, 0);
    CHECK(armor_slots[32].f_visual_plays == 1u);
    for (unsigned int i = 32u; i <= 40u; i += 8u)
    {
        MpMonsterState* s = &gallery_remote[i].state;
        s->vid = i == 32u ? 60u : 80u;
        s->form_revision = 3u;
        s->form_phase = 0;
        s->health = 90;
    }
    f = gallery_step(2030u, 0);
    CHECK(f.gallery_active == 48u && !f.deaths && !f.gallery_faults);
    CHECK(mock_slots[32].f_spawns == 2u && armor_slots[32].f_visual_plays == 1u &&
          mock_slots[32].f_spawn_vid == 60u);
    gallery_remote[32].state.alive = 0;
    gallery_remote[32].state.health = 0;
    gallery_remote[32].state.death_animation = 15u;
    f = gallery_step(2040u, 0);
    CHECK(f.deaths == 1u && mock_slots[32].f_visual_vid == 60u &&
          mock_slots[32].f_visual_plays == 1u);
    CHECK(armor_slots[32].f_visual_owned &&
          armor_slots[32].f_visual_plays == 1u); /* Debris survives the successor kill. */
    finish();

    /* Late join creates only the current frozen/successor form, without
       replaying an old armor explosion. Lost transition events need no replay. */
    gallery_defaults();
    gallery_remote[0].state.vid = 49u;
    gallery_remote[0].state.form_revision = 2u;
    gallery_remote[32].state.vid = 60u;
    gallery_remote[32].state.form_revision = 3u;
    CHECK(monster_sync_enable(1u, MONSTER_CLIENT) && monster_sync_gallery(1));
    gallery_step(0u, 0);
    f = gallery_step(2000u, 0);
    CHECK(f.gallery_active == 48u && mock_slots[0].f_spawn_vid == 49u &&
          mock_slots[32].f_spawn_vid == 60u);
    CHECK(!mock_slots[0].f_visual_plays && !mock_slots[32].f_visual_plays);
    finish();

    gallery_defaults();
    CHECK(monster_sync_enable(1u, MONSTER_HOST) && monster_sync_gallery(1));
    gallery_step(0u, 1);
    gallery_step(2000u, 1);
    mock_slots[32].unit.transitioning = 1;
    mock_slots[32].unit.animation = 15u;
    f = gallery_step(2010u, 1);
    CHECK(!f.deaths && f.gallery[32].state == MS_TRANSFORMING);
    MpMonsterState state;
    int found = 0;
    while (monster_sync_take_state(&state))
        if (state.monster_id == 33u)
        {
            CHECK(state.alive && state.vid == 70u && state.form_revision == 2u &&
                  state.form_phase == MP_MONSTER_FORM_ARMOR);
            CHECK(state.health == 1 && !state.death_animation);
            found = 1;
        }
    CHECK(found);
    mock_slots[32].unit.entity = 0;
    mock_slots[32].unit.state = UNIT_PUPPET_LOST;
    mock_slots[32].unit.transitioning = 0;
    mock_slots[32].unit.vid = 60u;
    mock_slots[32].unit.animation = 0;
    mock_slots[32].unit.health = 90;
    mock_slots[32].f_root = 900u;
    f = gallery_step(2020u, 1);
    CHECK(!f.deaths && f.gallery[32].entity == 900u && f.gallery[32].vid == 60u &&
          f.gallery[32].form_revision == 3u);
    CHECK(mock_slots[32].f_spawns == 1u && mock_slots[32].f_watches == 2u);
    CHECK(!armor_slots[32].f_visual_plays); /* Medium host debris is already native. */
    uintptr_t large_root = mock_slots[40].unit.entity;
    mock_slots[40].unit.transitioning = 1;
    mock_slots[40].unit.animation = 15u;
    f = gallery_step(2030u, 1);
    CHECK(f.gallery[40].state == MS_TRANSFORMING && f.gallery[40].entity == large_root);
    CHECK(mock_slots[40].f_owned && armor_slots[40].f_visual_plays == 1u &&
          f.gallery[40].effects == 2u);
    gallery_step(2040u, 1);
    CHECK(armor_slots[40].f_visual_plays == 1u);
    mock_slots[40].unit.entity = 0;
    mock_slots[40].unit.state = UNIT_PUPPET_LOST;
    mock_slots[40].unit.transitioning = 0;
    mock_slots[40].unit.vid = 80u;
    mock_slots[40].unit.animation = 0;
    mock_slots[40].unit.health = 200;
    mock_slots[40].f_root = 902u;
    f = gallery_step(2050u, 1);
    CHECK(f.gallery[40].entity == 902u && f.gallery[40].vid == 80u && !f.deaths &&
          !f.gallery_faults);
    CHECK(mock_slots[40].f_spawns == 1u && armor_slots[40].f_visual_plays == 1u);
    /* A coalesced successor still emits debris. Direct gib/freeze does not. */
    mock_slots[41].unit.animation = 16u;
    mock_slots[41].unit.captured_deaths = 1u;
    mock_slots[41].unit.death_animation = 16u;
    mock_slots[42].unit.vid = 99u;
    mock_slots[43].unit.vid = 80u;
    f = gallery_step(2060u, 1);
    CHECK(f.deaths == 1u && !armor_slots[41].f_visual_plays && !armor_slots[42].f_visual_plays);
    CHECK(armor_slots[43].f_visual_plays == 1u && !f.gallery_faults);
    mock_slots[40].unit.animation = 16u;
    mock_slots[40].unit.captured_deaths = 1u;
    mock_slots[40].unit.death_animation = 16u;
    f = gallery_step(2070u, 1);
    CHECK(f.deaths == 2u && armor_slots[40].f_visual_owned && armor_slots[40].f_visual_plays == 1u);
    finish();
}
static void predicted_damage(unsigned int slot, int health, unsigned int vid, unsigned int death,
                             int armor)
{
    MockSlot* m = &mock_slots[slot];
    CHECK(m->f_owned && m->f_selected == 2 && m->unit.predictive);
    ++m->unit.predicted_hits;
    m->unit.health = health;
    m->unit.vid = vid;
    m->unit.transitioning = armor;
    m->unit.animation = death ? death : armor ? STEAM_ANIM_DEATH : STEAM_ANIM_IDLE;
    if (death)
    {
        m->unit.captured_deaths = 1;
        m->unit.death_animation = death;
        m->unit.death_pose = m->pose;
    }
}
static void prediction_checks(void)
{
    MonsterSyncFrame f = begin(MONSTER_CLIENT);
    CHECK(native.predictive && f.gallery[0].presentation_vid == 50u);
    remote.state.sequence = 10u;
    step(2005u, 0);
    predicted_damage(0u, 45, 50u, 0, 0);
    f = step(2010u, 0);
    CHECK(native.health == 45 && f.predictions == 1u && !f.deaths && spawns == 1u);
    f = step(2020u, 0);
    CHECK(native.health == 45 && spawns == 1u && !f.confirmations);
    remote.state.sequence = 9u;
    f = step(2030u, 0);
    CHECK(native.health == 45 && !f.corrections && spawns == 1u);
    predicted_damage(0u, 20, 50u, 0, 0);
    f = step(2040u, 0);
    CHECK(native.health == 20 && f.predictions == 1u && !f.deaths);
    /* A duplicate received after the grace period is not host confirmation or
       rejection. A genuinely newer authoritative result may settle the HP. */
    remote.state.sequence = 10u;
    f = step(2800u, 0);
    CHECK(native.health == 20 && !f.corrections && spawns == 1u);
    remote.state.sequence = 11u;
    remote.state.health = 20;
    f = step(2810u, 0);
    CHECK(f.confirmations == 1u && !f.corrections && native.health == 20 && spawns == 1u);
    finish();

    f = begin(MONSTER_CLIENT);
    predicted_damage(0u, 30, 59u, 0, 0);
    f = step(2010u, 0);
    CHECK(f.state == MS_ACTIVE && f.vid == 50u && f.form_revision == 1u);
    CHECK(f.gallery[0].presentation_vid == 59u && native.vid == 59u && native.health == 30);
    CHECK(spawns == 1u && !f.deaths && !visual_plays && f.predictions == 1u);
    f = step(2020u, 0);
    CHECK(native.vid == 59u && !f.corrections && spawns == 1u);
    receive(1u, MP_MONSTER_ATTACK, 2025u);
    f = step(2025u, 0);
    CHECK(!replays && !f.attacks && native.vid == 59u && spawns == 1u);
    remote.state.sequence++;
    remote.state.vid = 59u;
    remote.state.form_revision = 2u;
    remote.state.health = 30;
    remote.state.velocity = 0;
    remote.state.moving = 0;
    f = step(2030u, 0);
    CHECK(f.vid == 59u && f.form_revision == 2u && f.confirmations == 1u && !f.corrections);
    CHECK(spawns == 1u && native.vid == 59u && !visual_plays);
    finish();

    f = begin(MONSTER_CLIENT);
    predicted_damage(0u, 70, 50u, STEAM_ANIM_GIB_DEATH, 0);
    f = step(2010u, 0);
    CHECK(f.state == MS_PREDICTED_DEAD && !f.deaths && !owned && !selected);
    CHECK(visual_plays == 1u && visual_owned && visual_animation == STEAM_ANIM_GIB_DEATH);
    CHECK(f.vid == 50u && f.form_revision == 1u && !f.confirmations);
    f = step(2020u, 0);
    CHECK(f.state == MS_PREDICTED_DEAD && visual_plays == 1u && spawns == 1u);
    remote.state.sequence++;
    remote.state.alive = 0;
    remote.state.health = 0;
    remote.state.death_animation = STEAM_ANIM_GIB_DEATH;
    f = step(2030u, 0);
    CHECK(f.state == MS_DEAD && f.deaths == 1u && f.confirmations == 1u && !f.corrections);
    CHECK(visual_plays == 1u && visual_owned && spawns == 1u);
    f = step(2040u, 0);
    CHECK(f.deaths == 1u && visual_plays == 1u);
    finish();

    f = begin(MONSTER_CLIENT);
    predicted_damage(0u, 70, 50u, STEAM_ANIM_GIB_DEATH, 0);
    step(2010u, 0);
    receive(1u, MP_MONSTER_DEATH, 2020u);
    f = step(2020u, 0);
    CHECK(f.state == MS_DEAD && f.deaths == 1u && f.confirmations == 1u && !f.corrections);
    CHECK(visual_plays == 1u && visual_owned && spawns == 1u);
    receive(1u, MP_MONSTER_DEATH, 2030u);
    f = step(2030u, 0);
    CHECK(f.state == MS_DEAD && f.deaths == 1u && visual_plays == 1u);
    finish();

    f = begin(MONSTER_CLIENT);
    predicted_damage(0u, 70, 50u, STEAM_ANIM_GIB_DEATH, 0);
    step(2010u, 0);
    f = step(2760u, 0);
    CHECK(f.state == MS_PREDICTED_DEAD && !f.corrections && visual_owned);
    remote.state.sequence++;
    f = step(2770u, 0);
    CHECK(f.state == MS_ACTIVE && f.corrections == 1u && !f.deaths && !f.confirmations);
    CHECK(owned && selected == 2 && native.health == 70 && !visual_owned && spawns == 2u);
    CHECK(visual_plays == 1u && native.predictive && !native.captured_deaths &&
          !native.predicted_hits);
    finish();

    /* Native impacts on several logical monsters have independent provisional
       forms/effects. Pending armor snapshots already have visible successors. */
    gallery_defaults();
    CHECK(monster_sync_enable(1u, MONSTER_CLIENT) && monster_sync_gallery(1));
    gallery_step(0u, 0);
    gallery_step(2000u, 0);
    predicted_damage(0u, 50, 40u, STEAM_ANIM_GIB_DEATH, 0);
    predicted_damage(8u, 25, 59u, 0, 0);
    predicted_damage(32u, 82, 70u, 0, 1);
    predicted_damage(40u, 90, 90u, 0, 1);
    f = gallery_step(2010u, 0);
    CHECK(f.predictions == 4u && !f.deaths && !f.gallery_faults);
    CHECK(f.gallery[0].state == MS_PREDICTED_DEAD && mock_slots[0].f_visual_plays == 1u);
    CHECK(f.gallery[8].vid == 50u && f.gallery[8].presentation_vid == 59u &&
          mock_slots[8].f_spawns == 1u);
    CHECK(mock_slots[32].f_owned && mock_slots[40].f_owned);
    CHECK(f.gallery[32].vid == 70u && f.gallery[32].presentation_vid == 60u);
    CHECK(f.gallery[40].vid == 90u && f.gallery[40].presentation_vid == 80u);
    CHECK(mock_slots[32].f_spawns == 2u && mock_slots[40].f_spawns == 2u);
    CHECK(armor_slots[32].f_visual_plays == 1u && armor_slots[40].f_visual_plays == 1u);
    for (unsigned int i = 32u; i <= 40u; i += 8u)
    {
        MpMonsterState* s = &gallery_remote[i].state;
        s->sequence++;
        s->form_revision = 2u;
        s->form_phase = MP_MONSTER_FORM_ARMOR;
        s->health = 1;
        s->velocity = 0;
        s->moving = 0;
    }
    f = gallery_step(2020u, 0);
    CHECK(f.gallery[32].state == MS_TRANSFORMING && f.gallery[40].state == MS_TRANSFORMING);
    CHECK(mock_slots[32].f_owned && mock_slots[40].f_owned && !f.gallery_faults);
    CHECK(mock_slots[32].f_spawns == 2u && mock_slots[40].f_spawns == 2u);
    CHECK(armor_slots[32].f_visual_plays == 1u && armor_slots[40].f_visual_plays == 1u);
    for (unsigned int i = 32u; i <= 40u; i += 8u)
    {
        MpMonsterState* s = &gallery_remote[i].state;
        s->sequence++;
        s->vid = i == 32u ? 60u : 80u;
        s->form_revision = 3u;
        s->form_phase = 0;
        s->health = 90;
    }
    gallery_remote[8].state.sequence++;
    gallery_remote[8].state.vid = 59u;
    gallery_remote[8].state.form_revision = 2u;
    gallery_remote[8].state.health = 25;
    f = gallery_step(2030u, 0);
    CHECK(f.confirmations == 3u && !f.corrections && !f.deaths && !f.gallery_faults);
    CHECK(mock_slots[32].f_spawns == 2u && mock_slots[40].f_spawns == 2u &&
          mock_slots[8].f_spawns == 1u);
    CHECK(armor_slots[32].f_visual_plays == 1u && armor_slots[40].f_visual_plays == 1u);
    finish(); /* Stop also removes the unconfirmed death and both armor banks. */

    /* World and authority boundaries remove provisional effects without ever
       turning them into a host tombstone or carrying them into the new copy. */
    f = begin(MONSTER_CLIENT);
    predicted_damage(0u, 70, 50u, STEAM_ANIM_DEATH, 0);
    step(2010u, 0);
    local_epoch++;
    remote.state.world_epoch++;
    f = step(2020u, 0);
    CHECK(f.state == MS_WAITING && !visual_owned && !owned && !f.deaths);
    f = step(4020u, 0);
    CHECK(f.state == MS_ACTIVE && owned && !visual_owned && spawns == 2u);
    CHECK(!native.predicted_hits && !native.captured_deaths);
    finish();

    f = begin(MONSTER_CLIENT);
    predicted_damage(0u, 70, 50u, STEAM_ANIM_DEATH, 0);
    step(2010u, 0);
    remote.authority_session++;
    f = step(2020u, 0);
    CHECK(f.state == MS_ACTIVE && owned && !visual_owned && !f.deaths && !f.confirmations);
    CHECK(spawns == 2u && !native.predicted_hits && !native.captured_deaths);
    finish();
}
static void prediction_chain_checks(void)
{
    MonsterSyncFrame f = begin(MONSTER_CLIENT);
    predicted_damage(0u, 30, 59u, 0, 0);
    step(2010u, 0);
    predicted_damage(0u, 30, 59u, STEAM_ANIM_DEATH, 0);
    f = step(2020u, 0);
    CHECK(f.state == MS_PREDICTED_DEAD && !f.deaths && visual_vid == 59u && visual_plays == 1u);
    CHECK(f.vid == 50u && f.form_revision == 1u && !owned);
    remote.state.sequence++;
    remote.state.vid = 59u;
    remote.state.form_revision = 2u;
    remote.state.health = 30;
    remote.state.velocity = 0;
    remote.state.moving = 0;
    f = step(2030u, 0);
    CHECK(f.state == MS_PREDICTED_DEAD && f.vid == 59u && !owned && !f.deaths && !f.corrections);
    CHECK(visual_plays == 1u && !f.confirmations);
    remote.state.sequence++;
    remote.state.alive = 0;
    remote.state.health = 0;
    remote.state.death_animation = STEAM_ANIM_DEATH;
    f = step(2040u, 0);
    CHECK(f.state == MS_DEAD && f.confirmations == 1u && f.deaths == 1u && !f.corrections);
    CHECK(visual_plays == 1u && spawns == 1u);
    finish();

    gallery_defaults();
    CHECK(monster_sync_enable(1u, MONSTER_CLIENT) && monster_sync_gallery(1));
    gallery_step(0u, 0);
    gallery_step(2000u, 0);
    predicted_damage(32u, 82, 70u, 0, 1);
    gallery_step(2010u, 0);
    predicted_damage(32u, 88, 60u, STEAM_ANIM_GIB_DEATH, 0);
    f = gallery_step(2020u, 0);
    CHECK(f.gallery[32].state == MS_PREDICTED_DEAD && !f.deaths && !mock_slots[32].f_owned);
    CHECK(mock_slots[32].f_visual_vid == 60u && mock_slots[32].f_visual_plays == 1u);
    CHECK(armor_slots[32].f_visual_owned && armor_slots[32].f_visual_plays == 1u);
    MpMonsterState* armored = &gallery_remote[32].state;
    armored->sequence++;
    armored->form_revision = 2u;
    armored->form_phase = MP_MONSTER_FORM_ARMOR;
    armored->health = 1;
    armored->velocity = 0;
    armored->moving = 0;
    f = gallery_step(2030u, 0);
    CHECK(f.gallery[32].state == MS_PREDICTED_DEAD && !mock_slots[32].f_owned && !f.corrections &&
          !f.deaths);
    armored->sequence++;
    armored->vid = 60u;
    armored->form_revision = 3u;
    armored->form_phase = 0;
    armored->health = 90;
    f = gallery_step(2040u, 0);
    CHECK(f.gallery[32].state == MS_PREDICTED_DEAD && !mock_slots[32].f_owned && !f.confirmations);
    CHECK(mock_slots[32].f_visual_plays == 1u && armor_slots[32].f_visual_plays == 1u);
    armored->sequence++;
    armored->alive = 0;
    armored->health = 0;
    armored->death_animation = STEAM_ANIM_GIB_DEATH;
    f = gallery_step(2050u, 0);
    CHECK(f.gallery[32].state == MS_DEAD && f.deaths == 1u && f.confirmations == 1u &&
          !f.corrections);
    CHECK(mock_slots[32].f_spawns == 2u && mock_slots[32].f_visual_plays == 1u &&
          armor_slots[32].f_visual_plays == 1u);
    finish();

    /* An armor-form acknowledgement predates an additional successor hit. It
       confirms the form, but may not restore the HP lost in that later hit. */
    gallery_defaults();
    CHECK(monster_sync_enable(1u, MONSTER_CLIENT) && monster_sync_gallery(1));
    gallery_step(0u, 0);
    gallery_step(2000u, 0);
    predicted_damage(32u, 82, 70u, 0, 1);
    gallery_step(2010u, 0);
    predicted_damage(32u, 25, 60u, 0, 0);
    f = gallery_step(2020u, 0);
    CHECK(mock_slots[32].unit.health == 25 && !f.confirmations);
    armored = &gallery_remote[32].state;
    armored->sequence++;
    armored->form_revision = 2u;
    armored->form_phase = MP_MONSTER_FORM_ARMOR;
    armored->health = 1;
    armored->velocity = 0;
    armored->moving = 0;
    gallery_step(2030u, 0);
    armored->sequence++;
    armored->vid = 60u;
    armored->form_revision = 3u;
    armored->form_phase = 0;
    armored->health = 90;
    f = gallery_step(2040u, 0);
    CHECK(f.gallery[32].vid == 60u && f.gallery[32].presentation_vid == 60u &&
          mock_slots[32].unit.health == 25);
    CHECK(!f.confirmations && !f.corrections && mock_slots[32].f_spawns == 2u &&
          armor_slots[32].f_visual_plays == 1u);
    armored->sequence++;
    armored->health = 25;
    f = gallery_step(2050u, 0);
    CHECK(f.confirmations == 1u && !f.corrections && mock_slots[32].unit.health == 25);
    finish();

    f = begin(MONSTER_CLIENT);
    predicted_damage(0u, 70, 50u, STEAM_ANIM_GIB_DEATH, 0);
    step(2010u, 0);
    remote.state.sequence++;
    remote.state.alive = 0;
    remote.state.health = 0;
    remote.state.death_animation = STEAM_ANIM_DEATH;
    f = step(2020u, 0);
    CHECK(f.state == MS_DEAD && f.deaths == 1u && f.corrections == 1u && !f.confirmations);
    CHECK(visual_plays == 2u && visual_animation == STEAM_ANIM_DEATH && visual_owned && !owned);
    step(2030u, 0);
    CHECK(visual_plays == 2u);
    finish();

    f = begin(MONSTER_CLIENT);
    predicted_damage(0u, 70, 50u, STEAM_ANIM_GIB_DEATH, 0);
    step(2010u, 0);
    remote.state.sequence++;
    remote.state.vid = 59u;
    remote.state.form_revision = 2u;
    remote.state.health = 30;
    remote.state.velocity = 0;
    remote.state.moving = 0;
    f = step(2020u, 0);
    CHECK(f.state == MS_ACTIVE && f.vid == 59u && f.corrections == 1u && !f.deaths &&
          !f.confirmations);
    CHECK(owned && native.vid == 59u && native.health == 30 && !visual_owned && spawns == 2u);
    finish();

    f = begin(MONSTER_CLIENT);
    predicted_damage(0u, 70, 50u, STEAM_ANIM_GIB_DEATH, 0);
    step(2010u, 0);
    clock_now = 3011u;
    f = monster_sync_tick(&local, PROBE_OK, local_epoch, 301u, clock_now);
    CHECK(f.state == MS_WAITING && !owned && !selected && !visual_owned && !f.deaths);
    remote.state.sequence++;
    f = step(3020u, 0);
    CHECK(f.state == MS_ACTIVE && owned && native.health == 70 && !visual_owned && spawns == 2u);
    CHECK(!native.predicted_hits && !native.captured_deaths);
    finish();
}
int main(void)
{
    MonsterSyncFrame f = begin(MONSTER_CLIENT);
    CHECK(f.state == MS_ACTIVE && spawns == 1 && attaches == 1 && applies == 1 &&
          health_writes == 1);
    CHECK(last_pose.x == 900.0f && last_pose.y == 300.0f && last_pose.z == 20.0f);
    CHECK(last_pose.velocity == 0.03f && last_pose.direction == 64u && last_pose.moving == 1u &&
          last_health == 70);
    CHECK(!watches && !replays);
    local.health = 0;
    f = step(2010u, 0);
    CHECK(f.state == MS_ACTIVE && spawns == 1 && !native_removals && spawn_health > 0);
    receive(1u, MP_MONSTER_ATTACK, 2020u);
    f = step(2020u, 0);
    CHECK(replays == 1 && f.attacks == 1);
    receive(1u, MP_MONSTER_ATTACK, 2030u);
    receive(0u, MP_MONSTER_ATTACK, 2030u);
    MonsterEvent invalid = event_for(2u, MP_MONSTER_ATTACK);
    invalid.authority_session++;
    monster_sync_receive(&invalid, 2030u);
    invalid = event_for(2u, MP_MONSTER_ATTACK);
    invalid.event.world_low++;
    monster_sync_receive(&invalid, 2030u);
    invalid = event_for(2u, MP_MONSTER_ATTACK);
    invalid.event.world_epoch++;
    monster_sync_receive(&invalid, 2030u);
    f = step(2030u, 0);
    CHECK(replays == 1 && f.rejected == 5u);
    receive(2u, MP_MONSTER_ATTACK, 2040u);
    f = step(2300u, 0);
    CHECK(replays == 1 &&
          f.rejected == 6u); /* Received event expiry, independently of fresh states. */
    CHECK(!monster_sync_stop() && !unhooks && !fixture_stops);
    monster_sync_request_stop();
    CHECK(!monster_sync_stopped() && !monster_sync_stop() && !unhooks);
    finish();
    CHECK(unhooks == 1 && fixture_stops == 1 && native_removals == 1);

    /* The native player/army may disappear after death while the map remains
       loaded. Neither probe result changes the monster's world identity. */
    const enum ProbeResult absent_results[] = {PROBE_NO_PLAYER, PROBE_NO_ARMY};
    for (unsigned int i = 0; i < sizeof(absent_results) / sizeof(absent_results[0]); ++i)
    {
        CHECK(begin(MONSTER_CLIENT).state == MS_ACTIVE);
        unsigned int before_releases = releases, before_removals = removals;
        local.player = local.army = 0;
        local.health = 0;
        f = step_result(2010u, 0, absent_results[i]);
        CHECK(f.state == MS_ACTIVE && owned && selected == 2 && spawns == 1);
        CHECK(releases == before_releases && removals == before_removals && !native_removals);
        CHECK(applies == 2 && health_writes == 2 && last_health == remote.state.health);
        finish();

        CHECK(begin(MONSTER_HOST).state == MS_ACTIVE);
        before_releases = releases;
        local.player = local.army = 0;
        local.health = 0;
        f = step_result(2010u, 1, absent_results[i]);
        CHECK(f.state == MS_ACTIVE && selected == 1 && watches == 1 && releases == before_releases);
        MpMonsterState retained;
        CHECK(monster_sync_take_state(&retained) && retained.alive && retained.health == 88);
        CHECK(retained.world_low == local.world_low && retained.world_high == local.world_high &&
              retained.world_epoch == local_epoch);
        CHECK(!applies && !health_writes && !f.deaths);
        finish();

        /* A client can settle the world and create a replica even while its
           local player is absent; its factory only needs the live native map. */
        defaults();
        local.player = local.army = 0;
        local.health = 0;
        CHECK(monster_sync_enable(1u, MONSTER_CLIENT));
        CHECK(step_result(0u, 0, absent_results[i]).state == MS_WAITING && !spawns);
        CHECK(step_result(2000u, 0, absent_results[i]).state == MS_ACTIVE && spawns == 1 &&
              spawn_health > 0);
        finish();

        /* Host discovery needs a player to exclude, but its settling timer
           must survive the temporarily absent player/army. */
        defaults();
        local.player = local.army = 0;
        local.health = 0;
        CHECK(monster_sync_enable(1u, MONSTER_HOST));
        CHECK(step_result(0u, 1, absent_results[i]).state == MS_WAITING && !watches);
        before_releases = releases;
        CHECK(step_result(2000u, 1, absent_results[i]).state == MS_WAITING && !watches &&
              releases == before_releases);
        local.player = 20u;
        local.health = 100;
        CHECK(step(2010u, 1).state == MS_ACTIVE && watches == 1);
        finish();
    }

    /* An explicit death wins over an older alive snapshot already in flight. */
    f = begin(MONSTER_CLIENT);
    CHECK(f.state == MS_ACTIVE);
    receive(1u, MP_MONSTER_DEATH, 2010u);
    f = step(2010u, 0);
    CHECK(f.state == MS_DEAD && f.deaths == 1 && native_removals == 1 && !owned);
    f = step(2020u, 0);
    CHECK(f.state == MS_DEAD && spawns == 1 && !owned);
    remote.state.alive = 0;
    remote.state.health = 0;
    remote.state.death_animation = STEAM_ANIM_GIB_DEATH;
    f = step(2030u, 0);
    CHECK(f.deaths == 1 && native_removals == 1 && visual_plays == 1 && visual_animation == 16u);
    CHECK(f.death_animation == 16 && f.death_pose.x == remote.state.x);
    CHECK(visual_pose.x == remote.state.x && visual_pose.direction == remote.state.direction);
    finish();

    f = begin(MONSTER_CLIENT);
    CHECK(f.state == MS_ACTIVE);
    remote.state.alive = 0;
    remote.state.health = 0;
    remote.state.death_animation = STEAM_ANIM_GIB_DEATH;
    f = step(2010u, 0);
    CHECK(f.state == MS_DEAD && !owned && native_removals == 1 && f.deaths == 1);
    remote.state.alive = 1;
    remote.state.health = 70;
    f = step(2020u, 0);
    CHECK(spawns == 1 && !owned);
    finish();

    /* Persistent death state supplies late join and event-loss recovery. */
    defaults();
    remote.state.alive = 0;
    remote.state.health = 0;
    remote.state.death_animation = 15;
    CHECK(monster_sync_enable(1u, MONSTER_CLIENT));
    CHECK(step(0u, 0).state == MS_WAITING);
    f = step(2000u, 0);
    CHECK(f.state == MS_DEAD && !spawns && visual_plays == 1 && visual_animation == 15);
    CHECK(visual_pose.x == 900.0f && visual_pose.y == 300.0f && visual_pose.z == 20.0f);
    CHECK(step(2500u, 0).state == MS_DEAD && visual_plays == 1 && visual_owned);
    CHECK(step(32001u, 0).state == MS_DEAD && !visual_owned && visual_plays == 1);
    CHECK(step(33000u, 0).state == MS_DEAD && visual_plays == 1);
    finish();

    defaults();
    remote.state.alive = 0;
    remote.state.health = 0;
    remote.state.death_animation = 16;
    fail_visual = 1;
    CHECK(monster_sync_enable(1u, MONSTER_CLIENT));
    step(0u, 0);
    CHECK(step(2000u, 0).state == MS_BACKOFF && visual_plays == 1 && !visual_owned);
    CHECK(step(3999u, 0).state == MS_BACKOFF && visual_plays == 1);
    fail_visual = 0;
    CHECK(step(4000u, 0).state == MS_DEAD && visual_plays == 2 && visual_owned);
    fail_visual_remove = 1;
    remote.present = 0;
    CHECK(step(4010u, 0).state == MS_ABANDONED && visual_owned);
    unsigned int visual_before = visual_removes;
    CHECK(step(5000u, 0).state == MS_ABANDONED && visual_removes == visual_before);
    visual_owned = fail_visual_remove = 0;
    local_epoch++;
    remote.present = 1;
    remote.state.world_epoch++;
    CHECK(step(5010u, 0).state == MS_WAITING);
    CHECK(step(7010u, 0).state == MS_DEAD && visual_plays == 3);
    finish();

    /* A late join begins at the host's event watermark rather than replaying
       attacks which finished before the client had a native replica. */
    defaults();
    remote.state.event_sequence = 30u;
    CHECK(monster_sync_enable(1u, MONSTER_CLIENT));
    CHECK(step(0u, 0).state == MS_WAITING);
    receive(29u, MP_MONSTER_ATTACK, 2000u);
    receive(30u, MP_MONSTER_ATTACK, 2000u);
    f = step(2000u, 0);
    CHECK(f.state == MS_ACTIVE && !replays && f.rejected == 2u);
    receive(31u, MP_MONSTER_ATTACK, 2010u);
    CHECK(step(2010u, 0).attacks == 1u && replays == 1);
    finish();

    f = begin(MONSTER_CLIENT);
    CHECK(f.state == MS_ACTIVE);
    for (uint32_t i = 1u; i <= 65u; ++i)
        receive(i, MP_MONSTER_ATTACK, 2010u);
    f = step(2010u, 0);
    CHECK(replays == 64u && f.attacks == 64u && f.dropped == 1u);
    CHECK(step(2020u, 0).dropped == 1u && replays == 64u);
    finish();

    f = begin(MONSTER_CLIENT);
    CHECK(f.state == MS_ACTIVE);
    clock_now = 3001u;
    f = monster_sync_tick(&local, PROBE_OK, local_epoch, 300u, clock_now);
    CHECK(f.state == MS_WAITING && !owned &&
          native_removals == 1); /* No new publish: host expired. */
    CHECK(step(3010u, 0).state == MS_ACTIVE && spawns == 2);
    remote.present = 0;
    CHECK(step(3020u, 0).state == MS_WAITING && !owned);
    finish();

    f = begin(MONSTER_CLIENT);
    CHECK(f.state == MS_ACTIVE);
    /* Same map, new world epochs: remove old object and start a fresh settling window. */
    local_epoch++;
    remote.state.world_epoch++;
    invalid = event_for(1u, MP_MONSTER_ATTACK);
    invalid.event.world_epoch--;
    CHECK(step(2010u, 0).state == MS_WAITING && !owned && native_removals == 1);
    monster_sync_receive(&invalid, 4010u);
    f = step(4010u, 0);
    CHECK(f.state == MS_ACTIVE && spawns == 2 && !replays && f.rejected == 1);
    finish();

    /* Factory faults get exactly one cleanup and unlimited capped backoff. */
    defaults();
    fail_spawn = 1;
    CHECK(monster_sync_enable(1u, MONSTER_CLIENT));
    CHECK(step(0u, 0).state == MS_WAITING);
    DWORD attempt_at = 2000u;
    const DWORD delays[] = {2000u, 4000u, 8000u, 16000u, 30000u, 30000u};
    for (unsigned int i = 0; i < sizeof(delays) / sizeof(delays[0]); ++i)
    {
        CHECK(step(attempt_at, 0).state == MS_BACKOFF && spawns == i + 1u &&
              native_removals == i + 1u);
        CHECK(step(attempt_at + delays[i] - 1u, 0).state == MS_BACKOFF && spawns == i + 1u);
        attempt_at += delays[i];
    }
    fail_spawn = 0;
    CHECK(step(attempt_at, 0).state == MS_ACTIVE && owned);
    finish();

    f = begin(MONSTER_CLIENT);
    CHECK(f.state == MS_ACTIVE);
    fail_apply = fail_remove = 1;
    CHECK(step(2010u, 0).state == MS_ABANDONED && native_removals == 1);
    unsigned int removal_calls = removals;
    CHECK(step(200000u, 0).state == MS_ABANDONED && removals == removal_calls && spawns == 1);
    /* The engine has torn down the old world. No second unsafe native removal. */
    owned = 0;
    fail_apply = fail_remove = 0;
    local_epoch++;
    remote.state.world_epoch++;
    CHECK(step(200010u, 0).state == MS_WAITING && native_removals == 1);
    CHECK(step(202010u, 0).state == MS_ACTIVE && spawns == 2);
    finish();

    f = begin(MONSTER_HOST);
    CHECK(f.state == MS_ACTIVE && watches == 1 && !spawns);
    CHECK(native.behavior == 3u && native.goal == 333u && native.velocity == 0.02f);
    CHECK(!applies && !health_writes &&
          !replays); /* Host AI and native actions remain authoritative. */
    MpMonsterState state;
    MpMonsterEvent outgoing;
    CHECK(monster_sync_take_state(&state) && state.alive && state.health == 88 &&
          state.monster_id == 1u);
    native.animation = STEAM_ANIM_ATTACK;
    f = step(2010u, 1);
    CHECK(!monster_sync_take_event(&outgoing) &&
          !f.attacks); /* Animation alone is never an attack event. */
    native.captured_attacks = 2u;
    f = step(2020u, 1);
    CHECK(f.attacks == 2u && monster_sync_take_event(&outgoing) && outgoing.sequence == 1u &&
          outgoing.kind == MP_MONSTER_ATTACK);
    CHECK(monster_sync_take_event(&outgoing) && outgoing.sequence == 2u &&
          outgoing.kind == MP_MONSTER_ATTACK);
    CHECK(!monster_sync_take_event(&outgoing) && monster_sync_take_state(&state) &&
          state.event_sequence == 2u);
    CHECK(step(2030u, 1).attacks == 2u && !monster_sync_take_event(&outgoing));
    native.captured_deaths = 1u;
    native.animation = STEAM_ANIM_GIB_DEATH;
    native.death_animation = 16;
    native.death_pose.x = 123.0f;
    native.death_pose.y = 456.0f;
    native.death_pose.z = 7.0f;
    native.death_pose.direction = 99;
    CHECK(step(2040u, 1).deaths == 1u && monster_sync_take_event(&outgoing));
    CHECK(outgoing.sequence == 3u && outgoing.kind == MP_MONSTER_DEATH);
    CHECK(outgoing.death_animation == 16 && outgoing.x == 123.0f && outgoing.y == 456.0f &&
          outgoing.direction == 99);
    CHECK(monster_sync_take_state(&state) && !state.alive && !state.health &&
          state.event_sequence == 3u);
    CHECK(state.death_animation == 16 && state.x == 123.0f && state.z == 7.0f &&
          state.direction == 99);
    native.entity = 0;
    native.x = native.y = native.z = 0;
    native.state = UNIT_PUPPET_LOST;
    step(2045u, 1);
    CHECK(monster_sync_take_state(&state) && state.x == 123.0f && state.direction == 99 &&
          state.death_animation == 16);
    CHECK(step(2050u, 1).deaths == 1u && !monster_sync_take_event(&outgoing));
    finish();

    f = begin(MONSTER_HOST);
    CHECK(f.state == MS_ACTIVE);
    native.captured_attacks = 1u;
    CHECK(step(2010u, 1).attacks == 1u);
    CHECK(monster_sync_take_event(&outgoing) && outgoing.sequence == 1u);
    CHECK(step(2020u, 0).state == MS_WAITING && !selected && !applies && !health_writes);
    CHECK(step(2030u, 1).state == MS_ACTIVE && watches == 2u);
    CHECK(!monster_sync_take_event(
        &outgoing)); /* New watch counters cannot underflow into phantom attacks. */
    native.state = UNIT_PUPPET_FAULT;
    CHECK(step(2040u, 1).state == MS_BACKOFF && !selected);
    CHECK(step(4039u, 1).state == MS_BACKOFF && watches == 2u);
    CHECK(step(4040u, 1).state == MS_ACTIVE && watches == 3u &&
          !monster_sync_take_event(&outgoing));
    finish();

    f = begin(MONSTER_CLIENT);
    CHECK(f.state == MS_ACTIVE);
    fail_release = 1;
    monster_sync_request_stop();
    f = monster_sync_tick(&local, PROBE_OK, local_epoch, 0, 2010u);
    CHECK(!f.cleanup && !monster_sync_stopped() && !monster_sync_stop() && !unhooks &&
          !fixture_stops);
    fail_release = 0;
    finish();

    /* Failed initialization can use the same truthful shutdown path and then reinstall. */
    defaults();
    fail_install = 1;
    UnitPuppetEngine engine = api();
    CHECK(!monster_sync_initialize(1u, MONSTER_CLIENT, &engine, dummy_create));
    CHECK(monster_sync_stop() && !installed);
    defaults();
    fail_init = 1;
    CHECK(!monster_sync_initialize(1u, MONSTER_CLIENT, &engine, dummy_create));
    CHECK(monster_sync_stopped() && monster_sync_stop() && !installed);
    defaults();
    CHECK(monster_sync_initialize(1u, MONSTER_CLIENT, &engine, dummy_create));
    finish();
    gallery_checks();
    form_checks();
    prediction_checks();
    prediction_chain_checks();
    puts("Monster coordinator authority, events, death and lifecycle checks passed.");
    return 0;
}
