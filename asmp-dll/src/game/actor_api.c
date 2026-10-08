#include <windows.h>
#include <string.h>
#include "actor.h"

#define CREATE_RVA 0x40680u
#define DESTROY_RVA 0x34440u
#define MOVE_RVA 0x6BC20u
#define ROTATE_RVA 0x6BD50u
#define ACTION_RVA 0x34470u
#define WEAPON_RVA 0x34BA0u
#define HEALTH_RVA 0x6BFB0u
#define ENTITY_CHILD 0x40u
#define VID_CATEGORY 0x38Cu
#define LIST_FIRST 0x5Cu
#define LIST_COUNT 19u

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
        if (memcmp((void*)(base + CREATE_RVA), create, sizeof(create)) ||
            memcmp((void*)(base + DESTROY_RVA), destroy, sizeof(destroy)) ||
            memcmp((void*)(base + MOVE_RVA), move, sizeof(move)) ||
            memcmp((void*)(base + ROTATE_RVA), rotate, sizeof(rotate)) ||
            memcmp((void*)(base + ACTION_RVA), action, sizeof(action)) ||
            memcmp((void*)(base + WEAPON_RVA), weapon, sizeof(weapon)) ||
            memcmp((void*)(base + HEALTH_RVA), health, sizeof(health)) ||
            *(uintptr_t*)(base + STEAM_MAN_VTABLE_RVA + 4u) != base + ACTION_RVA ||
            *(uintptr_t*)(base + STEAM_GAME_VTABLE_RVA + 8 * sizeof(uintptr_t)) != base + CREATE_RVA ||
            *(uintptr_t*)(base + STEAM_MAN_VTABLE_RVA) != base + DESTROY_RVA ||
            /* Class 7 maps to jump-table index 4, whose arm constructs MAN.
               The switch's physical code order is not the class numbering. */
            *(unsigned char*)(base + 0x394B4u + 7u - 2u) != 4u ||
            *(uintptr_t*)(base + 0x39484u + 4u * sizeof(uintptr_t)) != base + 0x3934Cu) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    union { uintptr_t address; ActorCreate function; } c;
    union { uintptr_t address; ActorDestroy function; } d;
    union { uintptr_t address; ActorMove function; } m;
    union { uintptr_t address; ActorRotate function; } r;
    union { uintptr_t address; ActorAction function; } action_call;
    union { uintptr_t address; ActorWeapon function; } weapon_call;
    union { uintptr_t address; ActorHealth function; } health_call;
    c.address = base + CREATE_RVA; d.address = base + DESTROY_RVA;
    m.address = base + MOVE_RVA; r.address = base + ROTATE_RVA;
    output->create = c.function; output->destroy = d.function;
    output->move = m.function; output->rotate = r.function;
    action_call.address = base + ACTION_RVA; output->action = action_call.function;
    weapon_call.address = base + WEAPON_RVA; output->weapon = weapon_call.function;
    health_call.address = base + HEALTH_RVA; output->health = health_call.function;
    return 1;
}
