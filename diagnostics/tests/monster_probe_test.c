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
static UnitPuppetSample sample;
static unsigned int epoch, loaded, applied, released, stopped;
static int attach_ready, load_ok = 1, release_ok = 1, stop_ok = 1, bind_ok = 1;
static enum MonsterProbeKind test_kind;
/* The probe always drives puppet slot 0; the real UnitPuppet is private to
   unit_puppet.c, so this mock handle only identifies that slot. */
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
    return bind_ok;
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
    return attach_ready;
}
UnitPuppetSample unit_puppet_sample(UnitPuppet* puppet, uintptr_t game)
{
    expect_probe_slot(puppet);
    (void)game;
    return sample;
}
int unit_puppet_apply(UnitPuppet* puppet, uintptr_t game, const UnitPuppetPose* p)
{
    expect_probe_slot(puppet);
    (void)game;
    ++applied;
    sample.x = p->x;
    sample.y = p->y;
    sample.z = p->z;
    sample.velocity = p->velocity;
    sample.moving = p->moving;
    sample.direction = p->direction;
    sample.animation = p->moving ? 2u : 0u;
    return 1;
}
int unit_puppet_release(UnitPuppet* puppet, uintptr_t game)
{
    expect_probe_slot(puppet);
    (void)game;
    ++released;
    return release_ok;
}
int unit_puppet_stop(void)
{
    ++stopped;
    return stop_ok;
}
int unit_puppet_replay(UnitPuppet* puppet, uintptr_t game, uint32_t sequence,
                       unsigned int animation)
{
    expect_probe_slot(puppet);
    (void)game;
    (void)sequence;
    (void)animation;
    return 0;
}
int unit_puppet_test_damage(UnitPuppet* puppet, uintptr_t game)
{
    expect_probe_slot(puppet);
    (void)game;
    return 0;
}
int unit_combat_bind(uintptr_t base, ActorCreate* out)
{
    (void)base;
    (void)out;
    return 0;
}
int unit_combat_init(uintptr_t base, ActorCreate c, ActorDestroy d, UnitAnimation a)
{
    (void)base;
    (void)c;
    (void)d;
    (void)a;
    return 0;
}
int unit_combat_spawn(const Snapshot* local_sample)
{
    (void)local_sample;
    return 0;
}
UnitCombatSample unit_combat_sample(uintptr_t game, int compare_sources)
{
    UnitCombatSample s = {0};
    (void)game;
    (void)compare_sources;
    return s;
}
int unit_combat_remove(uintptr_t game)
{
    (void)game;
    return 1;
}
int unit_combat_stop(void)
{
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
    ++loaded;
    if (strcmp(map, "maps\\survive_01.map"))
        return 0;
    ++epoch;
    return load_ok;
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
static Snapshot local;
static MonsterProbeFrame tick(DWORD now, int stop)
{
    return monster_probe_tick(&local, PROBE_OK, now, stop);
}
static void reset(void)
{
    memset(&sample, 0, sizeof(sample));
    sample.state = UNIT_PUPPET_ACTIVE;
    sample.frame_changes = sample.decisions = sample.movements = 10;
    memset(&local, 0, sizeof(local));
    local.game = 1;
    local.player = 2;
    local.in_level = 1;
    local.health = 100;
    local.x = 200;
    local.y = 300;
    local.world_low = 42;
    loaded = applied = released = stopped = epoch = 0;
    attach_ready = 1;
    sample.z = test_kind == MONSTER_PROBE_FLYING ? 80.0f : 0;
    sample.vid = test_kind == MONSTER_PROBE_GROUND ? 50u : 290u;
}
static int enable(void)
{
    return monster_probe_enable_kind(123, test_kind);
}
int main(int argc, char** argv)
{
    test_kind =
        argc == 2 && !strcmp(argv[1], "flying") ? MONSTER_PROBE_FLYING : MONSTER_PROBE_GROUND;
    reset();
    CHECK(enable());
    CHECK(!enable());
    CHECK(tick(100, 0).state == MONSTER_PROBE_MENU && !loaded);
    CHECK(tick(2099, 0).state == MONSTER_PROBE_MENU && !loaded);
    CHECK(tick(2100, 0).state == MONSTER_PROBE_SEARCH && loaded == 1);
    CHECK(tick(2200, 0).state == MONSTER_PROBE_RUNNING && applied == 1 && sample.x == 280);
    CHECK(sample.z == (test_kind == MONSTER_PROBE_FLYING ? 80.0f : 0.0f));
    CHECK(tick(7200, 0).phase == 1 && sample.moving && sample.animation == 2 &&
          sample.direction == 64);
    CHECK(tick(12200, 0).phase == 2 && !sample.moving && sample.x == 380);
    CHECK(tick(17200, 0).phase == 3 && sample.moving && sample.direction == 192);
    CHECK(tick(22200, 0).phase == 4 && !sample.moving && sample.direction == 128);
    CHECK(tick(27200, 0).phase == 5 && !sample.moving);
    MonsterProbeFrame f = tick(32200, 0);
    CHECK(f.state == MONSTER_PROBE_DONE && f.phase == 6 && !f.violations && f.cleanup &&
          released == 1);
    CHECK(tick(33000, 0).state == MONSTER_PROBE_DONE && released == 1);
    CHECK(monster_probe_stop());
    reset();
    CHECK(enable());
    tick(100, 0);
    tick(2100, 0);
    tick(2200, 0);
    sample.x += 1; /* Drift must be detected before next apply corrects it. */
    CHECK(tick(2300, 0).violations == 1 && sample.x == 280);
    CHECK(tick(32200, 0).state == MONSTER_PROBE_FAILED);
    CHECK(monster_probe_stop());
    reset();
    CHECK(enable());
    tick(100, 0);
    tick(2100, 0);
    tick(2200, 0);
    ++epoch; /* Same GAME and same path, different world. */
    CHECK(tick(2300, 0).state == MONSTER_PROBE_FAILED && released == 1);
    CHECK(monster_probe_stop());
    reset();
    CHECK(enable());
    tick(100, 0);
    tick(2100, 0);
    tick(2200, 0);
    sample.frame_changes = 0;
    sample.frame_last = 1; /* Multi-frame art must actually progress. */
    CHECK(tick(32200, 0).state == MONSTER_PROBE_FAILED);
    CHECK(monster_probe_stop());
    if (test_kind == MONSTER_PROBE_FLYING)
    {
        reset();
        sample.frame_changes = 0;
        CHECK(enable());
        tick(100, 0);
        tick(2100, 0);
        tick(2200, 0);
        CHECK(tick(32200, 0).state == MONSTER_PROBE_DONE); /* Native single-frame art. */
        CHECK(monster_probe_stop());
        reset();
        sample.frame_changes = 0;
        sample.child_frame_last = 3;
        CHECK(enable());
        tick(100, 0);
        tick(2100, 0);
        tick(2200, 0);
        CHECK(tick(32200, 0).state == MONSTER_PROBE_FAILED); /* Animated child frozen. */
        CHECK(monster_probe_stop());
        reset();
        sample.frame_changes = 0;
        sample.child_frame_last = 3;
        sample.child_frame_changes = 10;
        CHECK(enable());
        tick(100, 0);
        tick(2100, 0);
        tick(2200, 0);
        CHECK(tick(32200, 0).state == MONSTER_PROBE_DONE); /* Child owns animation. */
        CHECK(monster_probe_stop());
    }
    reset();
    CHECK(enable());
    tick(100, 0);
    tick(2100, 0);
    tick(2200, 0);
    release_ok = 0;
    CHECK(tick(2300, 1).state == MONSTER_PROBE_FAILED);
    stop_ok = 0;
    CHECK(!monster_probe_stop());
    stop_ok = release_ok = 1;
    CHECK(monster_probe_stop());
    reset();
    CHECK(enable());
    CHECK(tick(100, 1).state == MONSTER_PROBE_DONE && !loaded);
    CHECK(monster_probe_stop());
    reset();
    attach_ready = 0;
    CHECK(enable());
    tick(100, 0);
    tick(2100, 0);
    CHECK(tick(47200, 0).state == MONSTER_PROBE_FAILED && !applied);
    CHECK(monster_probe_stop());
    reset();
    load_ok = 0;
    CHECK(enable());
    tick(100, 0);
    CHECK(tick(2100, 0).state == MONSTER_PROBE_FAILED);
    CHECK(monster_probe_stop());
    load_ok = 1;
    bind_ok = 0;
    CHECK(!enable());
    CHECK(monster_probe_stop());
    bind_ok = 1;
    test_kind = MONSTER_PROBE_FLYING_RELOAD;
    reset();
    CHECK(enable());
    tick(100, 0);
    tick(2100, 0);
    tick(2200, 0);
    unsigned int before_reload = applied;
    f = tick(12200, 0);
    CHECK(f.state == MONSTER_PROBE_DONE && f.reloaded && f.cleanup && f.phase == 2 && loaded == 2);
    CHECK(applied == before_reload && released == 1); /* No old pose after load. */
    CHECK(tick(22200, 0).state == MONSTER_PROBE_DONE && loaded == 2);
    CHECK(monster_probe_stop());
    reset();
    CHECK(enable());
    tick(100, 0);
    tick(2100, 0);
    tick(2200, 0);
    load_ok = 0;
    CHECK(tick(12200, 0).state == MONSTER_PROBE_FAILED);
    load_ok = 1;
    CHECK(monster_probe_stop());
    test_kind = MONSTER_PROBE_FLYING;
    reset();
    sample.vid = 50;
    CHECK(enable());
    tick(100, 0);
    tick(2100, 0);
    CHECK(tick(2200, 0).state == MONSTER_PROBE_FAILED && !applied && released == 1);
    CHECK(monster_probe_stop());
    CHECK(!wrong_slot_calls);
    puts("Monster probe phases, drift, epoch and cleanup checks passed.");
    return 0;
}
