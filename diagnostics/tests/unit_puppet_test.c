#include <stdio.h>
#include <string.h>
#include "../../asmp-dll/src/game/unit_puppet.h"
#define CHECK(c)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(c))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);                                   \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static unsigned char game[0x2000], unit[0x94], other[0x94], vid[0x490], child[0x94],
    child_vid[0x490];
static unsigned char frozen_stock[STEAM_VID_SIZE], frozen_private[STEAM_VID_SIZE];
static int change_fault, change_private_seen;
static int predictive_native, nested_freeze, recursive_form_cleanup;
static unsigned char prediction_behavior[STEAM_BEHAVIOR_SIZE];
static uintptr_t entries[4];
static unsigned int actions, moves, destroyed, animations, clears, stops;
static void __fastcall animation(void* e, void* unused, unsigned int a);
static int __fastcall original_action(void* e, void* unused, unsigned int k, intptr_t a, intptr_t b,
                                      intptr_t c)
{
    (void)unused;
    (void)b;
    (void)c;
    ++actions;
    unsigned char* p = e;
    if (k == STEAM_ACT_CHANGE_VID)
    {
        uintptr_t descriptor =
            *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + (unsigned int)a * 4u);
        if (descriptor == (uintptr_t)frozen_private)
            ++change_private_seen;
        if (change_fault)
            RaiseException(0xE1234567u, 0, 0, NULL);
        *(uintptr_t*)(p + STEAM_ENTITY_VID_OFFSET) = descriptor;
        *(uintptr_t*)(p + STEAM_ENTITY_CHILD_OFFSET) = 0;
        *(unsigned int*)(p + STEAM_ENTITY_ANIM_OFFSET) = (unsigned int)b;
        if (recursive_form_cleanup)
        {
            ActorAction action = ((ActorAction*)*(uintptr_t*)p)[1];
            action(e, NULL, STEAM_ACT_SET_ANIMATION, STEAM_ANIM_IDLE, 0, 0);
        }
    }
    if (k == STEAM_ACT_SET_BEHAVIOR)
        *(unsigned int*)(p + STEAM_UNIT_BEHAVIOR_OFFSET) = (unsigned int)a;
    if (k == STEAM_ACT_CLEAR_COMMAND)
    {
        *(uintptr_t*)(p + STEAM_ENTITY_GOAL_OFFSET) = 0;
        *(unsigned int*)(p + STEAM_ENTITY_FLAGS_OFFSET) &= ~STEAM_ENTITY_COMMAND_MASK;
        ++clears;
    }
    if (k == STEAM_ACT_CLEAR_STACK)
    {
        *(unsigned int*)(p + STEAM_ENTITY_STACK_COUNT_OFFSET) = 0;
        ++clears;
    }
    if (k == STEAM_ACT_STOP)
    {
        *(float*)(p + STEAM_ENTITY_VELOCITY_OFFSET) = 0;
        *(unsigned int*)(p + STEAM_ENTITY_FLAGS_OFFSET) &= ~STEAM_ENTITY_MOVING_FLAG;
        ++stops;
    }
    if (k == STEAM_ACT_DAMAGE)
    {
        if (predictive_native)
        {
            ActorAction action = ((ActorAction*)*(uintptr_t*)p)[1];
            int health = *(int*)(p + STEAM_ENTITY_HEALTH_OFFSET);
            if (a >= health)
                action(e, NULL, STEAM_ACT_SET_HP, 0, 0, 0);
            else
            {
                *(int*)(p + STEAM_ENTITY_HEALTH_OFFSET) = health - (int)a;
                *(unsigned int*)(p + STEAM_ENTITY_ANIM_OFFSET) = 13u;
            }
            if (nested_freeze)
            {
                action(e, NULL, STEAM_ACT_CHANGE_VID, 59u, STEAM_ANIM_IDLE, 0);
                action(e, NULL, STEAM_ACT_SET_ANIMATION, STEAM_ANIM_IDLE, 0, 0);
                action(e, NULL, STEAM_ACT_SET_BEHAVIOR, 0, 0, 0);
                action(e, NULL, STEAM_ACT_CLEAR_COMMAND, 0, 0, 0);
                action(e, NULL, STEAM_ACT_CLEAR_STACK, 0, 0, 0);
                action(e, NULL, STEAM_ACT_STOP, 1, 0, 0);
            }
        }
        else
            *(int*)(p + STEAM_ENTITY_HEALTH_OFFSET) -= (int)a;
    }
    if (k == STEAM_ACT_SET_ANIMATION)
        animation(e, NULL, (unsigned int)a);
    if (k == STEAM_ACT_SET_HP)
    {
        /* Native lethal SetHP retains prelethal positive health. */
        if (!predictive_native || a > 0)
            *(int*)(p + STEAM_ENTITY_HEALTH_OFFSET) = (int)a;
        if (a <= 0)
            *(unsigned int*)(p + STEAM_ENTITY_ANIM_OFFSET) = STEAM_ANIM_DEATH;
    }
    return 77;
}
static void __fastcall original_movement(void* e, void* unused)
{
    (void)e;
    (void)unused;
    ++moves;
}
static void* __fastcall original_destroy(void* e, void* unused, int release)
{
    (void)unused;
    (void)release;
    ++destroyed;
    return e;
}
static void __fastcall animation(void* e, void* unused, unsigned int a)
{
    (void)unused;
    ++animations;
    if (*(unsigned int*)((unsigned char*)e + STEAM_ENTITY_ANIM_OFFSET) != a)
    {
        *(unsigned int*)((unsigned char*)e + STEAM_ENTITY_ANIM_OFFSET) = a;
        *(unsigned int*)((unsigned char*)e + STEAM_ENTITY_FRAME_CURRENT_OFFSET) = 0;
    }
}
static void __fastcall move(void* e, void* unused, float x, float y, float z)
{
    (void)unused;
    unsigned char* p = e;
    *(float*)(p + STEAM_ENTITY_X_OFFSET) = x;
    *(float*)(p + STEAM_ENTITY_Y_OFFSET) = y;
    *(float*)(p + STEAM_ENTITY_Z_OFFSET) = z;
}
static unsigned char __fastcall rotate(void* e, void* unused, unsigned int d)
{
    (void)unused;
    *((unsigned char*)e + STEAM_ENTITY_DIRECTION_OFFSET) = (unsigned char)d;
    return 1;
}
static int __fastcall foreign(void* e, void* unused, unsigned int k, intptr_t a, intptr_t b,
                              intptr_t c)
{
    (void)e;
    (void)unused;
    (void)k;
    (void)a;
    (void)b;
    (void)c;
    return 66;
}
static int form_observer(uintptr_t world, uintptr_t entity, uintptr_t* descriptor,
                         unsigned int* index, unsigned int* revision, int* pending,
                         unsigned int requested)
{
    if (world != (uintptr_t)game || entity != (uintptr_t)unit)
        return 0;
    if (requested == 59u)
    {
        *descriptor = (uintptr_t)frozen_private;
        return 1;
    }
    if (!requested && *(uintptr_t*)(entity + STEAM_ENTITY_VID_OFFSET) == (uintptr_t)frozen_private)
    {
        *descriptor = (uintptr_t)frozen_private;
        *index = 59u;
        *revision = 2u;
        *pending = 0;
        return 1;
    }
    return 0;
}
static void prediction_descriptor(unsigned char* descriptor)
{
    for (unsigned int a = 0; a < STEAM_ANIMATION_COUNT; ++a)
    {
        *(int*)(descriptor + STEAM_VID_ANIMATION_SCRIPTS_OFFSET + a * 4u) = -1;
        *(uintptr_t*)(descriptor + STEAM_VID_CREATIONS_OFFSET + a * 4u) = 0;
    }
    *(int*)(descriptor + STEAM_VID_CREATE_SCRIPT_OFFSET) = -1;
    *(int*)(descriptor + STEAM_VID_DELETE_SCRIPT_OFFSET) = -1;
    *(int*)(descriptor + STEAM_VID_DAMAGE_SCRIPT_OFFSET) = -1;
    *(int*)(descriptor + STEAM_VID_COLLISION_SCRIPT_OFFSET) = -1;
    *(int*)(descriptor + STEAM_VID_DAMAGE_OFFSET) = 0;
    *(uintptr_t*)(descriptor + STEAM_VID_BEHAVIOR_OFFSET) = (uintptr_t)prediction_behavior;
}
int main(void)
{
    uintptr_t base =
        (uintptr_t)VirtualAlloc(NULL, 0x140000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(base);
    void** vt = (void**)(base + STEAM_UNIT_VTABLE_RVA);
    UnitPuppetEngine api = {original_action, original_destroy, original_movement, animation, move,
                            rotate,          original_action,  original_destroy},
                     invalid;
    CHECK(!unit_puppet_bind(base, &invalid) && !invalid.action);
    vt[0] = (void*)original_destroy;
    vt[1] = (void*)original_action;
    vt[3] = (void*)original_movement;
    vt[2] = (void*)(base + STEAM_UNIT_UPDATE_RVA);
    void** part_vt = (void**)(base + STEAM_SPRITE_VTABLE_RVA);
    part_vt[0] = (void*)original_destroy;
    part_vt[1] = (void*)original_action;
    *(uintptr_t*)game = base + STEAM_GAME_VTABLE_RVA;
    *(unsigned int*)(game + STEAM_WORLD_LIST_OFFSET + STEAM_LIST_COUNT_OFFSET) = 2;
    *(unsigned int*)(game + STEAM_WORLD_LIST_OFFSET + STEAM_LIST_CAPACITY_OFFSET) = 4;
    *(uintptr_t**)(game + STEAM_WORLD_LIST_OFFSET + STEAM_LIST_ITEMS_OFFSET) = entries;
    entries[0] = (uintptr_t)other;
    entries[1] = (uintptr_t)unit;
    /* Script list beyond the native tick's 16 categories contains non-entity
       values. A scanner must never follow its entries. */
    *(unsigned int*)(game + STEAM_WORLD_LIST_OFFSET + 17u * STEAM_LIST_SIZE +
                     STEAM_LIST_COUNT_OFFSET) = 1;
    *(unsigned int*)(game + STEAM_WORLD_LIST_OFFSET + 17u * STEAM_LIST_SIZE +
                     STEAM_LIST_CAPACITY_OFFSET) = 1;
    *(uintptr_t*)(game + STEAM_WORLD_LIST_OFFSET + 17u * STEAM_LIST_SIZE +
                  STEAM_LIST_ITEMS_OFFSET) = 1;
    *(uintptr_t*)unit = *(uintptr_t*)other = (uintptr_t)vt;
    *(uintptr_t*)(unit + STEAM_ENTITY_VID_OFFSET) = *(uintptr_t*)(other + STEAM_ENTITY_VID_OFFSET) =
        (uintptr_t)vid;
    *(unsigned int*)(vid + STEAM_VID_CLASS_OFFSET) = STEAM_UNIT_CLASS;
    *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET) = 40;
    *(int*)(unit + STEAM_ENTITY_HEALTH_OFFSET) = *(int*)(other + STEAM_ENTITY_HEALTH_OFFSET) = 100;
    *(unsigned int*)(unit + STEAM_UNIT_BEHAVIOR_OFFSET) = 3;
    *(uintptr_t*)(unit + STEAM_ENTITY_GOAL_OFFSET) = 1234;
    *(unsigned int*)(unit + STEAM_ENTITY_FLAGS_OFFSET) = 0x1010u;
    *(unsigned int*)(unit + STEAM_ENTITY_STACK_COUNT_OFFSET) = 4;
    CHECK(unit_puppet_install(base, &api));
    CHECK(!unit_puppet_install(base, &api));
    CHECK(vt[2] == (void*)(base + STEAM_UNIT_UPDATE_RVA)); /* Never hook frame update. */
    *(uintptr_t*)child = (uintptr_t)part_vt;
    *(uintptr_t*)(child + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)child_vid;
    *(uintptr_t*)(unit + STEAM_ENTITY_CHILD_OFFSET) = (uintptr_t)child;
    CHECK(!unit_puppet_attach_first((uintptr_t)game, (uintptr_t)other));
    *(uintptr_t*)(unit + STEAM_ENTITY_CHILD_OFFSET) = 0;
    *(unsigned int*)(vid + STEAM_VID_CLASS_OFFSET) = 4;
    CHECK(!unit_puppet_attach_first((uintptr_t)game, (uintptr_t)other));
    *(unsigned int*)(vid + STEAM_VID_CLASS_OFFSET) = STEAM_UNIT_CLASS;
    CHECK(unit_puppet_attach_first((uintptr_t)game, (uintptr_t)other) == 1 && clears == 2);
    CHECK(!unit_puppet_stop()); /* Cannot abandon a borrowed active entity. */
    ActorAction hooked_action = (ActorAction)vt[1];
    UnitMovement hooked_move = (UnitMovement)vt[3];
    ActorDestroy hooked_destroy = (ActorDestroy)vt[0];
    CHECK(hooked_action(other, NULL, 130, 0, 0, 0) == 77);
    unsigned int before = actions;
    CHECK(hooked_action(unit, NULL, 0x4B, 123, 0, 0) == 0);  /* Script attack target. */
    CHECK(hooked_action(unit, NULL, 0x55, 1000, 0, 0) == 0); /* Damage. */
    CHECK(actions == before);
    UnitPuppetPose p = {100, 200, 0, 0.02f, 64, 1};
    CHECK(unit_puppet_apply((uintptr_t)game, &p) == 1);
    CHECK(hooked_action(unit, NULL, STEAM_ACT_NEXT_COMMAND, 0, 0, 0) == 0);
    hooked_move(unit, NULL);
    hooked_move(other, NULL);
    CHECK(moves == 1 && actions == before);
    *(unsigned int*)(unit + STEAM_ENTITY_FRAME_CURRENT_OFFSET) =
        5; /* Stand-in for untouched native update. */
    unit_puppet_sample((uintptr_t)game);
    *(unsigned int*)(unit + STEAM_ENTITY_FRAME_CURRENT_OFFSET) = 6;
    UnitPuppetSample s = unit_puppet_sample((uintptr_t)game);
    CHECK(s.frame_changes == 1 && s.frame == 6 && s.animation == 2 && s.decisions == 1 &&
          s.movements == 1);
    CHECK(s.goal == 0 && s.behavior == 0 && s.command == 0 && s.stack == 0 && s.blocked == 2);
    CHECK(s.velocity == p.velocity && s.moving && s.direction == 64 && s.x == 100);
    CHECK(s.class_id == STEAM_UNIT_CLASS);
    CHECK(*(unsigned int*)(unit + STEAM_ENTITY_FLAGS_OFFSET) == 0x1080u);
    CHECK(unit_puppet_apply((uintptr_t)game, &p) == 1 &&
          *(unsigned int*)(unit + STEAM_ENTITY_FRAME_CURRENT_OFFSET) == 6);
    p.z = 80.0f;
    CHECK(unit_puppet_apply((uintptr_t)game, &p) == 1 &&
          unit_puppet_sample((uintptr_t)game).z == 80.0f);
    p.direction = 256;
    CHECK(!unit_puppet_apply((uintptr_t)game, &p));
    p.direction = 64;
    p.moving = 0;
    p.velocity = 0;
    CHECK(unit_puppet_apply((uintptr_t)game, &p) == 1);
    CHECK(unit_puppet_sample((uintptr_t)game).animation == 0);
    CHECK(unit_puppet_release((uintptr_t)game));
    CHECK(*(unsigned int*)(unit + STEAM_UNIT_BEHAVIOR_OFFSET) == 3 && stops == 2);
    CHECK(hooked_action(unit, NULL, 130, 0, 0, 0) == 77);
    *(uintptr_t*)(unit + STEAM_ENTITY_CHILD_OFFSET) = (uintptr_t)child;
    *(uintptr_t*)(vid + STEAM_VID_LINKED_OFFSET) = (uintptr_t)child_vid;
    *(unsigned int*)(child_vid + STEAM_VID_CLASS_OFFSET) = STEAM_PUPPET_PART_CLASS;
    *(uintptr_t*)(child + STEAM_ENTITY_PARENT_OFFSET) = (uintptr_t)unit;
    entries[2] = (uintptr_t)child;
    *(unsigned int*)(game + STEAM_WORLD_LIST_OFFSET + STEAM_LIST_COUNT_OFFSET) = 3;
    *(uintptr_t*)(child + STEAM_ENTITY_GOAL_OFFSET) = 123;
    *(unsigned int*)(child + STEAM_ENTITY_STACK_COUNT_OFFSET) = 2;
    CHECK(unit_puppet_attach_first((uintptr_t)game, (uintptr_t)other) == 1);
    ActorAction part_action = (ActorAction)part_vt[1];
    CHECK(part_action(child, NULL, 0x4B, 123, 0, 0) == 0);
    CHECK(part_action(child, NULL, 130, 0, 0, 0) == 0);
    s = unit_puppet_sample((uintptr_t)game);
    CHECK(s.child == (uintptr_t)child && !s.child_goal && !s.child_command && !s.child_stack);
    ((ActorDestroy)part_vt[0])(child, NULL, 1);
    CHECK(unit_puppet_sample((uintptr_t)game).state == UNIT_PUPPET_FAULT);
    CHECK(unit_puppet_release((uintptr_t)game));
    *(uintptr_t*)(unit + STEAM_ENTITY_CHILD_OFFSET) = 0;
    entries[2] = 0;
    CHECK(unit_puppet_attach_first((uintptr_t)game, (uintptr_t)other) == 1);
    CHECK(hooked_destroy(unit, NULL, 1) == unit && destroyed == 2);
    CHECK(unit_puppet_sample((uintptr_t)game).state == UNIT_PUPPET_LOST);
    CHECK(hooked_action(unit, NULL, 130, 0, 0, 0) ==
          77); /* Same pointer reuse is no longer selected. */
    CHECK(unit_puppet_release((uintptr_t)game) && unit_puppet_stop());
    CHECK(vt[0] == (void*)original_destroy && vt[1] == (void*)original_action &&
          vt[3] == (void*)original_movement);
    CHECK(hooked_action(other, NULL, 130, 0, 0, 0) == 77); /* Cached callback after stop. */
    CHECK(unit_puppet_install(base, &api));
    vt[1] = (void*)foreign;
    CHECK(!unit_puppet_stop() && vt[1] == (void*)foreign);
    vt[1] = (void*)hooked_action;
    CHECK(unit_puppet_stop());
    CHECK(unit_puppet_install(base, &api));
    CHECK(unit_puppet_attach_first((uintptr_t)game, (uintptr_t)other) == 1);
    entries[1] = 0;
    *(unsigned int*)(unit + STEAM_UNIT_BEHAVIOR_OFFSET) = 123;
    CHECK(unit_puppet_release((uintptr_t)game));
    CHECK(*(unsigned int*)(unit + STEAM_UNIT_BEHAVIOR_OFFSET) ==
          123); /* No writes to an unregistered cached pointer. */
    CHECK(unit_puppet_stop());
    /* Replay must reject ordinary shared VIDs, even when the entity is ours.
       Sanitized private-like test VIDs permit visual events, never raw damage. */
    entries[1] = (uintptr_t)unit;
    *(int*)(unit + STEAM_ENTITY_HEALTH_OFFSET) = 100;
    CHECK(unit_puppet_install(base, &api));
    CHECK(unit_puppet_attach_first((uintptr_t)game, (uintptr_t)other) == 1);
    CHECK(!unit_puppet_replay((uintptr_t)game, 1u, STEAM_ANIM_ATTACK));
    CHECK(unit_puppet_test_damage((uintptr_t)game) == 1);
    CHECK(*(int*)(unit + STEAM_ENTITY_HEALTH_OFFSET) == 100);
    for (unsigned int i = 0; i < STEAM_ANIMATION_COUNT; ++i)
    {
        *(int*)(vid + STEAM_VID_ANIMATION_SCRIPTS_OFFSET + i * 4u) = -1;
        *(uintptr_t*)(vid + STEAM_VID_CREATIONS_OFFSET + i * 4u) = 0;
    }
    *(int*)(vid + STEAM_VID_DELETE_SCRIPT_OFFSET) = -1;
    *(int*)(vid + STEAM_VID_DAMAGE_SCRIPT_OFFSET) = -1;
    *(int*)(vid + STEAM_VID_COLLISION_SCRIPT_OFFSET) = -1;
    CHECK(unit_puppet_replay((uintptr_t)game, 1u, STEAM_ANIM_ATTACK) == 1);
    *(unsigned int*)(unit + STEAM_ENTITY_FRAME_CURRENT_OFFSET) = 7;
    CHECK(!unit_puppet_replay((uintptr_t)game, 1u, STEAM_ANIM_ATTACK));
    CHECK(*(unsigned int*)(unit + STEAM_ENTITY_FRAME_CURRENT_OFFSET) == 7);
    CHECK(unit_puppet_apply((uintptr_t)game, &p) == 1);
    CHECK(*(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) == STEAM_ANIM_ATTACK);
    CHECK(hooked_action(unit, NULL, STEAM_ACT_NEXT_COMMAND, 0, 0, 0) == 0);
    CHECK(*(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) == STEAM_ANIM_IDLE);
    CHECK(!unit_puppet_replay((uintptr_t)game, 0, STEAM_ANIM_ATTACK));
    CHECK(!unit_puppet_replay((uintptr_t)game, 2u, STEAM_ANIM_RUN));
    CHECK(unit_puppet_replay((uintptr_t)game, 2u, STEAM_ANIM_DEATH) == 1);
    CHECK(!unit_puppet_replay((uintptr_t)game, 2u, STEAM_ANIM_DEATH));
    CHECK(!unit_puppet_replay((uintptr_t)game, 3u, STEAM_ANIM_ATTACK));
    s = unit_puppet_sample((uintptr_t)game);
    CHECK(s.animation == STEAM_ANIM_DEATH && s.replayed == 2u && s.duplicates == 2u);
    CHECK(hooked_action(unit, NULL, STEAM_ACT_NEXT_COMMAND, 0, 0, 0) == 0);
    CHECK(*(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) == STEAM_ANIM_DEATH);
    CHECK(unit_puppet_release((uintptr_t)game));
    CHECK(*(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) == STEAM_ANIM_DEATH);
    CHECK(unit_puppet_stop());
    /* Host watch observes stock UNIT50/108 without controlling AI, motion,
       health, commands or native animation. Native USE_AMMO alone is an event. */
    memset(unit, 0, sizeof(unit));
    memset(child, 0, sizeof(child));
    *(uintptr_t*)unit = (uintptr_t)vt;
    *(uintptr_t*)child = (uintptr_t)part_vt;
    *(uintptr_t*)(unit + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)vid;
    *(uintptr_t*)(child + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)child_vid;
    *(uintptr_t*)(unit + STEAM_ENTITY_CHILD_OFFSET) = (uintptr_t)child;
    *(uintptr_t*)(child + STEAM_ENTITY_PARENT_OFFSET) = (uintptr_t)unit;
    *(uintptr_t*)(vid + STEAM_VID_LINKED_OFFSET) = (uintptr_t)child_vid;
    *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET) = 50u;
    *(unsigned int*)(child_vid + STEAM_VID_INDEX_OFFSET) = 108u;
    *(unsigned int*)(child_vid + STEAM_VID_CLASS_OFFSET) = STEAM_PUPPET_PART_CLASS;
    *(int*)(unit + STEAM_ENTITY_HEALTH_OFFSET) = *(int*)(child + STEAM_ENTITY_HEALTH_OFFSET) = 100;
    *(unsigned int*)(unit + STEAM_UNIT_BEHAVIOR_OFFSET) = 3;
    *(uintptr_t*)(unit + STEAM_ENTITY_GOAL_OFFSET) = 1234;
    *(unsigned int*)(unit + STEAM_ENTITY_STACK_COUNT_OFFSET) = 4;
    *(unsigned int*)(unit + STEAM_ENTITY_FLAGS_OFFSET) = STEAM_ENTITY_MOVING_FLAG | 0x10u;
    *(float*)(unit + STEAM_ENTITY_VELOCITY_OFFSET) = 0.02f;
    *(uintptr_t*)(child + STEAM_ENTITY_GOAL_OFFSET) = 2345;
    *(unsigned int*)(child + STEAM_ENTITY_STACK_COUNT_OFFSET) = 2;
    entries[1] = (uintptr_t)unit;
    entries[2] = (uintptr_t)child;
    *(unsigned int*)(game + STEAM_WORLD_LIST_OFFSET + STEAM_LIST_COUNT_OFFSET) = 3;
    unsigned char watch_root_before[sizeof(unit)], watch_part_before[sizeof(child)];
    memcpy(watch_root_before, unit, sizeof(unit));
    memcpy(watch_part_before, child, sizeof(child));
    CHECK(unit_puppet_install(base, &api));
    unsigned int watch_actions = actions, watch_animations = animations, watch_moves = moves;
    CHECK(unit_puppet_watch_first((uintptr_t)game, (uintptr_t)other) == 1);
    CHECK(actions == watch_actions && animations == watch_animations);
    CHECK(!memcmp(watch_root_before, unit, sizeof(unit)) &&
          !memcmp(watch_part_before, child, sizeof(child)));
    CHECK(!unit_puppet_apply((uintptr_t)game, &p) && !unit_puppet_health((uintptr_t)game, 10));
    CHECK(!unit_puppet_replay((uintptr_t)game, 1, STEAM_ANIM_ATTACK));
    CHECK(hooked_action(unit, NULL, STEAM_ACT_NEXT_COMMAND, 0, 0, 0) == 77);
    hooked_move(unit, NULL);
    CHECK(moves == watch_moves + 1u);
    CHECK(!memcmp(watch_root_before, unit, sizeof(unit)));
    CHECK(hooked_action(unit, NULL, STEAM_ACT_DAMAGE, 7, 0, 0) == 77);
    CHECK(part_action(child, NULL, STEAM_ACT_DAMAGE, 9, 0, 0) == 77);
    CHECK(*(int*)(unit + STEAM_ENTITY_HEALTH_OFFSET) == 93 &&
          *(int*)(child + STEAM_ENTITY_HEALTH_OFFSET) == 91);
    CHECK(hooked_action(unit, NULL, STEAM_ACT_USE_AMMO, 1, 0, 0) == 77); /* Query, no attack. */
    CHECK(part_action(child, NULL, STEAM_ACT_USE_AMMO, 10, 0, 0) == 77);
    CHECK(unit_puppet_sample((uintptr_t)game).captured_attacks == 0);
    *(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) = STEAM_ANIM_ATTACK;
    CHECK(unit_puppet_sample((uintptr_t)game).captured_attacks == 0); /* Pose is never an event. */
    CHECK(hooked_action(unit, NULL, STEAM_ACT_USE_AMMO, -1, 0, 0) == 77);
    CHECK(part_action(child, NULL, STEAM_ACT_USE_AMMO, -5, 0, 0) == 77);
    CHECK(hooked_action(unit, NULL, STEAM_ACT_USE_AMMO, 0, 0, 0) == 77);
    CHECK(part_action(child, NULL, STEAM_ACT_USE_AMMO, 0, 0, 0) == 77);
    CHECK(unit_puppet_sample((uintptr_t)game).captured_attacks == 4);
    CHECK(unit_puppet_host_kill((uintptr_t)game) == 1);
    CHECK(!unit_puppet_host_kill((uintptr_t)game));
    CHECK(unit_puppet_sample((uintptr_t)game).captured_deaths == 1);
    CHECK(unit_puppet_sample((uintptr_t)game).death_animation == STEAM_ANIM_DEATH);
    CHECK(hooked_action(unit, NULL, STEAM_ACT_USE_AMMO, 0, 0, 0) == 77);
    s = unit_puppet_sample((uintptr_t)game);
    CHECK(s.captured_attacks == 4 && s.captured_deaths == 1 && !s.blocked);
    memcpy(watch_root_before, unit, sizeof(unit));
    memcpy(watch_part_before, child, sizeof(child));
    watch_actions = actions;
    watch_animations = animations;
    CHECK(unit_puppet_release((uintptr_t)game));
    CHECK(actions == watch_actions && animations == watch_animations);
    CHECK(!memcmp(watch_root_before, unit, sizeof(unit)) &&
          !memcmp(watch_part_before, child, sizeof(child)));
    *(int*)(unit + STEAM_ENTITY_HEALTH_OFFSET) = 100;
    *(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) = STEAM_ANIM_IDLE;
    *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET) = 40u;
    CHECK(!unit_puppet_watch_first((uintptr_t)game, (uintptr_t)other));
    *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET) = 50u;
    CHECK(unit_puppet_watch_first((uintptr_t)game, (uintptr_t)other) == 1);
    *(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) = STEAM_ANIM_GIB_DEATH;
    *(float*)(unit + STEAM_ENTITY_X_OFFSET) = 123.0f;
    *(float*)(unit + STEAM_ENTITY_Y_OFFSET) = 456.0f;
    *(unsigned char*)(unit + STEAM_ENTITY_DIRECTION_OFFSET) = 99;
    CHECK(hooked_action(unit, NULL, STEAM_ACT_GET_HP, 0, 0, 0) == 77);
    s = unit_puppet_sample((uintptr_t)game);
    CHECK(s.captured_deaths == 1 && s.death_animation == 16 && s.death_pose.x == 123.0f &&
          s.death_pose.direction == 99);
    hooked_destroy(unit, NULL, 1);
    s = unit_puppet_sample((uintptr_t)game);
    CHECK(!s.entity && s.death_animation == 16 && s.death_pose.y == 456.0f &&
          s.captured_deaths == 1);
    CHECK(unit_puppet_release((uintptr_t)game));
    *(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) = STEAM_ANIM_IDLE;
    CHECK(!unit_puppet_attach_entity((uintptr_t)game, (uintptr_t)other, (uintptr_t)other));
    CHECK(!unit_puppet_attach_entity((uintptr_t)game, (uintptr_t)other, (uintptr_t)child));
    CHECK(!unit_puppet_attach_entity((uintptr_t)game, (uintptr_t)other,
                                     (uintptr_t)watch_root_before));
    CHECK(unit_puppet_attach_entity((uintptr_t)game, (uintptr_t)other, (uintptr_t)unit) == 1);
    CHECK(unit_puppet_sample((uintptr_t)game).entity == (uintptr_t)unit);
    CHECK(unit_puppet_release((uintptr_t)game) && unit_puppet_stop());
    /* Independent slots share hooks. Stationary host observers keep incoming
       native damage/death and restore the caller's slot after callbacks. */
    CHECK(unit_puppet_install(base, &api));
    CHECK(unit_puppet_select(0));
    *(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) = STEAM_ANIM_IDLE;
    *(int*)(unit + STEAM_ENTITY_HEALTH_OFFSET) = 100;
    *(uintptr_t*)(unit + STEAM_ENTITY_CHILD_OFFSET) = 0;
    *(unsigned int*)(game + STEAM_WORLD_LIST_OFFSET + STEAM_LIST_COUNT_OFFSET) = 2;
    CHECK(unit_puppet_watch_stationary((uintptr_t)game, (uintptr_t)other, (uintptr_t)unit) == 1);
    CHECK(unit_puppet_select(1));
    CHECK(!unit_puppet_watch_entity((uintptr_t)game, (uintptr_t)other, (uintptr_t)unit));
    CHECK(unit_puppet_watch_stationary((uintptr_t)game, (uintptr_t)unit, (uintptr_t)other) == 1);
    CHECK(!unit_puppet_select(UNIT_PUPPET_CAPACITY));
    unsigned int gallery_moves = moves, gallery_actions = actions;
    CHECK(hooked_action(unit, NULL, STEAM_ACT_NEXT_COMMAND, 0, 0, 0) == 0);
    CHECK(hooked_action(other, NULL, STEAM_ACT_NEXT_COMMAND, 0, 0, 0) == 0);
    hooked_move(unit, NULL);
    hooked_move(other, NULL);
    CHECK(moves == gallery_moves && actions == gallery_actions);
    CHECK(hooked_action(unit, NULL, STEAM_ACT_DAMAGE, 13, 0, 0) == 77);
    CHECK(*(int*)(unit + STEAM_ENTITY_HEALTH_OFFSET) == 87);
    CHECK(unit_puppet_sample((uintptr_t)game).entity == (uintptr_t)other);
    CHECK(unit_puppet_sample((uintptr_t)game).decisions == 1);
    CHECK(hooked_action(unit, NULL, STEAM_ACT_SET_HP, 0, 0, 0) == 77);
    CHECK(unit_puppet_sample((uintptr_t)game).captured_deaths == 0);
    CHECK(unit_puppet_select(0));
    CHECK(unit_puppet_sample((uintptr_t)game).captured_deaths == 1);
    CHECK(unit_puppet_sample((uintptr_t)game).death_animation == STEAM_ANIM_DEATH);
    CHECK(!unit_puppet_stop());
    CHECK(unit_puppet_release((uintptr_t)game));
    CHECK(!unit_puppet_stop()); /* Slot1 still owns its borrow. */
    CHECK(unit_puppet_select(1) && unit_puppet_release((uintptr_t)game));
    CHECK(unit_puppet_stop() && unit_puppet_select(0));
    /* Armor successors can receive another projectile before the coordinator
       adopts them. Pain and final death are valid stationary watch entries. */
    *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET) = 60u;
    for (unsigned int hit = 13u; hit <= STEAM_ANIM_GIB_DEATH; ++hit)
    {
        *(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) = hit;
        *(int*)(unit + STEAM_ENTITY_HEALTH_OFFSET) = 20;
        CHECK(unit_puppet_install(base, &api));
        CHECK(unit_puppet_watch_stationary((uintptr_t)game, (uintptr_t)other, (uintptr_t)unit) ==
              1);
        s = unit_puppet_sample((uintptr_t)game);
        CHECK(s.entity == (uintptr_t)unit && s.animation == hit && !s.transitioning);
        CHECK(s.captured_deaths == (hit >= STEAM_ANIM_DEATH ? 1u : 0u));
        if (hit >= STEAM_ANIM_DEATH)
        {
            CHECK(s.death_animation == hit);
            hooked_destroy(unit, NULL, 1);
            CHECK(unit_puppet_sample((uintptr_t)game).captured_deaths == 1u);
        }
        CHECK(unit_puppet_release((uintptr_t)game) && unit_puppet_stop());
    }
    /* Freeze damage briefly selects death inside a script; CHANGE_VID settles
       before sampling. It stays alive/damageable and adopts only verified form. */
    memset(frozen_stock, 0, sizeof(frozen_stock));
    *(unsigned int*)(frozen_stock + STEAM_VID_INDEX_OFFSET) = 59u;
    *(unsigned int*)(frozen_stock + STEAM_VID_CLASS_OFFSET) = STEAM_UNIT_CLASS;
    *(unsigned int*)(frozen_stock + STEAM_VID_CATEGORY_OFFSET) =
        *(unsigned int*)(vid + STEAM_VID_CATEGORY_OFFSET);
    memcpy(frozen_private, frozen_stock, sizeof(frozen_stock));
    *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 59u * 4u) = (uintptr_t)frozen_stock;
    *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET) = 50u;
    *(uintptr_t*)(unit + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)vid;
    *(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) = STEAM_ANIM_IDLE;
    *(int*)(unit + STEAM_ENTITY_HEALTH_OFFSET) = 100;
    CHECK(unit_puppet_install(base, &api));
    unit_puppet_set_form_observer(form_observer);
    CHECK(unit_puppet_watch_stationary((uintptr_t)game, (uintptr_t)other, (uintptr_t)unit) == 1);
    CHECK(hooked_action(unit, NULL, STEAM_ACT_DAMAGE, 30, 0, 0) == 77);
    *(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) = STEAM_ANIM_DEATH;
    CHECK(hooked_action(unit, NULL, STEAM_ACT_CHANGE_VID, 59u, STEAM_ANIM_IDLE, 0) == 77);
    CHECK(change_private_seen == 1 &&
          *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 59u * 4u) == (uintptr_t)frozen_stock);
    s = unit_puppet_sample((uintptr_t)game);
    CHECK(s.state == UNIT_PUPPET_ACTIVE && s.vid == 59u && s.form_revision == 2u &&
          s.captured_forms == 1u);
    CHECK(s.health == 70 && s.captured_deaths == 0 && !s.transitioning);
    CHECK(hooked_action(unit, NULL, STEAM_ACT_DAMAGE, 20, 0, 0) == 77);
    CHECK(unit_puppet_sample((uintptr_t)game).health == 50);
    CHECK(unit_puppet_host_kill((uintptr_t)game) == 1);
    s = unit_puppet_sample((uintptr_t)game);
    CHECK(s.captured_deaths == 1 && s.vid == 59u);
    CHECK(unit_puppet_release((uintptr_t)game) && unit_puppet_stop());
    /* A native exception cannot leave a private descriptor in the stock table. */
    *(uintptr_t*)(unit + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)vid;
    *(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) = STEAM_ANIM_IDLE;
    *(int*)(unit + STEAM_ENTITY_HEALTH_OFFSET) = 100;
    CHECK(unit_puppet_install(base, &api));
    unit_puppet_set_form_observer(form_observer);
    CHECK(unit_puppet_watch_stationary((uintptr_t)game, (uintptr_t)other, (uintptr_t)unit) == 1);
    change_fault = 1;
    CHECK(unit_puppet_host_freeze((uintptr_t)game) == -1);
    change_fault = 0;
    CHECK(*(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 59u * 4u) == (uintptr_t)frozen_stock);
    CHECK(unit_puppet_release((uintptr_t)game) && unit_puppet_stop());
    /* Borrowed native observers also accept the precise stock frozen form. */
    *(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) = STEAM_ANIM_IDLE;
    CHECK(unit_puppet_install(base, &api));
    CHECK(unit_puppet_watch_entity((uintptr_t)game, (uintptr_t)other, (uintptr_t)unit) == 1);
    CHECK(unit_puppet_host_freeze((uintptr_t)game) == 1);
    s = unit_puppet_sample((uintptr_t)game);
    CHECK(s.vid == 59u && s.state == UNIT_PUPPET_ACTIVE && !s.captured_deaths && s.health == 100);
    CHECK(unit_puppet_release((uintptr_t)game) && unit_puppet_stop());
    /* Normal armor break is an intermediate form; gib death is terminal. */
    for (unsigned int variant = STEAM_ANIM_DEATH; variant <= STEAM_ANIM_GIB_DEATH; ++variant)
    {
        *(uintptr_t*)(unit + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)vid;
        *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET) = 70u;
        *(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) = STEAM_ANIM_IDLE;
        *(int*)(unit + STEAM_ENTITY_HEALTH_OFFSET) = 100;
        CHECK(unit_puppet_install(base, &api));
        CHECK(unit_puppet_watch_stationary((uintptr_t)game, (uintptr_t)other, (uintptr_t)unit) ==
              1);
        *(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) = variant;
        s = unit_puppet_sample((uintptr_t)game);
        CHECK(s.transitioning == (variant == STEAM_ANIM_DEATH));
        CHECK(s.captured_deaths == (variant == STEAM_ANIM_GIB_DEATH ? 1u : 0u));
        hooked_destroy(unit, NULL, 1);
        s = unit_puppet_sample((uintptr_t)game);
        CHECK(s.vid == 70u && s.captured_deaths == (variant == STEAM_ANIM_GIB_DEATH ? 1u : 0u));
        CHECK(unit_puppet_release((uintptr_t)game) && unit_puppet_stop());
    }
    /* Cosmetic prediction opts in only for inert private VIDs. Actual native
       damage, including nested SetHP and a full freeze script, stays local. */
    *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET) = 50u;
    *(uintptr_t*)(unit + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)vid;
    *(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) = STEAM_ANIM_IDLE;
    *(int*)(unit + STEAM_ENTITY_HEALTH_OFFSET) = 100;
    CHECK(unit_puppet_install(base, &api));
    unit_puppet_set_form_observer(form_observer);
    CHECK(unit_puppet_attach_entity((uintptr_t)game, (uintptr_t)other, (uintptr_t)unit) == 1);
    CHECK(!unit_puppet_prediction((uintptr_t)game));
    CHECK(!unit_puppet_prediction_test_hit((uintptr_t)game, 30, 1));
    prediction_descriptor(vid);
    prediction_descriptor(frozen_private);
    *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 50u * 4u) = (uintptr_t)vid;
    CHECK(!unit_puppet_prediction((uintptr_t)game)); /* Shared, even if sanitized. */
    *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 50u * 4u) = 0;
    *(unsigned int*)(prediction_behavior + STEAM_BEHAVIOR_UNIT_FLAGS_OFFSET) = 1;
    CHECK(!unit_puppet_prediction((uintptr_t)game));
    *(unsigned int*)(prediction_behavior + STEAM_BEHAVIOR_UNIT_FLAGS_OFFSET) = 0;
    CHECK(unit_puppet_prediction((uintptr_t)game) == 1);
    CHECK(!unit_puppet_prediction_test_hit((uintptr_t)game, 0, 0));
    predictive_native = 1;
    CHECK(hooked_action(unit, NULL, STEAM_ACT_DAMAGE, 10, 0, 0) == 77);
    CHECK(hooked_action(unit, NULL, STEAM_ACT_DAMAGE, 15, 0, 0) == 77);
    s = unit_puppet_sample((uintptr_t)game);
    CHECK(s.predictive && s.predicted_hits == 2u && s.health == 75 && s.animation == 13u &&
          !s.captured_deaths);
    CHECK(unit_puppet_apply((uintptr_t)game, &p) == 1);
    CHECK(unit_puppet_sample((uintptr_t)game).animation == 13u);        /* Preserve impact. */
    CHECK(!hooked_action(unit, NULL, STEAM_ACT_NEXT_COMMAND, 0, 0, 0)); /* Pain completes. */
    CHECK(unit_puppet_sample((uintptr_t)game).animation == STEAM_ANIM_IDLE);
    before = actions;
    CHECK(!hooked_action(unit, NULL, STEAM_ACT_USE_AMMO, 0, 0, 0));
    CHECK(!hooked_action(unit, NULL, STEAM_ACT_SET_BEHAVIOR, 3, 0, 0));
    CHECK(!hooked_action(unit, NULL, STEAM_ACT_CHANGE_VID, 69u, 0, 0));
    CHECK(actions == before && !unit_puppet_sample((uintptr_t)game).captured_attacks);
    *(int*)(unit + STEAM_ENTITY_HEALTH_OFFSET) = 20;
    nested_freeze = recursive_form_cleanup = 1;
    CHECK(hooked_action(unit, NULL, STEAM_ACT_DAMAGE, 30, 0, 0) == 77);
    nested_freeze = recursive_form_cleanup = 0;
    s = unit_puppet_sample((uintptr_t)game);
    CHECK(s.vid == 59u && s.form_revision == 2u && s.predicted_hits == 3u && s.health == 20);
    CHECK(!s.captured_deaths && !s.transitioning && s.animation == STEAM_ANIM_IDLE);
    CHECK(*(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 59u * 4u) == (uintptr_t)frozen_stock);
    p.moving = 1;
    p.velocity = 0.02f;
    CHECK(unit_puppet_apply((uintptr_t)game, &p) == 1);
    s = unit_puppet_sample((uintptr_t)game);
    CHECK(!s.moving && s.velocity == 0 && s.animation == STEAM_ANIM_IDLE);
    CHECK(hooked_action(unit, NULL, STEAM_ACT_DAMAGE, 30, 0, 0) == 77);
    CHECK(unit_puppet_apply((uintptr_t)game, &p) == 1); /* Unsampled death survives apply. */
    s = unit_puppet_sample((uintptr_t)game);
    CHECK(s.captured_deaths == 1u && s.death_animation == STEAM_ANIM_DEATH &&
          s.predicted_hits == 4u);
    hooked_destroy(unit, NULL, 1);
    s = unit_puppet_sample((uintptr_t)game);
    CHECK(s.captured_deaths == 1u && s.state == UNIT_PUPPET_LOST);
    CHECK(unit_puppet_release((uintptr_t)game) && unit_puppet_stop());
    /* The live diagnostic follows the same native action sequence separately
       from the recursive script mock above. It never claims projectile input. */
    *(uintptr_t*)(unit + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)vid;
    *(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) = STEAM_ANIM_IDLE;
    *(int*)(unit + STEAM_ENTITY_HEALTH_OFFSET) = 20;
    CHECK(unit_puppet_install(base, &api));
    unit_puppet_set_form_observer(form_observer);
    CHECK(unit_puppet_attach_entity((uintptr_t)game, (uintptr_t)other, (uintptr_t)unit) == 1);
    CHECK(unit_puppet_prediction((uintptr_t)game) == 1);
    CHECK(unit_puppet_prediction_test_hit((uintptr_t)game, 30, 1) == 1);
    s = unit_puppet_sample((uintptr_t)game);
    CHECK(s.vid == 59u && s.predicted_hits == 1u && !s.captured_deaths && s.health == 20);
    CHECK(unit_puppet_release((uintptr_t)game) && unit_puppet_stop());
    /* An unsupported preparation cannot execute stock CHANGE_VID; faults are
       contained by the prediction hook and restore the global table entry. */
    *(uintptr_t*)(unit + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)vid;
    *(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) = STEAM_ANIM_IDLE;
    *(int*)(unit + STEAM_ENTITY_HEALTH_OFFSET) = 100;
    CHECK(unit_puppet_install(base, &api));
    CHECK(unit_puppet_attach_entity((uintptr_t)game, (uintptr_t)other, (uintptr_t)unit) == 1);
    CHECK(unit_puppet_prediction((uintptr_t)game) == 1);
    before = actions;
    CHECK(!hooked_action(unit, NULL, STEAM_ACT_CHANGE_VID, 59u, STEAM_ANIM_IDLE, 0));
    CHECK(actions == before && *(uintptr_t*)(unit + STEAM_ENTITY_VID_OFFSET) == (uintptr_t)vid);
    unit_puppet_set_form_observer(form_observer);
    change_fault = 1;
    CHECK(!hooked_action(unit, NULL, STEAM_ACT_CHANGE_VID, 59u, STEAM_ANIM_IDLE, 0));
    change_fault = 0;
    CHECK(unit_puppet_sample((uintptr_t)game).state == UNIT_PUPPET_FAULT);
    CHECK(*(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 59u * 4u) == (uintptr_t)frozen_stock);
    CHECK(unit_puppet_release((uintptr_t)game) && unit_puppet_stop());
    /* An armored prediction has no terminal death on ordinary shell break. */
    *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET) = 70u;
    *(unsigned int*)(unit + STEAM_ENTITY_ANIM_OFFSET) = STEAM_ANIM_IDLE;
    CHECK(unit_puppet_install(base, &api));
    CHECK(unit_puppet_attach_entity((uintptr_t)game, (uintptr_t)other, (uintptr_t)unit) == 1);
    CHECK(unit_puppet_prediction((uintptr_t)game) == 1);
    CHECK(hooked_action(unit, NULL, STEAM_ACT_DAMAGE, 1000, 0, 0) == 77);
    s = unit_puppet_sample((uintptr_t)game);
    CHECK(s.transitioning && !s.captured_deaths && s.predicted_hits == 1u);
    CHECK(unit_puppet_release((uintptr_t)game) && unit_puppet_stop());
    predictive_native = 0;
    vt[1] = (void*)foreign;
    CHECK(!unit_puppet_install(base, &api));
    CHECK(vt[0] == (void*)original_destroy && vt[3] == (void*)original_movement &&
          vt[1] == (void*)foreign);
    CHECK(unit_puppet_stop());
    VirtualFree((void*)base, 0, MEM_RELEASE);
    printf("UNIT puppet isolation/lifecycle checks passed.\n");
    return 0;
}
