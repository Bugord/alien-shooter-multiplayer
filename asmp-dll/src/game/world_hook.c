#include <string.h>
#include "world_hook.h"
#include "steam_profile.h"
typedef void (__fastcall* LoadMapFn)(void*, void*, char*);
static struct { SlotHook hook; LoadMapFn original; volatile LONG generation; } world;
static void __fastcall on_load(void* game, void* unused, char* map) {
    (void)unused;
    world.original(game, NULL, map);
    InterlockedIncrement(&world.generation);
}
int world_hook_install(uintptr_t base) {
    if (world.hook.slot) return 0;
    static const unsigned char prefix[] = {0x55,0x8B,0xEC,0x6A,0xFF};
    __try { if (memcmp((void*)(base + STEAM_LOAD_MAP_RVA), prefix, sizeof(prefix))) return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    union { LoadMapFn function; void* pointer; } p;
    p.pointer = (void*)(base + STEAM_LOAD_MAP_RVA); world.original = p.function;
    p.function = on_load;
    InterlockedExchange(&world.generation, 0);
    return slot_install(&world.hook, (void* volatile*)(base + STEAM_GAME_VTABLE_RVA + STEAM_LOAD_MAP_SLOT * sizeof(void*)),
        (void*)(base + STEAM_LOAD_MAP_RVA), p.pointer) == SLOT_OK;
}
int world_hook_stop(void) { return slot_stop(&world.hook) == SLOT_OK; }
uint32_t world_hook_generation(void) { return (uint32_t)InterlockedCompareExchange(&world.generation, 0, 0); }
