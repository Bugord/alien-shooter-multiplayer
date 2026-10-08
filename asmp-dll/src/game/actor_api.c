#include <windows.h>
#include <string.h>
#include "actor.h"

int actor_engine_bind(uintptr_t base, ActorEngine* output)
{
    static const unsigned char create[] = {0x55,0x8B,0xEC,0x6A,0xFF};
    static const unsigned char destroy[] = {0x55,0x8B,0xEC,0x56,0x8B,0xF1,0xC7,0x06};
    static const unsigned char move[] = {0x55,0x8B,0xEC,0xF3,0x0F,0x10,0x4D,0x08};
    static const unsigned char rotate[] = {0x55,0x8B,0xEC,0x83,0xEC,0x10,0x8A,0x45,0x08};
    static const unsigned char action[] = {0x55,0x8B,0xEC,0x53,0x8B,0x5D,0x08,0x56,0x57,0x8B,0xF9};
    static const unsigned char weapon[] = {0x55,0x8B,0xEC,0x51,0x56,0x8B,0xF1,0x57,0x8B,0x46,0x1C};
    static const unsigned char health[] = {0x55,0x8B,0xEC,0x56,0x57,0x8B,0x7D,0x08,0x8B,0xF1};
    memset(output, 0, sizeof(*output));
    __try {
        if (memcmp((void*)(base + STEAM_ACTOR_CREATE_RVA), create, sizeof(create)) ||
            memcmp((void*)(base + STEAM_ACTOR_DESTROY_RVA), destroy, sizeof(destroy)) ||
            memcmp((void*)(base + STEAM_ACTOR_MOVE_RVA), move, sizeof(move)) ||
            memcmp((void*)(base + STEAM_ACTOR_ROTATE_RVA), rotate, sizeof(rotate)) ||
            memcmp((void*)(base + STEAM_ACTOR_ACTION_RVA), action, sizeof(action)) ||
            memcmp((void*)(base + STEAM_ACTOR_WEAPON_RVA), weapon, sizeof(weapon)) ||
            memcmp((void*)(base + STEAM_ACTOR_HEALTH_RVA), health, sizeof(health)) ||
            *(uintptr_t*)(base + STEAM_MAN_VTABLE_RVA + 4u) != base + STEAM_ACTOR_ACTION_RVA ||
            *(uintptr_t*)(base + STEAM_GAME_VTABLE_RVA + 8 * sizeof(uintptr_t)) != base + STEAM_ACTOR_CREATE_RVA ||
            *(uintptr_t*)(base + STEAM_MAN_VTABLE_RVA) != base + STEAM_ACTOR_DESTROY_RVA ||
            /* Class 7 maps to jump-table index 4, whose arm constructs MAN.
               The switch's physical code order is not the class numbering. */
            *(unsigned char*)(base + STEAM_CLASS_INDEX_TABLE_RVA + STEAM_MAN_CLASS - 2u) != STEAM_MAN_JUMP_INDEX ||
            *(uintptr_t*)(base + STEAM_CLASS_JUMP_TABLE_RVA + STEAM_MAN_JUMP_INDEX * sizeof(uintptr_t)) != base + STEAM_MAN_CONSTRUCT_RVA) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    union { uintptr_t address; ActorCreate function; } c;
    union { uintptr_t address; ActorDestroy function; } d;
    union { uintptr_t address; ActorMove function; } m;
    union { uintptr_t address; ActorRotate function; } r;
    union { uintptr_t address; ActorAction function; } action_call;
    union { uintptr_t address; ActorWeapon function; } weapon_call;
    union { uintptr_t address; ActorHealth function; } health_call;
    c.address = base + STEAM_ACTOR_CREATE_RVA; d.address = base + STEAM_ACTOR_DESTROY_RVA;
    m.address = base + STEAM_ACTOR_MOVE_RVA; r.address = base + STEAM_ACTOR_ROTATE_RVA;
    output->create = c.function; output->destroy = d.function;
    output->move = m.function; output->rotate = r.function;
    action_call.address = base + STEAM_ACTOR_ACTION_RVA; output->action = action_call.function;
    weapon_call.address = base + STEAM_ACTOR_WEAPON_RVA; output->weapon = weapon_call.function;
    health_call.address = base + STEAM_ACTOR_HEALTH_RVA; output->health = health_call.function;
    return 1;
}
