#include <windows.h>
#include <string.h>
#include "dummy_actor.h"

#define CREATE_RVA 0x40680u
#define DESTROY_RVA 0x34440u
#define MOVE_RVA 0x6BC20u
#define ROTATE_RVA 0x6BD50u
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
    memset(output, 0, sizeof(*output));
    __try {
        if (memcmp((void*)(base + CREATE_RVA), create, sizeof(create)) ||
            memcmp((void*)(base + DESTROY_RVA), destroy, sizeof(destroy)) ||
            memcmp((void*)(base + MOVE_RVA), move, sizeof(move)) ||
            memcmp((void*)(base + ROTATE_RVA), rotate, sizeof(rotate)) ||
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
    c.address = base + CREATE_RVA; d.address = base + DESTROY_RVA;
    m.address = base + MOVE_RVA; r.address = base + ROTATE_RVA;
    output->create = c.function; output->destroy = d.function;
    output->move = m.function; output->rotate = r.function;
    return 1;
}

void dummy_actor_init(DummyActor* actor, const ActorEngine* engine)
{
    memset(actor, 0, sizeof(*actor));
    actor->engine = *engine;
}

/* Do not dereference a saved entity pointer until the current map's list owns
   it. Checking the private VID then also rejects a reused allocation address. */
static int registered(const DummyActor* actor, uintptr_t game)
{
    if (!actor->entity || actor->game != game || actor->category >= LIST_COUNT) return 0;
    uintptr_t list = game + LIST_FIRST + actor->category * 0x10u;
    unsigned int count = *(unsigned int*)(list + 4);
    unsigned int capacity = *(unsigned int*)(list + 8);
    uintptr_t* entries = *(uintptr_t**)(list + 12);
    if (!entries || count > capacity || count > 65536u) return 0;
    for (unsigned int i = 0; i < count; ++i) {
        if (entries[i] == actor->entity)
            return *(uintptr_t*)(actor->entity + STEAM_ENTITY_VID_OFFSET) == (uintptr_t)actor->vid;
    }
    return 0;
}

static int level_name(uintptr_t game, char name[128])
{
    const char* map = *(const char**)(game + 0x20);
    if (!map) return 0;
    unsigned int i;
    for (i = 0; i < 127 && map[i]; ++i) name[i] = map[i];
    name[i] = 0;
    if (i == 127 && map[i]) return 0;
    const char* file = strrchr(name, '\\');
    file = file ? file + 1 : name;
    return !_strnicmp(file, "level_", 6) || !_strnicmp(file, "survive_", 8);
}

ActorResult dummy_actor_tick(DummyActor* actor, uintptr_t base,
    const Snapshot* local, enum ProbeResult state, DWORD now, int stop)
{
    ActorResult result = {0};
    __try {
        uintptr_t game = *(uintptr_t*)(base + STEAM_GAME_PTR_RVA);
        int valid_game = game && *(uintptr_t*)game == base + STEAM_GAME_VTABLE_RVA;
        char map[128] = {0};
        int gameplay = valid_game && state == PROBE_OK && local->game == game &&
            local->health > 0 && level_name(game, map);
        int live = valid_game && registered(actor, game);
        if (actor->entity && !live) {
            result.event = ACTOR_LOST; result.entity = actor->entity;
            result.updates = actor->updates; actor->entity = 0;
        }
        int same_level = gameplay && actor->game == game && actor->player == local->player &&
            !strcmp(actor->map, map);
        if (live && (stop || !same_level || now - actor->spawned_at >= 60000u)) {
            result.event = ACTOR_REMOVED; result.entity = actor->entity;
            result.category = actor->category; result.updates = actor->updates;
            uintptr_t entity = actor->entity;
            actor->entity = 0;
            actor->engine.destroy((void*)entity, NULL, 1);
            return result;
        }
        if (stop) return result;
        if (!same_level) {
            actor->waiting = 0; actor->attempted = 0;
            actor->game = gameplay ? game : 0;
            actor->player = gameplay ? local->player : 0;
            strcpy_s(actor->map, sizeof(actor->map), map);
        }
        if (!gameplay) return result;
        if (!actor->waiting) { actor->ready_since = now; actor->waiting = 1; }
        if (!actor->entity && !actor->attempted && now - actor->ready_since >= 2000u) {
            actor->attempted = 1;
            uintptr_t source = *(uintptr_t*)(local->player + STEAM_ENTITY_VID_OFFSET);
            /* Steam's MAN branch is class 7, selected by MAP::create_entity.
               VID size is 0x490 (Steam allocations and copy sites). */
            result.source_class = source ? *(unsigned int*)(source + 0x10) : 0;
            if (!source || result.source_class != 7u) {
                result.event = ACTOR_REJECTED; result.reason = ACTOR_REASON_VID_CLASS; return result;
            }
            if (!*(uintptr_t*)(local->player + ENTITY_CHILD)) {
                result.event = ACTOR_REJECTED; result.reason = ACTOR_REASON_NO_CHILD; return result;
            }
            memcpy(actor->vid, (void*)source, sizeof(actor->vid));
            memset(actor->vid + 0x3A8, 0, 0x30); /* Private counts, deaths and recolors. */
            *(int32_t*)(actor->vid + 0x440) = -1; /* No local player's creation script. */
            *(int32_t*)(actor->vid + 0x44C) = -1; /* No local player's deletion script. */
            actor->category = *(unsigned int*)(actor->vid + VID_CATEGORY);
            result.category = actor->category;
            if (actor->category >= LIST_COUNT) {
                result.event = ACTOR_REJECTED; result.reason = ACTOR_REASON_CATEGORY; return result;
            }
            actor->entity = (uintptr_t)actor->engine.create((void*)game, NULL, actor->vid,
                local->x + 80.0f, local->y, local->z, local->direction, NULL);
            actor->spawned_at = now; actor->updates = 0;
            actor->last_pose = now;
            result.entity = actor->entity; result.category = actor->category;
            if (!actor->entity) result.reason = ACTOR_REASON_FACTORY;
            else if (!registered(actor, game)) result.reason = ACTOR_REASON_REGISTRATION;
            else if (*(uintptr_t*)actor->entity != base + STEAM_MAN_VTABLE_RVA)
                result.reason = ACTOR_REASON_ENTITY_TYPE;
            else if (*(uintptr_t*)(local->army + STEAM_ARMY_PLAYER_OFFSET) != local->player)
                result.reason = ACTOR_REASON_LOCAL_PLAYER;
            if (result.reason != ACTOR_REASON_NONE) { result.event = ACTOR_REJECTED; return result; }
            result.event = ACTOR_SPAWNED;
        }
        if (actor->entity && live) {
            actor->engine.move((void*)actor->entity, NULL, local->x + 80.0f, local->y, local->z);
            actor->engine.rotate((void*)actor->entity, NULL, local->direction);
            /* Port the legacy RPS_SPAWNED velocity/leg/torso updates from
               asmp-dll/src/multiplayer/multiplayer.c to the verified Steam ABI.
               Supply motion intent and speed. Native MAN logic chooses idle/run
               and advances its own frames and timers on subsequent ticks.
               Preserve all unrelated flags (army, collisions, ownership). */
            *(float*)(actor->entity + STEAM_ENTITY_VELOCITY_OFFSET) = local->velocity;
            uint32_t* flags = (uint32_t*)(actor->entity + STEAM_ENTITY_FLAGS_OFFSET);
            *flags = (*flags & ~STEAM_ENTITY_MOVING_FLAG) |
                (local->moving ? STEAM_ENTITY_MOVING_FLAG : 0u);
            result.applied_velocity = local->velocity;
            result.applied_moving = local->moving;
            result.native_animation = *(uint32_t*)(actor->entity + STEAM_ENTITY_ANIM_OFFSET);
            result.native_frame = *(uint32_t*)(actor->entity + STEAM_ENTITY_FRAME_CURRENT_OFFSET);
            uintptr_t child = *(uintptr_t*)(actor->entity + STEAM_ENTITY_CHILD_OFFSET);
            uintptr_t weapon = *(uintptr_t*)(actor->vid + STEAM_VID_LINKED_OFFSET);
            if (local->torso_present && child && weapon &&
                *(uintptr_t*)(child + STEAM_ENTITY_VID_OFFSET) == weapon) {
                /* Rotate legs first: that method may also rotate the child.
                   Apply the independent aim direction to the torso last. */
                actor->engine.rotate((void*)child, NULL, local->torso_direction);
                result.applied_torso = *(unsigned char*)(child + STEAM_ENTITY_DIRECTION_OFFSET);
                result.torso_present = 1;
            }
            ++actor->updates;
            if (now - actor->last_pose >= 1000u) {
                actor->last_pose = now;
                result.event = ACTOR_POSE; result.entity = actor->entity;
                result.category = actor->category; result.updates = actor->updates;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result.event = ACTOR_FAULT; result.reason = ACTOR_REASON_EXCEPTION;
    }
    return result;
}
