#include <stdio.h>
#include <string.h>
#include "../src/monster_probe.h"
#include "../../asmp-dll/src/game/ui.h"
#include "../../asmp-dll/src/game/world_hook.h"

#define CHECK(c)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(c))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);                                   \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

/* The controller owns sequencing only. These doubles expose native observations
   explicitly, so replay cannot manufacture animation progress or deletion. */
static Snapshot local;
static UnitPuppetSample unit;
static UnitCombatSample combat;
static unsigned int epoch, loads, spawns, samples, applies, releases, removes;
static unsigned int puppet_stops, combat_stops, damage_calls, replay_calls;
static unsigned int attack_calls, death_calls, last_sequence;
static int replay_ok, damage_ok, remove_ok, release_ok, spawn_ok;

/* The probe always drives slot 0 of each unit module; the real structs are
   private to their modules, so these mock handles only identify that slot. */
struct UnitPuppet
{
    int unused;
};
static UnitPuppet probe_puppet;
static unsigned int wrong_slot_calls;
UnitPuppet* unit_puppet_slot(unsigned int index)
{
    return index == 0 ? &probe_puppet : NULL;
}
static void expect_probe_slot(const UnitPuppet* puppet)
{
    if (puppet != &probe_puppet)
        ++wrong_slot_calls;
}
int unit_puppet_bind(uintptr_t base, UnitPuppetEngine* out)
{
    (void)base;
    memset(out, 0, sizeof(*out));
    return 1;
}
int unit_puppet_install(uintptr_t base, const UnitPuppetEngine* api)
{
    (void)base;
    (void)api;
    return 1;
}
int unit_puppet_attach_first(UnitPuppet* puppet, uintptr_t game, uintptr_t player)
{
    expect_probe_slot(puppet);
    (void)game;
    (void)player;
    return 1;
}
UnitPuppetSample unit_puppet_sample(UnitPuppet* puppet, uintptr_t game)
{
    expect_probe_slot(puppet);
    (void)game;
    return unit;
}
int unit_puppet_apply(UnitPuppet* puppet, uintptr_t game, const UnitPuppetPose* pose)
{
    expect_probe_slot(puppet);
    (void)game;
    (void)pose;
    ++applies;
    return 1;
}
int unit_puppet_test_damage(UnitPuppet* puppet, uintptr_t game)
{
    expect_probe_slot(puppet);
    (void)game;
    ++damage_calls;
    return damage_ok;
}
int unit_puppet_replay(UnitPuppet* puppet, uintptr_t game, uint32_t sequence,
                       unsigned int animation)
{
    expect_probe_slot(puppet);
    (void)game;
    ++replay_calls;
    if (animation == STEAM_ANIM_ATTACK)
        ++attack_calls;
    else if (animation == STEAM_ANIM_DEATH)
        ++death_calls;
    else
        return -1;
    if (!replay_ok)
        return -1;
    if (sequence <= last_sequence)
    {
        ++unit.duplicates;
        return 0;
    }
    last_sequence = sequence;
    ++unit.replayed;
    unit.animation = unit.child_animation = animation;
    return 1;
}
int unit_puppet_release(UnitPuppet* puppet, uintptr_t game)
{
    expect_probe_slot(puppet);
    (void)game;
    ++releases;
    return release_ok;
}
int unit_puppet_stop(void)
{
    ++puppet_stops;
    return 1;
}
struct UnitCombat
{
    int unused;
};
static UnitCombat probe_fixture;
UnitCombat* unit_combat_slot(unsigned int index)
{
    return index == 0 ? &probe_fixture : NULL;
}
static void expect_probe_fixture(const UnitCombat* fixture)
{
    if (fixture != &probe_fixture)
        ++wrong_slot_calls;
}
int unit_combat_bind(uintptr_t base, ActorCreate* create)
{
    (void)base;
    *create = NULL;
    return 1;
}
int unit_combat_init(uintptr_t base, ActorCreate create, ActorDestroy destroy,
                     UnitAnimation animation)
{
    (void)base;
    (void)create;
    (void)destroy;
    (void)animation;
    return 1;
}
int unit_combat_spawn(UnitCombat* fixture, const Snapshot* snapshot)
{
    expect_probe_fixture(fixture);
    (void)snapshot;
    ++spawns;
    return spawn_ok;
}
UnitCombatSample unit_combat_sample(UnitCombat* fixture, uintptr_t game, int compare_sources)
{
    expect_probe_fixture(fixture);
    (void)game;
    (void)compare_sources;
    ++samples;
    return combat;
}
int unit_combat_remove(UnitCombat* fixture, uintptr_t game)
{
    expect_probe_fixture(fixture);
    (void)game;
    ++removes;
    return remove_ok;
}
int unit_combat_stop(void)
{
    ++combat_stops;
    return 1;
}
int ui_bind(uintptr_t base)
{
    (void)base;
    return 1;
}
void ui_stop(void)
{
}
int ui_load_map(uintptr_t game, const char* map)
{
    (void)game;
    ++loads;
    if (strcmp(map, "maps\\survive_01.map"))
        return 0;
    ++epoch;
    return 1;
}
int world_hook_install(uintptr_t base)
{
    (void)base;
    return 1;
}
int world_hook_stop(void)
{
    return 1;
}
uint32_t world_hook_generation(void)
{
    return epoch;
}

