#include "action_hook.h"
#include "slot_hook.h"
static struct {
    uintptr_t image_base, aim_player;
    int aim_x, aim_y, has_aim;
    ActorAction original;
    ShotCallback notify;
    SlotHook hook;
    ReplicaFilter replica;
    volatile LONG enabled;
} action_state;
static int __fastcall action(void* entity, void* unused, unsigned int kind, intptr_t a, intptr_t b, intptr_t c)
{
    (void)unused;
    int local = 0; unsigned int weapon = 10;
    if (InterlockedCompareExchange(&action_state.enabled, 0, 0)) {
        __try {
            uintptr_t game = *(uintptr_t*)(action_state.image_base + STEAM_GAME_PTR_RVA);
            if (game && *(uintptr_t*)game == action_state.image_base + STEAM_GAME_VTABLE_RVA) {
                unsigned int army_id = *(unsigned int*)(game + STEAM_ARMY_INDEX_OFFSET) & 3u;
                uintptr_t army = *(uintptr_t*)(game + STEAM_ARMY_ARRAY_OFFSET + army_id * 4u);
                local = army && *(uintptr_t*)(army + STEAM_ARMY_PLAYER_OFFSET) == (uintptr_t)entity;
            }
            if (local) {
                if (action_state.aim_player != (uintptr_t)entity) { action_state.has_aim = 0; action_state.aim_player = (uintptr_t)entity; }
                if (kind == 0x25) { action_state.aim_x = (int)a; action_state.aim_y = (int)b; action_state.has_aim = 1; }
                uintptr_t vid = *(uintptr_t*)((uintptr_t)entity + STEAM_ENTITY_VID_OFFSET);
                uintptr_t linked = vid ? *(uintptr_t*)(vid + STEAM_VID_LINKED_OFFSET) : 0;
                if (linked) weapon = *(unsigned int*)(linked + STEAM_VID_INDEX_OFFSET) - 10u;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { local = 0; }
    }
    /* Health belongs to the remote owner. Local simulation must not kill its
       replica between owner snapshots; preserve ordinary damage to the local player. */
    if (!local && kind == 0x55 && InterlockedCompareExchange(&action_state.enabled, 0, 0) &&
        action_state.replica((uintptr_t)entity)) return 0;
    int result = action_state.original(entity, NULL, kind, a, b, c);
    /* Capture actual ammo-consuming attacks without file/network work in native
       callbacks. Remote replay invokes the original action directly. */
    if (local && action_state.has_aim && kind == 0x5D && a <= -1 && a >= -2 && weapon < 10u &&
        InterlockedCompareExchange(&action_state.enabled, 0, 0)) action_state.notify(action_state.aim_x, action_state.aim_y, weapon);
    return result;
}
static void* hook_address(void) {
    union { ActorAction call; void* address; } p; p.call = action; return p.address;
}
int action_hook_install(uintptr_t base, ReplicaFilter filter, ShotCallback callback) {
    if (action_state.hook.slot || !filter || !callback) return 0;
    ActorEngine checked; if (!actor_engine_bind(base, &checked)) return 0;
    action_state.image_base = base; action_state.original = checked.action; action_state.notify = callback; action_state.replica = filter;
    action_state.aim_player = 0; action_state.has_aim = 0;
    InterlockedExchange(&action_state.enabled, 1);
    enum SlotResult result = slot_install(&action_state.hook,
        (void* volatile*)(base + STEAM_MAN_VTABLE_RVA + 4u),
        (void*)(base + STEAM_ACTOR_ACTION_RVA), hook_address());
    if (result != SLOT_OK) { InterlockedExchange(&action_state.enabled, 0); return 0; }
    return 1;
}
int action_hook_stop(void) {
    InterlockedExchange(&action_state.enabled, 0);
    int ok = slot_stop(&action_state.hook) == SLOT_OK;
    if (ok) { action_state.aim_player = 0; action_state.has_aim = 0; }
    return ok;
}
