#if defined(ASMP_STEAM_BUILD)
#include <windows.h>
#include <string.h>
#include "steam_actor.h"
#define ENTITY_CHILD 0x40u
#define VID_CATEGORY 0x38Cu
#define LIST_FIRST 0x5Cu
#define LIST_COUNT 19u
void steam_actor_init(SteamActor* actor, const ActorEngine* engine)
{
    memset(actor, 0, sizeof(*actor));
    actor->engine = *engine;
    actor->armed_weapon = -1;
}

/* Do not dereference a saved entity pointer until the current map's list owns
   it. Checking the private VID then also rejects a reused allocation address. */
static int registered(const SteamActor* actor, uintptr_t game)
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

int steam_actor_live(const SteamActor* actor, uintptr_t game)
{
    __try { return registered(actor, game); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
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

ActorResult steam_actor_tick(SteamActor* actor, uintptr_t base,
    const Snapshot* local, enum ProbeResult state, const Snapshot* target, DWORD now, int stop, int combat, float offset, DWORD lifetime)
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
        if (live && (stop || !same_level || (lifetime && now - actor->spawned_at >= lifetime))) {
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
                target->x + offset, target->y, target->z, target->direction, NULL);
            actor->spawned_at = now; actor->updates = 0;
            actor->prepared = 0; actor->armed_weapon = -1;
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
            if (combat) {
                /* RPS_SPAWNING waits for the native linked torso. */
                uintptr_t torso = *(uintptr_t*)(actor->entity + ENTITY_CHILD);
                if (!torso || *(uintptr_t*)(torso + STEAM_ENTITY_VID_OFFSET) !=
                    *(uintptr_t*)(actor->vid + STEAM_VID_LINKED_OFFSET)) return result;
                if (!actor->prepared) {
                    /* Original RPS_SPAWNING/RPS_JUST_SPAWNED: put the remote
                       actor in the opposing army and prepare its weapons. */
                    actor->engine.action((void*)actor->entity, NULL, 0x61,
                        (local->army_index + 1u) & 3u, 0, 0);
                    for (int slot = 2; slot < 10; ++slot) {
                        if (!actor->engine.action((void*)actor->entity, NULL, 0x38, 260 + slot, 0, 0))
                            actor->engine.action((void*)actor->entity, NULL, 0x36, 260 + slot, 0, 0);
                    }
                    actor->prepared = 1;
                }
                if (target->weapon_slot >= 0 && target->weapon_slot != actor->armed_weapon) {
                    if (!actor->engine.weapon((void*)actor->entity, NULL, target->weapon_slot)) {
                        result.event = ACTOR_REJECTED; result.reason = ACTOR_REASON_WEAPON;
                        return result;
                    }
                    actor->armed_weapon = target->weapon_slot;
                }
                actor->engine.health((void*)actor->entity, NULL, target->health);
                /* Keep the signed live fixed-point count, avoiding integer
                   overflow from untrusted wire values. Weapon changes remain
                   native operations; ammo is verified UNIT storage. */
                int64_t raw_ammo = (int64_t)target->current_ammo * STEAM_AMMO_SCALE;
                if (raw_ammo > INT32_MAX) raw_ammo = INT32_MAX;
                if (raw_ammo < 0) raw_ammo = 0;
                *(int32_t*)(actor->entity + STEAM_CURRENT_AMMO_OFFSET) = (int32_t)raw_ammo;
            }
            actor->engine.move((void*)actor->entity, NULL, target->x + offset, target->y, target->z);
            actor->engine.rotate((void*)actor->entity, NULL, target->direction);
            /* Port the legacy RPS_SPAWNED velocity/leg/torso updates from
               asmp-dll/src/multiplayer/multiplayer.c to the verified Steam ABI.
               Supply motion intent and speed. Native MAN logic chooses idle/run
               and advances its own frames and timers on subsequent ticks.
               Preserve all unrelated flags (army, collisions, ownership). */
            *(float*)(actor->entity + STEAM_ENTITY_VELOCITY_OFFSET) = target->velocity;
            uint32_t* flags = (uint32_t*)(actor->entity + STEAM_ENTITY_FLAGS_OFFSET);
            *flags = (*flags & ~STEAM_ENTITY_MOVING_FLAG) |
                (target->moving ? STEAM_ENTITY_MOVING_FLAG : 0u);
            result.applied_velocity = target->velocity;
            result.applied_moving = target->moving;
            result.native_animation = *(uint32_t*)(actor->entity + STEAM_ENTITY_ANIM_OFFSET);
            result.native_frame = *(uint32_t*)(actor->entity + STEAM_ENTITY_FRAME_CURRENT_OFFSET);
            uintptr_t child = *(uintptr_t*)(actor->entity + STEAM_ENTITY_CHILD_OFFSET);
            uintptr_t weapon = *(uintptr_t*)(actor->vid + STEAM_VID_LINKED_OFFSET);
            if (target->torso_present && child && weapon &&
                *(uintptr_t*)(child + STEAM_ENTITY_VID_OFFSET) == weapon) {
                /* Rotate legs first: that method may also rotate the child.
                   Apply the independent aim direction to the torso last. */
                actor->engine.rotate((void*)child, NULL, target->torso_direction);
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

int steam_actor_shoot(SteamActor* actor, uintptr_t game, int x, int y, int weapon)
{
    __try {
        if (!registered(actor, game) || !actor->prepared || weapon < 0 || weapon >= 10) return 0;
        if (actor->armed_weapon != weapon) {
            if (!actor->engine.weapon((void*)actor->entity, NULL, weapon)) return 0;
            actor->armed_weapon = weapon;
        }
        /* Same native attack path as the original on_actor_shoot callback.
           Reserve ammo for this received event if a later state arrived first. */
        int ammo = actor->engine.action((void*)actor->entity, NULL, 0x5C, 0, 0, 0);
        /* Some weapons consume two units. A post-shot owner snapshot may
           leave one unit, which is still insufficient to replay that event. */
        if (ammo < 2)
            actor->engine.action((void*)actor->entity, NULL, 0x5D, 2 - (intptr_t)ammo, 0, 0);
        actor->engine.action((void*)actor->entity, NULL, 0x25, x, y, 0);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

#endif
