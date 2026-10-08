#if defined(ASMP_STEAM_BUILD)
#include <windows.h>
#include <string.h>
#include "steam_actor.h"

void steam_actor_init(SteamActor* a, const ActorEngine* engine)
{
    memset(a, 0, sizeof(*a)); a->engine = *engine; a->armed_weapon = -1;
}
/* Saved pointers are inspected only after the current map list owns them. */
static int registered(const SteamActor* a, uintptr_t game)
{
    if (!game || !a->entity || a->game != game || a->category >= STEAM_WORLD_LIST_COUNT) return 0;
    uintptr_t list = game + STEAM_WORLD_LIST_OFFSET + a->category * STEAM_LIST_SIZE;
    unsigned int count = *(unsigned int*)(list + 4), capacity = *(unsigned int*)(list + 8);
    uintptr_t* entries = *(uintptr_t**)(list + 12);
    if (!entries || count > capacity || count > 65536u) return 0;
    for (unsigned int i = 0; i < count; ++i)
        if (entries[i] == a->entity)
            return *(uintptr_t*)(a->entity + STEAM_ENTITY_VID_OFFSET) == (uintptr_t)a->vid;
    return 0;
}
int steam_actor_live(const SteamActor* a, uintptr_t game)
{
    __try { return registered(a, game); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static ActorResult diagnostic(const SteamActor* a, enum ActorEvent event, enum ActorReason reason)
{
    ActorResult r = {0}; r.event = event; r.reason = reason;
    r.entity = a->entity; r.category = a->category; r.updates = a->updates; return r;
}
ActorResult steam_actor_spawn(SteamActor* a, uintptr_t base, const Snapshot* local, const Snapshot* target)
{
    ActorResult r = {0};
    __try {
        if (a->entity || !local->game || *(uintptr_t*)local->game != base + STEAM_GAME_VTABLE_RVA)
            return diagnostic(a, ACTOR_REJECTED, ACTOR_REASON_ENTITY_TYPE);
        uintptr_t source = *(uintptr_t*)(local->player + STEAM_ENTITY_VID_OFFSET);
        r.source_class = source ? *(unsigned int*)(source + STEAM_VID_CLASS_OFFSET) : 0;
        if (!source || r.source_class != 7u) { r.event = ACTOR_REJECTED; r.reason = ACTOR_REASON_VID_CLASS; return r; }
        if (!*(uintptr_t*)(local->player + STEAM_ENTITY_CHILD_OFFSET))
            return diagnostic(a, ACTOR_REJECTED, ACTOR_REASON_NO_CHILD);
        memcpy(a->vid, (void*)source, sizeof(a->vid));
        memset(a->vid + STEAM_VID_COUNTERS_OFFSET, 0, STEAM_VID_COUNTERS_SIZE);
        *(int32_t*)(a->vid + STEAM_VID_CREATE_SCRIPT_OFFSET) = -1;
        *(int32_t*)(a->vid + STEAM_VID_DELETE_SCRIPT_OFFSET) = -1;
        a->category = *(unsigned int*)(a->vid + STEAM_VID_CATEGORY_OFFSET);
        if (a->category >= STEAM_WORLD_LIST_COUNT) return diagnostic(a, ACTOR_REJECTED, ACTOR_REASON_CATEGORY);
        a->game = local->game; a->player = local->player;
        a->entity = (uintptr_t)a->engine.create((void*)local->game, NULL, a->vid,
            target->x, target->y, target->z, target->direction, NULL);
        a->updates = 0; a->armed_weapon = -1;
        if (!a->entity) return diagnostic(a, ACTOR_REJECTED, ACTOR_REASON_FACTORY);
        if (!registered(a, local->game)) return diagnostic(a, ACTOR_REJECTED, ACTOR_REASON_REGISTRATION);
        if (*(uintptr_t*)a->entity != base + STEAM_MAN_VTABLE_RVA) return diagnostic(a, ACTOR_REJECTED, ACTOR_REASON_ENTITY_TYPE);
        if (*(uintptr_t*)(local->army + STEAM_ARMY_PLAYER_OFFSET) != local->player)
            return diagnostic(a, ACTOR_REJECTED, ACTOR_REASON_LOCAL_PLAYER);
        return diagnostic(a, ACTOR_SPAWNED, ACTOR_REASON_NONE);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return diagnostic(a, ACTOR_FAULT, ACTOR_REASON_EXCEPTION); }
}
ActorResult steam_actor_remove(SteamActor* a, uintptr_t game)
{
    ActorResult r = diagnostic(a, ACTOR_NONE, ACTOR_REASON_NONE);
    __try {
        if (!a->entity) return r;
        if (!registered(a, game)) { a->entity = 0; r.event = ACTOR_LOST; return r; }
        a->engine.destroy((void*)a->entity, NULL, 1);
        a->entity = 0; a->armed_weapon = -1; r.event = ACTOR_REMOVED; return r;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return diagnostic(a, ACTOR_FAULT, ACTOR_REASON_EXCEPTION); }
}
int steam_actor_torso_ready(const SteamActor* a)
{
    __try {
        if (!registered(a, a->game)) return 0;
        uintptr_t torso = *(uintptr_t*)(a->entity + STEAM_ENTITY_CHILD_OFFSET);
        return torso && *(uintptr_t*)(torso + STEAM_ENTITY_VID_OFFSET) == *(uintptr_t*)(a->vid + STEAM_VID_LINKED_OFFSET);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
int steam_actor_set_army(SteamActor* a, unsigned int army)
{
    __try {
        if (army > 3u || !registered(a, a->game)) return 0;
        a->engine.action((void*)a->entity, NULL, 0x61, army, 0, 0); return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
int steam_actor_arm(SteamActor* a, int slot)
{
    __try {
        if (slot < 0 || slot >= (int)STEAM_WEAPON_SLOT_COUNT || !registered(a, a->game)) return 0;
        if (slot == a->armed_weapon) return 1;
        if (!a->engine.action((void*)a->entity, NULL, 0x38, 260 + slot, 0, 0))
            a->engine.action((void*)a->entity, NULL, 0x36, 260 + slot, 0, 0);
        if (!a->engine.weapon((void*)a->entity, NULL, slot)) return 0;
        a->armed_weapon = slot; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
ActorResult steam_actor_apply(SteamActor* a, const Snapshot* target)
{
    __try {
        if (!registered(a, a->game)) { ActorResult r = diagnostic(a, ACTOR_LOST, ACTOR_REASON_NONE); a->entity = 0; return r; }
        a->engine.move((void*)a->entity, NULL, target->x, target->y, target->z);
        a->engine.rotate((void*)a->entity, NULL, target->direction);
        *(float*)(a->entity + STEAM_ENTITY_VELOCITY_OFFSET) = target->velocity;
        uint32_t* flags = (uint32_t*)(a->entity + STEAM_ENTITY_FLAGS_OFFSET);
        *flags = (*flags & ~STEAM_ENTITY_MOVING_FLAG) | (target->moving ? STEAM_ENTITY_MOVING_FLAG : 0u);
        ActorResult r = diagnostic(a, ACTOR_POSE, ACTOR_REASON_NONE);
        r.applied_velocity = target->velocity; r.applied_moving = target->moving;
        r.native_animation = *(uint32_t*)(a->entity + STEAM_ENTITY_ANIM_OFFSET);
        r.native_frame = *(uint32_t*)(a->entity + STEAM_ENTITY_FRAME_CURRENT_OFFSET);
        uintptr_t child = *(uintptr_t*)(a->entity + STEAM_ENTITY_CHILD_OFFSET);
        uintptr_t linked = *(uintptr_t*)(a->vid + STEAM_VID_LINKED_OFFSET);
        if (target->torso_present && child && linked && *(uintptr_t*)(child + STEAM_ENTITY_VID_OFFSET) == linked) {
            a->engine.rotate((void*)child, NULL, target->torso_direction);
            r.applied_torso = *(unsigned char*)(child + STEAM_ENTITY_DIRECTION_OFFSET); r.torso_present = 1;
        }
        if (a->engine.health) {
            a->engine.health((void*)a->entity, NULL, target->health);
            int64_t ammo = (int64_t)target->current_ammo * STEAM_AMMO_SCALE;
            if (ammo > INT32_MAX) ammo = INT32_MAX;
            if (ammo < 0) ammo = 0;
            *(int32_t*)(a->entity + STEAM_CURRENT_AMMO_OFFSET) = (int32_t)ammo;
        }
        r.updates = ++a->updates; return r;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return diagnostic(a, ACTOR_FAULT, ACTOR_REASON_EXCEPTION); }
}
int steam_actor_shoot(SteamActor* a, uintptr_t game, int x, int y, int weapon)
{
    __try {
        if (!registered(a, game) || steam_actor_torso_ready(a) != 1) return 0;
        int armed = steam_actor_arm(a, weapon); if (armed != 1) return armed;
        int ammo = a->engine.action((void*)a->entity, NULL, 0x5C, 0, 0, 0);
        if (ammo < 2) a->engine.action((void*)a->entity, NULL, 0x5D, 2 - (intptr_t)ammo, 0, 0);
        a->engine.action((void*)a->entity, NULL, 0x25, x, y, 0); return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
#endif
