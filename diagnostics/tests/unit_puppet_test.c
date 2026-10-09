#include <stdio.h>
#include <string.h>
#include "../../asmp-dll/src/game/unit_puppet.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
static unsigned char game[0x300], unit[0x94], other[0x94], vid[0x490], child[0x94], child_vid[0x490];
static uintptr_t entries[4];
static unsigned int actions, moves, destroyed, animations, clears, stops;
static int __fastcall original_action(void* e, void* unused, unsigned int k, intptr_t a, intptr_t b, intptr_t c) {
    (void)unused; (void)b; (void)c; ++actions;
    unsigned char* p = e;
    if (k == STEAM_ACT_SET_BEHAVIOR) *(unsigned int*)(p + STEAM_UNIT_BEHAVIOR_OFFSET) = (unsigned int)a;
    if (k == STEAM_ACT_CLEAR_COMMAND) { *(uintptr_t*)(p + STEAM_ENTITY_GOAL_OFFSET) = 0;
        *(unsigned int*)(p + STEAM_ENTITY_FLAGS_OFFSET) &= ~STEAM_ENTITY_COMMAND_MASK; ++clears; }
    if (k == STEAM_ACT_CLEAR_STACK) { *(unsigned int*)(p + STEAM_ENTITY_STACK_COUNT_OFFSET) = 0; ++clears; }
    if (k == STEAM_ACT_STOP) { *(float*)(p + STEAM_ENTITY_VELOCITY_OFFSET) = 0;
        *(unsigned int*)(p + STEAM_ENTITY_FLAGS_OFFSET) &= ~STEAM_ENTITY_MOVING_FLAG; ++stops; }
    return 77;
}
static void __fastcall original_movement(void* e, void* unused) { (void)e; (void)unused; ++moves; }
static void* __fastcall original_destroy(void* e, void* unused, int release) {
    (void)unused; (void)release; ++destroyed; return e;
}
static void __fastcall animation(void* e, void* unused, unsigned int a) {
    (void)unused; ++animations;
    if (*(unsigned int*)((unsigned char*)e + STEAM_ENTITY_ANIM_OFFSET) != a) {
        *(unsigned int*)((unsigned char*)e + STEAM_ENTITY_ANIM_OFFSET) = a;
        *(unsigned int*)((unsigned char*)e + STEAM_ENTITY_FRAME_CURRENT_OFFSET) = 0;
    }
}
static void __fastcall move(void* e, void* unused, float x, float y, float z) {
    (void)unused; unsigned char* p = e;
    *(float*)(p + STEAM_ENTITY_X_OFFSET) = x; *(float*)(p + STEAM_ENTITY_Y_OFFSET) = y; *(float*)(p + STEAM_ENTITY_Z_OFFSET) = z;
}
static unsigned char __fastcall rotate(void* e, void* unused, unsigned int d) {
    (void)unused; *((unsigned char*)e + STEAM_ENTITY_DIRECTION_OFFSET) = (unsigned char)d; return 1;
}
static int __fastcall foreign(void* e, void* unused, unsigned int k, intptr_t a, intptr_t b, intptr_t c) {
    (void)e; (void)unused; (void)k; (void)a; (void)b; (void)c; return 66;
}
int main(void) {
    uintptr_t base = (uintptr_t)VirtualAlloc(NULL, 0x140000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(base);
    void** vt = (void**)(base + STEAM_UNIT_VTABLE_RVA);
    UnitPuppetEngine api = {original_action, original_destroy, original_movement, animation, move, rotate, original_action, original_destroy}, invalid;
    CHECK(!unit_puppet_bind(base, &invalid) && !invalid.action);
    vt[0] = (void*)original_destroy; vt[1] = (void*)original_action; vt[3] = (void*)original_movement;
    vt[2] = (void*)(base + STEAM_UNIT_UPDATE_RVA);
    void** part_vt = (void**)(base + STEAM_SPRITE_VTABLE_RVA);
    part_vt[0] = (void*)original_destroy; part_vt[1] = (void*)original_action;
    *(uintptr_t*)game = base + STEAM_GAME_VTABLE_RVA;
    *(unsigned int*)(game + STEAM_WORLD_LIST_OFFSET + 4) = 2;
    *(unsigned int*)(game + STEAM_WORLD_LIST_OFFSET + 8) = 4;
    *(uintptr_t**)(game + STEAM_WORLD_LIST_OFFSET + 12) = entries;
    entries[0] = (uintptr_t)other; entries[1] = (uintptr_t)unit;
    /* Script list beyond the native tick's 16 categories contains non-entity
       values. A scanner must never follow its entries. */
    *(unsigned int*)(game + STEAM_WORLD_LIST_OFFSET + 17u * STEAM_LIST_SIZE + 4) = 1;
    *(unsigned int*)(game + STEAM_WORLD_LIST_OFFSET + 17u * STEAM_LIST_SIZE + 8) = 1;
    *(uintptr_t*)(game + STEAM_WORLD_LIST_OFFSET + 17u * STEAM_LIST_SIZE + 12) = 1;
    *(uintptr_t*)unit = *(uintptr_t*)other = (uintptr_t)vt;
    *(uintptr_t*)(unit + STEAM_ENTITY_VID_OFFSET) = *(uintptr_t*)(other + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)vid;
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
    CHECK(hooked_action(unit, NULL, 0x4B, 123, 0, 0) == 0); /* Script attack target. */
    CHECK(hooked_action(unit, NULL, 0x55, 1000, 0, 0) == 0); /* Damage. */
    CHECK(actions == before);
    UnitPuppetPose p = {100, 200, 0, 0.02f, 64, 1};
    CHECK(unit_puppet_apply((uintptr_t)game, &p) == 1);
    CHECK(hooked_action(unit, NULL, STEAM_ACT_NEXT_COMMAND, 0, 0, 0) == 0);
    hooked_move(unit, NULL); hooked_move(other, NULL);
    CHECK(moves == 1 && actions == before);
    *(unsigned int*)(unit + STEAM_ENTITY_FRAME_CURRENT_OFFSET) = 5; /* Stand-in for untouched native update. */
    unit_puppet_sample((uintptr_t)game);
    *(unsigned int*)(unit + STEAM_ENTITY_FRAME_CURRENT_OFFSET) = 6;
    UnitPuppetSample s = unit_puppet_sample((uintptr_t)game);
    CHECK(s.frame_changes == 1 && s.frame == 6 && s.animation == 2 && s.decisions == 1 && s.movements == 1);
    CHECK(s.goal == 0 && s.behavior == 0 && s.command == 0 && s.stack == 0 && s.blocked == 2);
    CHECK(s.velocity == p.velocity && s.moving && s.direction == 64 && s.x == 100);
    CHECK(s.class_id == STEAM_UNIT_CLASS);
    CHECK(*(unsigned int*)(unit + STEAM_ENTITY_FLAGS_OFFSET) == 0x1080u);
    CHECK(unit_puppet_apply((uintptr_t)game, &p) == 1 && *(unsigned int*)(unit + STEAM_ENTITY_FRAME_CURRENT_OFFSET) == 6);
    p.z = 80.0f;
    CHECK(unit_puppet_apply((uintptr_t)game, &p) == 1 && unit_puppet_sample((uintptr_t)game).z == 80.0f);
    p.direction = 256; CHECK(!unit_puppet_apply((uintptr_t)game, &p)); p.direction = 64;
    p.moving = 0; p.velocity = 0;
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
    *(unsigned int*)(game + STEAM_WORLD_LIST_OFFSET + 4) = 3;
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
    CHECK(hooked_action(unit, NULL, 130, 0, 0, 0) == 77); /* Same pointer reuse is no longer selected. */
    CHECK(unit_puppet_release((uintptr_t)game) && unit_puppet_stop());
    CHECK(vt[0] == (void*)original_destroy && vt[1] == (void*)original_action && vt[3] == (void*)original_movement);
    CHECK(hooked_action(other, NULL, 130, 0, 0, 0) == 77); /* Cached callback after stop. */
    CHECK(unit_puppet_install(base, &api));
    vt[1] = (void*)foreign;
    CHECK(!unit_puppet_stop() && vt[1] == (void*)foreign);
    vt[1] = (void*)hooked_action; CHECK(unit_puppet_stop());
    CHECK(unit_puppet_install(base, &api));
    CHECK(unit_puppet_attach_first((uintptr_t)game, (uintptr_t)other) == 1);
    entries[1] = 0;
    *(unsigned int*)(unit + STEAM_UNIT_BEHAVIOR_OFFSET) = 123;
    CHECK(unit_puppet_release((uintptr_t)game));
    CHECK(*(unsigned int*)(unit + STEAM_UNIT_BEHAVIOR_OFFSET) == 123); /* No writes to an unregistered cached pointer. */
    CHECK(unit_puppet_stop());
    vt[1] = (void*)foreign;
    CHECK(!unit_puppet_install(base, &api));
    CHECK(vt[0] == (void*)original_destroy && vt[3] == (void*)original_movement && vt[1] == (void*)foreign);
    CHECK(unit_puppet_stop());
    VirtualFree((void*)base, 0, MEM_RELEASE);
    printf("UNIT puppet isolation/lifecycle checks passed.\n"); return 0;
}