static MonsterProbeFrame tick(DWORD now, int stop)
{
    /* Phase timestamps below begin after the separate two-second load settle. */
    return monster_probe_tick(&local, PROBE_OK, now >= 2200u ? now + 2000u : now, stop);
}
static void reset(void)
{
    memset(&local, 0, sizeof(local));
    local.game = 1;
    local.player = 2;
    local.in_level = 1;
    local.health = 100;
    local.world_low = 42;
    memset(&unit, 0, sizeof(unit));
    unit.state = UNIT_PUPPET_ACTIVE;
    unit.entity = 3;
    unit.child = 4;
    unit.vid = 50;
    unit.class_id = 2;
    unit.health = 100;
    unit.animation = unit.child_animation = STEAM_ANIM_IDLE;
    memset(&combat, 0, sizeof(combat));
    combat.root = 3;
    combat.child = 4;
    combat.private_entities = 2;
    combat.other_entities = 7;
    combat.sources_unchanged = 1;
    epoch = loads = spawns = samples = applies = releases = removes = 0;
    puppet_stops = combat_stops = damage_calls = replay_calls = 0;
    attack_calls = death_calls = last_sequence = 0;
    replay_ok = damage_ok = remove_ok = release_ok = spawn_ok = 1;
}
static int start(void)
{
    reset();
    if (!monster_probe_enable_kind(123, MONSTER_PROBE_COMBAT))
        return 0;
    if (tick(100, 0).state != MONSTER_PROBE_MENU || loads)
        return 0;
    if (tick(2100, 0).state != MONSTER_PROBE_SEARCH || loads != 1)
        return 0;
    if (monster_probe_tick(&local, PROBE_OK, 2200u, 0).state != MONSTER_PROBE_SEARCH || spawns)
        return 0;
    if (monster_probe_tick(&local, PROBE_OK, 4199u, 0).state != MONSTER_PROBE_SEARCH || spawns)
        return 0;
    MonsterProbeFrame f = tick(2200, 0);
    return f.state == MONSTER_PROBE_RUNNING && f.phase == MONSTER_COMBAT_IDLE && f.combat_event &&
           !f.violations && spawns == 1 && !replay_calls && !applies;
}
static int attack(void)
{
    MonsterProbeFrame f = tick(5200, 0);
    if (f.state != MONSTER_PROBE_RUNNING || f.phase != MONSTER_COMBAT_DAMAGE || !f.damage_blocked ||
        damage_calls != 1)
        return 0;
    f = tick(7200, 0);
    return f.state == MONSTER_PROBE_RUNNING && f.phase == MONSTER_COMBAT_ATTACK &&
           f.unit.animation == STEAM_ANIM_ATTACK && unit.replayed == 1 && unit.duplicates == 1 &&
           attack_calls == 2 && !death_calls;
}
static int recover(void)
{
    ++unit.frame_changes;
    ++unit.attack_frame_changes;
    unit.animation = unit.child_animation = STEAM_ANIM_IDLE;
    MonsterProbeFrame f = tick(10200, 0);
    return f.state == MONSTER_PROBE_RUNNING && f.phase == MONSTER_COMBAT_RECOVER && !f.violations;
}
static int failed(MonsterProbeFrame f)
{
    return f.state == MONSTER_PROBE_FAILED && f.cleanup && releases == 1 && removes == 1;
}

