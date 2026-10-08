#if defined(ASMP_STEAM_BUILD)
#include "steam_action_hook.h"
#include "../../multiplayer/steam/steam_multiplayer.h"
static uintptr_t image_base, aim_player;
static int aim_x, aim_y, has_aim;
static ActorAction original;
static SteamShotCallback notify;
static void* volatile* slot;
static volatile LONG enabled;
static int __fastcall action(void* entity, void* unused, unsigned int kind, intptr_t a, intptr_t b, intptr_t c)
{
    (void)unused;
    int local = 0; unsigned int weapon = 10;
    if (InterlockedCompareExchange(&enabled, 0, 0)) {
        __try {
            uintptr_t game = *(uintptr_t*)(image_base + STEAM_GAME_PTR_RVA);
            if (game && *(uintptr_t*)game == image_base + STEAM_GAME_VTABLE_RVA) {
                unsigned int army_id = *(unsigned int*)(game + STEAM_ARMY_INDEX_OFFSET) & 3u;
                uintptr_t army = *(uintptr_t*)(game + STEAM_ARMY_ARRAY_OFFSET + army_id * 4u);
                local = army && *(uintptr_t*)(army + STEAM_ARMY_PLAYER_OFFSET) == (uintptr_t)entity;
            }
            if (local) {
                if (aim_player != (uintptr_t)entity) { has_aim = 0; aim_player = (uintptr_t)entity; }
                if (kind == 0x25) { aim_x = (int)a; aim_y = (int)b; has_aim = 1; }
                uintptr_t vid = *(uintptr_t*)((uintptr_t)entity + STEAM_ENTITY_VID_OFFSET);
                uintptr_t linked = vid ? *(uintptr_t*)(vid + STEAM_VID_LINKED_OFFSET) : 0;
                if (linked) weapon = *(unsigned int*)(linked + STEAM_VID_INDEX_OFFSET) - 10u;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { local = 0; }
    }
    /* Health belongs to the remote owner. Local simulation must not kill its
       replica between owner snapshots; preserve ordinary damage to the local player. */
    if (!local && kind == 0x55 && InterlockedCompareExchange(&enabled, 0, 0) &&
        steam_multiplayer_is_replica((uintptr_t)entity)) return 0;
    int result = original(entity, NULL, kind, a, b, c);
    /* Port the original ammo-consuming shot hook, keeping file/network work
       out of native callbacks. Remote replay invokes the original directly. */
    if (local && has_aim && kind == 0x5D && a <= -1 && a >= -2 && weapon < 10u &&
        InterlockedCompareExchange(&enabled, 0, 0)) notify(aim_x, aim_y, weapon);
    return result;
}
static void* hook_address(void) {
    union { ActorAction call; void* address; } p; p.call = action; return p.address;
}
static int replace(void* expected, void* value) {
    DWORD protection, ignored;
    if (!VirtualProtect((void*)slot, sizeof(void*), PAGE_READWRITE, &protection)) return 0;
    void* previous = InterlockedCompareExchangePointer(slot, value, expected);
    int restored = VirtualProtect((void*)slot, sizeof(void*), protection, &ignored) != 0;
    return previous == expected && restored;
}
int steam_action_hook_install(uintptr_t base, SteamShotCallback callback) {
    if (slot || !callback) return 0;
    ActorEngine checked; if (!actor_engine_bind(base, &checked)) return 0;
    image_base = base; original = checked.action; notify = callback;
    slot = (void* volatile*)(base + STEAM_MAN_VTABLE_RVA + 4u);
    InterlockedExchange(&enabled, 1);
    if (!replace((void*)(base + 0x34470u), hook_address())) {
        InterlockedExchange(&enabled, 0);
        /* A protection-restore failure may have installed the callback. */
        replace(hook_address(), (void*)(base + 0x34470u)); return 0;
    }
    return 1;
}
int steam_action_hook_stop(void) {
    InterlockedExchange(&enabled, 0);
    return !slot || replace(hook_address(), (void*)(image_base + 0x34470u));
}
#endif
