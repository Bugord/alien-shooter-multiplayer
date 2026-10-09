#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../asmp-dll/src/multiplayer/monster_sync.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

/* The coordinator is real. These substitutes model native ownership and
   callbacks, so failures exercise orchestration without allocating engine objects. */
static Snapshot local;
static MonsterState remote;
static UnitPuppetSample native;
static UnitPuppetPose last_pose;
static uint32_t local_epoch, last_replay;
static int installed, selected, owned, removal_abandoned;
static int fail_install, fail_init, fail_spawn, fail_apply, fail_remove, fail_release;
static unsigned int spawns, watches, attaches, applies, health_writes, replays;
static unsigned int removals, native_removals, releases, unhooks, fixture_stops;
static int last_health, spawn_health;
static DWORD clock_now;
static void* __fastcall dummy_create(void* g, void* u, void* v, float x, float y, float z, int d, void* p) {
    (void)g; (void)u; (void)v; (void)x; (void)y; (void)z; (void)d; (void)p; return NULL;
}
static void* __fastcall dummy_destroy(void* e, void* u, int r) { (void)u; (void)r; return e; }
static void __fastcall dummy_animation(void* e, void* u, unsigned int a) { (void)e; (void)u; (void)a; }
static UnitPuppetEngine api(void) {
    UnitPuppetEngine out = {0}; out.destroy = dummy_destroy; out.animation = dummy_animation; return out;
}
int unit_puppet_bind(uintptr_t base, UnitPuppetEngine* out) { CHECK(base == 1u); *out = api(); return 1; }
int unit_combat_bind(uintptr_t base, ActorCreate* out) { CHECK(base == 1u); *out = dummy_create; return 1; }
int unit_puppet_install(uintptr_t base, const UnitPuppetEngine* engine) {
    CHECK(base == 1u && engine && !installed);
    if (fail_install) return 0; installed = 1; return 1;
}
int unit_combat_init(uintptr_t base, ActorCreate create, ActorDestroy destroy, UnitAnimation animation) {
    CHECK(base == 1u && create && destroy && animation); return !fail_init;
}
int unit_puppet_watch_first(uintptr_t game, uintptr_t player) {
    CHECK(game == local.game && player == local.player && installed && !selected);
    ++watches; selected = 1; native.state = UNIT_PUPPET_ACTIVE; native.entity = 100u;
    native.captured_attacks = native.captured_deaths = 0; return 1;
}
int unit_puppet_attach_entity(uintptr_t game, uintptr_t player, uintptr_t entity) {
    CHECK(game == local.game && player == local.player && entity == 100u && owned && !selected);
    ++attaches; selected = 2; native.state = UNIT_PUPPET_ACTIVE; native.entity = entity; return 1;
}
int unit_combat_spawn(const Snapshot* spawn) {
    CHECK(spawn && spawn->game == local.game && !owned);
    ++spawns; owned = 1; spawn_health = spawn->health; return fail_spawn ? -1 : 1;
}
UnitCombatSample unit_combat_sample(uintptr_t game, int compare_sources) {
    CHECK(game == local.game && compare_sources);
    UnitCombatSample s = {0}; s.root = owned ? 100u : 0; s.child = owned ? 101u : 0;
    s.private_entities = owned ? 2u : 0; s.sources_unchanged = 1; return s;
}
int unit_puppet_apply(uintptr_t game, const UnitPuppetPose* pose) {
    CHECK(game == local.game && selected == 2 && pose); ++applies; last_pose = *pose;
    if (fail_apply) return -1;
    native.x = pose->x; native.y = pose->y; native.z = pose->z;
    native.velocity = pose->velocity; native.direction = pose->direction; native.moving = pose->moving; return 1;
}
int unit_puppet_health(uintptr_t game, int value) {
    CHECK(game == local.game && selected == 2); ++health_writes; last_health = value; native.health = value; return 1;
}
int unit_puppet_replay(uintptr_t game, uint32_t sequence, unsigned int animation) {
    CHECK(game == local.game && selected == 2 && animation == STEAM_ANIM_ATTACK);
    CHECK(!last_replay || mp_sequence_newer(sequence, last_replay)); last_replay = sequence; ++replays; return 1;
}
UnitPuppetSample unit_puppet_sample(uintptr_t game) { CHECK(game == local.game); return native; }
int unit_puppet_release(uintptr_t game) {
    CHECK(game == local.game); ++releases;
    if (fail_release) return 0;
    selected = 0; native.state = UNIT_PUPPET_READY; native.entity = 0; last_replay = 0; return 1;
}
int unit_combat_remove(uintptr_t game) {
    CHECK(game == local.game); ++removals;
    if (!owned) { removal_abandoned = 0; return 1; }
    CHECK(!selected); /* Release all borrowed hook references before deletion. */
    if (removal_abandoned) return 0;
    ++native_removals;
    if (fail_remove) { removal_abandoned = 1; return 0; }
    owned = 0; return 1;
}
int unit_combat_stop(void) { ++fixture_stops; CHECK(!owned); return 1; }
int unit_puppet_stop(void) { ++unhooks; CHECK(!selected); installed = 0; return 1; }