int main(void)
{
    MonsterProbeFrame f;
    CHECK(start());
    CHECK(!monster_probe_enable_kind(123, MONSTER_PROBE_COMBAT));
    CHECK(tick(5199, 0).phase == MONSTER_COMBAT_IDLE && !damage_calls);
    CHECK(attack());
    CHECK(tick(7300, 0).phase == MONSTER_COMBAT_ATTACK && attack_calls == 2 && damage_calls == 1);
    CHECK(recover());
    f = tick(12200, 0);
    CHECK(f.state == MONSTER_PROBE_RUNNING && f.phase == MONSTER_COMBAT_DEATH);
    CHECK(f.unit.animation == STEAM_ANIM_DEATH && unit.replayed == 2 && unit.duplicates == 2);
    CHECK(death_calls == 2 && replay_calls == 4);
    CHECK(tick(12300, 0).state == MONSTER_PROBE_RUNNING && death_calls == 2);
    unit.state = UNIT_PUPPET_LOST;
    combat.private_entities = 0;
    f = tick(12400, 0);
    CHECK(f.state == MONSTER_PROBE_DONE && f.phase == MONSTER_COMBAT_DISPOSED && f.cleanup &&
          f.damage_blocked && !f.violations && releases == 1 && removes == 1);
    CHECK(tick(13000, 0).state == MONSTER_PROBE_DONE && removes == 1 && releases == 1);
    CHECK(!applies && spawns == 1);
    CHECK(monster_probe_stop());
    CHECK(puppet_stops == 1 && combat_stops == 1);

    CHECK(start());
    unit.state = UNIT_PUPPET_LOST;
    combat.private_entities = 0;
    CHECK(failed(tick(2300, 0)) && !replay_calls);
    CHECK(monster_probe_stop());
    CHECK(start());
    --local.health;
    CHECK(failed(tick(2300, 0)) && !replay_calls);
    CHECK(monster_probe_stop());
    CHECK(start());
    combat.sources_unchanged = 0;
    CHECK(failed(tick(2300, 0)));
    CHECK(monster_probe_stop());
    CHECK(start());
    ++combat.unexpected_entities;
    CHECK(failed(tick(2300, 0)));
    CHECK(monster_probe_stop());
    CHECK(start());
    --combat.other_entities; /* An existing ambient object may expire. */
    CHECK(tick(2300, 0).state == MONSTER_PROBE_RUNNING);
    CHECK(tick(2400, 1).cleanup);
    CHECK(monster_probe_stop());
    CHECK(start());
    combat.fault = 1;
    CHECK(failed(tick(2300, 0)));
    CHECK(monster_probe_stop());
    CHECK(start());
    unit.child_command = 1;
    CHECK(failed(tick(2300, 0)));
    CHECK(monster_probe_stop());
    CHECK(start());
    unit.animation = STEAM_ANIM_ATTACK;
    CHECK(failed(tick(2300, 0)));
    CHECK(monster_probe_stop());
    CHECK(start());
    unit.x += 1.0f;
    CHECK(failed(tick(2300, 0)));
    CHECK(monster_probe_stop());

    /* Returning to idle without any native frames does not prove an attack. */
    CHECK(start());
    CHECK(attack());
    unit.animation = unit.child_animation = STEAM_ANIM_IDLE;
    CHECK(failed(tick(10200, 0)));
    CHECK(monster_probe_stop());
    /* Frames progressing while the native action remains stuck also fails. */
    CHECK(start());
    CHECK(attack());
    ++unit.child_frame_changes;
    ++unit.attack_frame_changes;
    CHECK(failed(tick(10200, 0)));
    CHECK(monster_probe_stop());
    CHECK(start());
    CHECK(attack());
    ++unit.child_frame_changes;
    ++unit.attack_frame_changes;
    unit.animation = unit.child_animation = STEAM_ANIM_IDLE;
    CHECK(tick(10200, 0).state == MONSTER_PROBE_RUNNING);
    CHECK(monster_probe_stop());

    CHECK(start());
    CHECK(attack());
    CHECK(recover());
    CHECK(tick(12200, 0).state == MONSTER_PROBE_RUNNING);
    CHECK(failed(tick(22201, 0)) && death_calls == 2);
    CHECK(monster_probe_stop());
    CHECK(start());
    CHECK(attack());
    f = tick(7300, 1);
    CHECK(f.state == MONSTER_PROBE_DONE && f.cleanup && releases == 1 && removes == 1);
    CHECK(tick(7400, 1).state == MONSTER_PROBE_DONE && removes == 1 && replay_calls == 2);
    CHECK(monster_probe_stop());
    CHECK(start());
    CHECK(attack());
    ++epoch;
    CHECK(failed(tick(10200, 0)) && replay_calls == 2 && !death_calls);
    CHECK(monster_probe_stop());

    CHECK(start());
    damage_ok = 0;
    CHECK(failed(tick(5200, 0)));
    CHECK(monster_probe_stop());
    CHECK(start());
    tick(5200, 0);
    replay_ok = 0;
    CHECK(failed(tick(7200, 0)));
    CHECK(monster_probe_stop());
    CHECK(start());
    CHECK(attack());
    CHECK(recover());
    ++unit.duplicates;
    CHECK(failed(tick(10300, 0)));
    CHECK(monster_probe_stop());
    CHECK(start());
    CHECK(attack()); /* Missing recover observation cannot authorize death. */
    CHECK(failed(tick(12200, 0)) && !death_calls);
    CHECK(monster_probe_stop());
    CHECK(start());
    remove_ok = 0;
    CHECK(tick(2300, 1).state == MONSTER_PROBE_FAILED && removes == 1);
    CHECK(monster_probe_stop());

    CHECK(!wrong_slot_calls);
    puts("Monster combat controller phases, native progress, isolation and cleanup checks passed.");
    return 0;
}
