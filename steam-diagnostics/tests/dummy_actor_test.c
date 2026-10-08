#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../src/dummy_actor.h"

static unsigned char game[0x300], army[0x20], player[0xBC], vid[0x490], entity[0xBC];
static unsigned char torso[0x70], weapon[8];
static uintptr_t entries[4], image_base;
static unsigned int created, destroyed, moved, failures;
static DummyActor actor;
static Snapshot local;
static void check(int condition, const char* label)
{
    if (!condition) { fprintf(stderr, "FAIL %s\n", label); ++failures; }
}
static void* __fastcall create_actor(void* g, void* unused, void* descriptor,
    float x, float y, float z, int direction, void* parent)
{
    (void)unused; (void)parent;
    check(g == game && descriptor == actor.vid, "factory object and private descriptor");
    check(*(int32_t*)((unsigned char*)descriptor + 0x440) == -1 &&
        *(int32_t*)((unsigned char*)descriptor + 0x44C) == -1, "disable local creation/deletion scripts");
    check(*(unsigned int*)((unsigned char*)descriptor + 0x3A8) == 0, "private counters start empty");
    memset(entity, 0, sizeof(entity));
    *(uintptr_t*)entity = image_base + STEAM_MAN_VTABLE_RVA;
    *(uintptr_t*)(entity + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)descriptor;
    *(float*)(entity + 0x30) = x; *(float*)(entity + 0x34) = y; *(float*)(entity + 0x38) = z;
    entity[0x50] = (unsigned char)direction;
    *(uintptr_t*)(entity + 0x40) = (uintptr_t)torso;
    *(uintptr_t*)(torso + 0x1C) = (uintptr_t)weapon;
    *(uint32_t*)(entity + 8) = 1000; *(uint32_t*)(entity + 0xC) = 1000;
    *(uint32_t*)(entity + 0x10) = 1009;
    *(uint32_t*)(entity + STEAM_ENTITY_FLAGS_OFFSET) = 0x3000u;
    entries[1] = (uintptr_t)entity; *(unsigned int*)(game + 0x60) = 2;
    ++created;
    return entity;
}
static void* __fastcall destroy_actor(void* e, void* unused, int release)
{
    (void)unused;
    check(e == entity && release == 1 && entries[1] == (uintptr_t)e, "destroy only registered owned actor");
    entries[1] = 0; ++destroyed;
    return e;
}
static void __fastcall move_actor(void* e, void* unused, float x, float y, float z)
{
    (void)unused;
    check(e == entity && entries[1] == (uintptr_t)e, "move only a registered actor");
    *(float*)(entity + 0x30) = x; *(float*)(entity + 0x34) = y; *(float*)(entity + 0x38) = z;
    ++moved;
}
static unsigned char __fastcall rotate_actor(void* e, void* unused, unsigned int direction)
{
    (void)unused; check(e == entity || e == torso, "rotation object");
    ((unsigned char*)e)[0x50] = (unsigned char)direction;
    if (e == entity) torso[0x50] = (unsigned char)direction;
    return 1;
}
static ActorResult step(DWORD time, int stop)
{
    return dummy_actor_tick(&actor, image_base, &local, PROBE_OK, time, stop);
}
static void reset(void)
{
    ActorEngine engine = {create_actor, destroy_actor, move_actor, rotate_actor};
    dummy_actor_init(&actor, &engine);
    created = destroyed = moved = 0;
    memset(game, 0, sizeof(game)); memset(army, 0, sizeof(army)); memset(vid, 0, sizeof(vid));
    *(uintptr_t*)(image_base + STEAM_GAME_PTR_RVA) = (uintptr_t)game;
    *(uintptr_t*)game = image_base + STEAM_GAME_VTABLE_RVA;
    *(const char**)(game + 0x20) = "maps\\Level_01.map";
    *(unsigned int*)(game + 0x60) = 1; *(unsigned int*)(game + 0x64) = 4;
    *(uintptr_t**)(game + 0x68) = entries; entries[0] = (uintptr_t)player; entries[1] = 0;
    *(uintptr_t*)(player + 0x1C) = (uintptr_t)vid; *(uintptr_t*)(player + 0x40) = 1;
    *(unsigned int*)(vid + 0x10) = 7;
    *(unsigned int*)(vid + 0x3A8) = 1;
    *(uintptr_t*)(vid + 0x5C) = (uintptr_t)weapon;
    *(uintptr_t*)(army + STEAM_ARMY_PLAYER_OFFSET) = (uintptr_t)player;
    local.game = (uintptr_t)game; local.army = (uintptr_t)army; local.player = (uintptr_t)player;
    local.health = 110; local.x = 200; local.y = 300; local.z = 0;
    local.animation = 2; local.direction = 168;
    local.velocity = 0.125f; local.moving = 1;
    local.torso_present = 1; local.torso_direction = 220;
}
int main(void)
{
    image_base = (uintptr_t)VirtualAlloc(NULL, 0x276000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!image_base) return 2;
    ActorEngine invalid;
    check(!actor_engine_bind(image_base, &invalid) && !invalid.create, "reject an unsupported mapped image");
    reset();
    check(step(100, 0).event == ACTOR_NONE && !created, "wait for stable gameplay");
    check(step(2099, 0).event == ACTOR_NONE && !created, "wait two seconds");
    check(step(2100, 0).event == ACTOR_SPAWNED && created == 1, "spawn once on a level");
    local.x = 400; step(2120, 0);
    check(moved == 1 && *(float*)(entity + 0x30) == 480 && entity[0x50] == 168 &&
        *(float*)(entity + STEAM_ENTITY_VELOCITY_OFFSET) == 0.125f &&
        *(uint32_t*)(entity + STEAM_ENTITY_FLAGS_OFFSET) == 0x3080u,
        "movement speed, intent, full byte direction, preserve unrelated flags");
    check(*(uint32_t*)(entity + 0x4C) == 0 && *(uint32_t*)(entity + 0xC) == 1000 && torso[0x50] == 220,
        "leave animation selection and cursor to engine, apply independent torso aim");
    /* Stand in for native advancement between snapshots. Replication must not
       change this cursor or its timing even if the source has another phase. */
    *(uint32_t*)(entity + 0x4C) = 2;
    *(uint32_t*)(entity + 0xC) = 1006;
    *(uint32_t*)(entity + 0x14) = 2077;
    *(uint32_t*)(entity + 0x18) = 2099;
    *(uint32_t*)(player + 0xC) = 3001;
    local.torso_direction = 35; step(2130, 0); step(2140, 0);
    check(*(uint32_t*)(entity + 0xC) == 1006 && *(uint32_t*)(entity + 0x14) == 2077 &&
        *(uint32_t*)(entity + 0x18) == 2099 && torso[0x50] == 35,
        "repeated running snapshots preserve native cursor and timers");
    local.moving = 0; local.velocity = 0.0625f; step(2150, 0);
    check(*(uint32_t*)(entity + STEAM_ENTITY_FLAGS_OFFSET) == 0x3000u &&
        *(float*)(entity + STEAM_ENTITY_VELOCITY_OFFSET) == 0.0625f,
        "release run intent while retaining deceleration speed");
    local.velocity = 0; step(2160, 0);
    check(*(float*)(entity + STEAM_ENTITY_VELOCITY_OFFSET) == 0 &&
        *(uint32_t*)(entity + 0x4C) == 2 && *(uint32_t*)(entity + 0xC) == 1006,
        "stop supplies state while native engine owns transition to idle");
    check(*(uintptr_t*)(army + 0x10) == (uintptr_t)player && *(unsigned int*)(vid + 0x3A8) == 1,
        "preserve local player and original descriptor");
    check(step(62100, 0).event == ACTOR_REMOVED && destroyed == 1, "timed engine cleanup");
    step(70000, 0); check(created == 1, "do not respawn after timeout in same level");
    reset(); step(100, 0); step(2100, 0);
    check(step(2200, 1).event == ACTOR_REMOVED && destroyed == 1, "stop request removes on tick");
    reset(); step(100, 0); step(2100, 0);
    *(const char**)(game + 0x20) = "maps\\shop.map";
    check(step(2200, 0).event == ACTOR_REMOVED && destroyed == 1, "remove on transition to shop");
    step(5000, 0); check(created == 1, "never spawn inside shop");
    reset(); step(100, 0); step(2100, 0);
    entries[1] = 0;
    check(step(2200, 0).event == ACTOR_LOST && !destroyed && !moved, "discard entity removed by engine");
    reset(); step(100, 0); step(2100, 0);
    *(uintptr_t*)(entity + 0x1C) = (uintptr_t)vid;
    check(step(2200, 0).event == ACTOR_LOST && !destroyed && !moved, "reject reused entity address with foreign VID");
    reset(); *(unsigned int*)(vid + 0x10) = 19; step(100, 0);
    ActorResult invalid_class = step(2100, 0);
    check(invalid_class.event == ACTOR_REJECTED && invalid_class.reason == ACTOR_REASON_VID_CLASS &&
        invalid_class.source_class == 19 && !created, "reject non-MAN VID class with diagnostic reason");
    reset(); *(unsigned int*)(vid + 0x38C) = 100; step(100, 0);
    check(step(2100, 0).event == ACTOR_REJECTED && !created, "reject invalid list category");
    reset(); *(const char**)(game + 0x20) = "maps\\mainmenu.map";
    step(100, 0); step(3000, 0); check(!created, "never spawn in menu");
    check(dummy_actor_tick(&actor, image_base, &local, PROBE_NO_PLAYER, 4000, 0).event == ACTOR_NONE,
        "no player during loading");
    VirtualFree((void*)image_base, 0, MEM_RELEASE);
    if (!failures) puts("PASS dummy actor: motion intent/speed, independent aim, native animation ownership, lifecycle and stale pointers");
    return failures ? 1 : 0;
}