static void defaults(void) {
    memset(&local, 0, sizeof(local)); memset(&remote, 0, sizeof(remote)); memset(&native, 0, sizeof(native));
    memset(&last_pose, 0, sizeof(last_pose));
    local.game = 10u; local.player = 20u; local.in_level = 1; local.health = 100;
    local.world_low = 77u; local.world_high = 88u; local_epoch = 5u;
    remote.present = 1; remote.authority_id = 2u; remote.authority_session = 9u;
    remote.state.world_low = local.world_low; remote.state.world_high = local.world_high;
    remote.state.world_epoch = 12u; remote.state.monster_id = 1u; remote.state.vid = 50u;
    remote.state.sequence = 1u; remote.state.alive = 1u; remote.state.health = 70;
    remote.state.x = 900.0f; remote.state.y = 300.0f; remote.state.z = 20.0f;
    remote.state.velocity = 0.03f; remote.state.direction = 64u; remote.state.moving = 1u;
    native.state = UNIT_PUPPET_READY; native.health = 88; native.behavior = 3u; native.goal = 333u;
    native.x = 50.0f; native.y = 60.0f; native.z = 0; native.velocity = 0.02f; native.moving = 1u;
    installed = selected = owned = removal_abandoned = 0;
    fail_install = fail_init = fail_spawn = fail_apply = fail_remove = fail_release = 0;
    spawns = watches = attaches = applies = health_writes = replays = 0;
    removals = native_removals = releases = unhooks = fixture_stops = 0;
    last_health = spawn_health = 0; last_replay = 0; clock_now = 0;
}
static MonsterSyncFrame step_result(DWORD now, int authority, enum ProbeResult result) {
    clock_now = now; monster_sync_publish(&remote, authority, now);
    return monster_sync_tick(&local, result, local_epoch, now / 10u, now);
}
static MonsterSyncFrame step(DWORD now, int authority) { return step_result(now, authority, PROBE_OK); }
static MonsterSyncFrame begin(enum MonsterRole role) {
    defaults(); CHECK(monster_sync_enable(1u, role));
    CHECK(step(0u, role == MONSTER_HOST).state == MS_WAITING);
    CHECK(!spawns && !watches);
    return step(2000u, role == MONSTER_HOST);
}
static void finish(void) {
    monster_sync_request_stop();
    MonsterSyncFrame f = monster_sync_tick(&local, PROBE_OK, local_epoch, 0, clock_now + 1u);
    CHECK(f.cleanup && monster_sync_stopped() && monster_sync_stop());
    CHECK(!installed && !owned && !selected);
}
static MonsterEvent event_for(uint32_t sequence, unsigned int kind) {
    MonsterEvent e = {0}; e.authority_id = remote.authority_id; e.authority_session = remote.authority_session;
    e.event.sequence = sequence; e.event.kind = kind; e.event.monster_id = 1u;
    e.event.world_low = remote.state.world_low; e.event.world_high = remote.state.world_high;
    e.event.world_epoch = remote.state.world_epoch; return e;
}
static void receive(uint32_t sequence, unsigned int kind, DWORD now) {
    MonsterEvent e = event_for(sequence, kind); monster_sync_receive(&e, now);
}
int main(void) {
    MonsterSyncFrame f = begin(MONSTER_CLIENT);
    CHECK(f.state == MS_ACTIVE && spawns == 1 && attaches == 1 && applies == 1 && health_writes == 1);
    CHECK(last_pose.x == 900.0f && last_pose.y == 300.0f && last_pose.z == 20.0f);
    CHECK(last_pose.velocity == 0.03f && last_pose.direction == 64u && last_pose.moving == 1u && last_health == 70);
    CHECK(!watches && !replays);
    local.health = 0; f = step(2010u, 0);
    CHECK(f.state == MS_ACTIVE && spawns == 1 && !native_removals && spawn_health > 0);
    receive(1u, MP_MONSTER_ATTACK, 2020u); f = step(2020u, 0);
    CHECK(replays == 1 && f.attacks == 1);
    receive(1u, MP_MONSTER_ATTACK, 2030u); receive(0u, MP_MONSTER_ATTACK, 2030u);
    MonsterEvent invalid = event_for(2u, MP_MONSTER_ATTACK); invalid.authority_session++;
    monster_sync_receive(&invalid, 2030u); invalid = event_for(2u, MP_MONSTER_ATTACK); invalid.event.world_low++;
    monster_sync_receive(&invalid, 2030u); invalid = event_for(2u, MP_MONSTER_ATTACK); invalid.event.world_epoch++;
    monster_sync_receive(&invalid, 2030u); f = step(2030u, 0);
    CHECK(replays == 1 && f.rejected == 5u);
    receive(2u, MP_MONSTER_ATTACK, 2040u); f = step(2300u, 0);
    CHECK(replays == 1 && f.rejected == 6u); /* Received event expiry, independently of fresh states. */
    CHECK(!monster_sync_stop() && !unhooks && !fixture_stops);
    monster_sync_request_stop(); CHECK(!monster_sync_stopped() && !monster_sync_stop() && !unhooks);
    finish(); CHECK(unhooks == 1 && fixture_stops == 1 && native_removals == 1);

    /* The native player/army may disappear after death while the map remains
       loaded. Neither probe result changes the monster's world identity. */
    const enum ProbeResult absent_results[] = {PROBE_NO_PLAYER, PROBE_NO_ARMY};
    for (unsigned int i = 0; i < sizeof(absent_results) / sizeof(absent_results[0]); ++i) {
        CHECK(begin(MONSTER_CLIENT).state == MS_ACTIVE);
        unsigned int before_releases = releases, before_removals = removals;
        local.player = local.army = 0; local.health = 0;
        f = step_result(2010u, 0, absent_results[i]);
        CHECK(f.state == MS_ACTIVE && owned && selected == 2 && spawns == 1);
        CHECK(releases == before_releases && removals == before_removals && !native_removals);
        CHECK(applies == 2 && health_writes == 2 && last_health == remote.state.health);
        finish();

        CHECK(begin(MONSTER_HOST).state == MS_ACTIVE);
        before_releases = releases; local.player = local.army = 0; local.health = 0;
        f = step_result(2010u, 1, absent_results[i]);
        CHECK(f.state == MS_ACTIVE && selected == 1 && watches == 1 && releases == before_releases);
        MpMonsterState retained;
        CHECK(monster_sync_take_state(&retained) && retained.alive && retained.health == 88);
        CHECK(retained.world_low == local.world_low && retained.world_high == local.world_high && retained.world_epoch == local_epoch);
        CHECK(!applies && !health_writes && !f.deaths); finish();

        /* A client can settle the world and create a replica even while its
           local player is absent; its factory only needs the live native map. */
        defaults(); local.player = local.army = 0; local.health = 0;
        CHECK(monster_sync_enable(1u, MONSTER_CLIENT));
        CHECK(step_result(0u, 0, absent_results[i]).state == MS_WAITING && !spawns);
        CHECK(step_result(2000u, 0, absent_results[i]).state == MS_ACTIVE && spawns == 1 && spawn_health > 0);
        finish();

        /* Host discovery needs a player to exclude, but its settling timer
           must survive the temporarily absent player/army. */
        defaults(); local.player = local.army = 0; local.health = 0;
        CHECK(monster_sync_enable(1u, MONSTER_HOST));
        CHECK(step_result(0u, 1, absent_results[i]).state == MS_WAITING && !watches);
        before_releases = releases;
        CHECK(step_result(2000u, 1, absent_results[i]).state == MS_WAITING && !watches && releases == before_releases);
        local.player = 20u; local.health = 100;
        CHECK(step(2010u, 1).state == MS_ACTIVE && watches == 1); finish();
    }

    /* An explicit death wins over an older alive snapshot already in flight. */
    f = begin(MONSTER_CLIENT); CHECK(f.state == MS_ACTIVE);
    receive(1u, MP_MONSTER_DEATH, 2010u); f = step(2010u, 0);
    CHECK(f.state == MS_WAITING && f.deaths == 1 && native_removals == 1 && !owned);
    f = step(2020u, 0); CHECK(f.state == MS_WAITING && spawns == 1 && !owned);
    remote.state.alive = 0; remote.state.health = 0; f = step(2030u, 0);
    CHECK(f.deaths == 1 && native_removals == 1); finish();

    f = begin(MONSTER_CLIENT); CHECK(f.state == MS_ACTIVE);
    remote.state.alive = 0; remote.state.health = 0; f = step(2010u, 0);
    CHECK(f.state == MS_WAITING && !owned && native_removals == 1 && f.deaths == 1);
    remote.state.alive = 1; remote.state.health = 70; f = step(2020u, 0);
    CHECK(spawns == 1 && !owned); finish();

    /* A late join begins at the host's event watermark rather than replaying
       attacks which finished before the client had a native replica. */
    defaults(); remote.state.event_sequence = 30u;
    CHECK(monster_sync_enable(1u, MONSTER_CLIENT)); CHECK(step(0u, 0).state == MS_WAITING);
    receive(29u, MP_MONSTER_ATTACK, 2000u); receive(30u, MP_MONSTER_ATTACK, 2000u);
    f = step(2000u, 0); CHECK(f.state == MS_ACTIVE && !replays && f.rejected == 2u);
    receive(31u, MP_MONSTER_ATTACK, 2010u); CHECK(step(2010u, 0).attacks == 1u && replays == 1); finish();

    f = begin(MONSTER_CLIENT); CHECK(f.state == MS_ACTIVE);
    for (uint32_t i = 1u; i <= 65u; ++i) receive(i, MP_MONSTER_ATTACK, 2010u);
    f = step(2010u, 0);
    CHECK(replays == 64u && f.attacks == 64u && f.dropped == 1u);
    CHECK(step(2020u, 0).dropped == 1u && replays == 64u); finish();

    f = begin(MONSTER_CLIENT); CHECK(f.state == MS_ACTIVE);
    clock_now = 3001u; f = monster_sync_tick(&local, PROBE_OK, local_epoch, 300u, clock_now);
    CHECK(f.state == MS_WAITING && !owned && native_removals == 1); /* No new publish: host expired. */
    CHECK(step(3010u, 0).state == MS_ACTIVE && spawns == 2);
    remote.present = 0; CHECK(step(3020u, 0).state == MS_WAITING && !owned); finish();

    f = begin(MONSTER_CLIENT); CHECK(f.state == MS_ACTIVE);
    /* Same map, new world epochs: remove old object and start a fresh settling window. */
    local_epoch++; remote.state.world_epoch++;
    invalid = event_for(1u, MP_MONSTER_ATTACK); invalid.event.world_epoch--;
    CHECK(step(2010u, 0).state == MS_WAITING && !owned && native_removals == 1);
    monster_sync_receive(&invalid, 4010u); f = step(4010u, 0);
    CHECK(f.state == MS_ACTIVE && spawns == 2 && !replays && f.rejected == 1); finish();

    /* Factory faults get exactly one cleanup and unlimited capped backoff. */
    defaults(); fail_spawn = 1; CHECK(monster_sync_enable(1u, MONSTER_CLIENT));
    CHECK(step(0u, 0).state == MS_WAITING);
    DWORD attempt_at = 2000u;
    const DWORD delays[] = {2000u,4000u,8000u,16000u,30000u,30000u};
    for (unsigned int i = 0; i < sizeof(delays) / sizeof(delays[0]); ++i) {
        CHECK(step(attempt_at, 0).state == MS_BACKOFF && spawns == i + 1u && native_removals == i + 1u);
        CHECK(step(attempt_at + delays[i] - 1u, 0).state == MS_BACKOFF && spawns == i + 1u);
        attempt_at += delays[i];
    }
    fail_spawn = 0; CHECK(step(attempt_at, 0).state == MS_ACTIVE && owned); finish();

    f = begin(MONSTER_CLIENT); CHECK(f.state == MS_ACTIVE);
    fail_apply = fail_remove = 1; CHECK(step(2010u, 0).state == MS_ABANDONED && native_removals == 1);
    unsigned int removal_calls = removals;
    CHECK(step(200000u, 0).state == MS_ABANDONED && removals == removal_calls && spawns == 1);
    /* The engine has torn down the old world. No second unsafe native removal. */
    owned = 0; fail_apply = fail_remove = 0; local_epoch++; remote.state.world_epoch++;
    CHECK(step(200010u, 0).state == MS_WAITING && native_removals == 1);
    CHECK(step(202010u, 0).state == MS_ACTIVE && spawns == 2); finish();

    f = begin(MONSTER_HOST); CHECK(f.state == MS_ACTIVE && watches == 1 && !spawns);
    CHECK(native.behavior == 3u && native.goal == 333u && native.velocity == 0.02f);
    CHECK(!applies && !health_writes && !replays); /* Host AI and native actions remain authoritative. */
    MpMonsterState state; MpMonsterEvent outgoing;
    CHECK(monster_sync_take_state(&state) && state.alive && state.health == 88 && state.monster_id == 1u);
    native.animation = STEAM_ANIM_ATTACK; f = step(2010u, 1);
    CHECK(!monster_sync_take_event(&outgoing) && !f.attacks); /* Animation alone is never an attack event. */
    native.captured_attacks = 2u; f = step(2020u, 1);
    CHECK(f.attacks == 2u && monster_sync_take_event(&outgoing) && outgoing.sequence == 1u && outgoing.kind == MP_MONSTER_ATTACK);
    CHECK(monster_sync_take_event(&outgoing) && outgoing.sequence == 2u && outgoing.kind == MP_MONSTER_ATTACK);
    CHECK(!monster_sync_take_event(&outgoing) && monster_sync_take_state(&state) && state.event_sequence == 2u);
    CHECK(step(2030u, 1).attacks == 2u && !monster_sync_take_event(&outgoing));
    native.captured_deaths = 1u; native.animation = STEAM_ANIM_DEATH;
    CHECK(step(2040u, 1).deaths == 1u && monster_sync_take_event(&outgoing));
    CHECK(outgoing.sequence == 3u && outgoing.kind == MP_MONSTER_DEATH);
    CHECK(monster_sync_take_state(&state) && !state.alive && !state.health && state.event_sequence == 3u);
    CHECK(step(2050u, 1).deaths == 1u && !monster_sync_take_event(&outgoing)); finish();

    f = begin(MONSTER_HOST); CHECK(f.state == MS_ACTIVE);
    native.captured_attacks = 1u; CHECK(step(2010u, 1).attacks == 1u);
    CHECK(monster_sync_take_event(&outgoing) && outgoing.sequence == 1u);
    CHECK(step(2020u, 0).state == MS_WAITING && !selected && !applies && !health_writes);
    CHECK(step(2030u, 1).state == MS_ACTIVE && watches == 2u);
    CHECK(!monster_sync_take_event(&outgoing)); /* New watch counters cannot underflow into phantom attacks. */
    native.state = UNIT_PUPPET_FAULT;
    CHECK(step(2040u, 1).state == MS_BACKOFF && !selected);
    CHECK(step(4039u, 1).state == MS_BACKOFF && watches == 2u);
    CHECK(step(4040u, 1).state == MS_ACTIVE && watches == 3u && !monster_sync_take_event(&outgoing));
    finish();

    f = begin(MONSTER_CLIENT); CHECK(f.state == MS_ACTIVE);
    fail_release = 1; monster_sync_request_stop();
    f = monster_sync_tick(&local, PROBE_OK, local_epoch, 0, 2010u);
    CHECK(!f.cleanup && !monster_sync_stopped() && !monster_sync_stop() && !unhooks && !fixture_stops);
    fail_release = 0; finish();

    /* Failed initialization can use the same truthful shutdown path and then reinstall. */
    defaults(); fail_install = 1; UnitPuppetEngine engine = api();
    CHECK(!monster_sync_initialize(1u, MONSTER_CLIENT, &engine, dummy_create));
    CHECK(monster_sync_stop() && !installed);
    defaults(); fail_init = 1;
    CHECK(!monster_sync_initialize(1u, MONSTER_CLIENT, &engine, dummy_create));
    CHECK(monster_sync_stopped() && monster_sync_stop() && !installed);
    defaults(); CHECK(monster_sync_initialize(1u, MONSTER_CLIENT, &engine, dummy_create)); finish();
    puts("Monster coordinator authority, events, death and lifecycle checks passed."); return 0;
}
