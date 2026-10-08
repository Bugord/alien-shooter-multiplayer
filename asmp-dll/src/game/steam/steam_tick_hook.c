#if defined(ASMP_STEAM_BUILD)
#include "steam_tick_hook.h"
typedef int (__fastcall* TickFn)(void*, void*);
static struct {
    SteamSlotHook hook;
    TickFn original;
    SteamTickCallback notify;
    volatile LONG enabled, calls, active;
} tick;
static int __fastcall on_tick(void* game, void* unused) {
    (void)unused;
    int result = tick.original(game, NULL);
    LONG call = InterlockedIncrement(&tick.calls);
    if (!result) {
        InterlockedIncrement(&tick.active);
        if (InterlockedCompareExchange(&tick.enabled, 0, 0)) tick.notify(game, call, GetTickCount());
        InterlockedDecrement(&tick.active);
    }
    return result;
}
enum SteamSlotResult steam_tick_hook_install(void* volatile* slot, void* original, SteamTickCallback callback) {
    if (tick.hook.slot) return STEAM_SLOT_USED;
    if (!callback) return STEAM_SLOT_INVALID;
    union { void* pointer; TickFn function; } p;
    p.pointer = original; tick.original = p.function; tick.notify = callback;
    InterlockedExchange(&tick.calls, 0); InterlockedExchange(&tick.enabled, 1);
    p.function = on_tick;
    enum SteamSlotResult result = steam_slot_install(&tick.hook, slot, original, p.pointer);
    if (result != STEAM_SLOT_OK) InterlockedExchange(&tick.enabled, 0);
    return result;
}
enum SteamSlotResult steam_tick_hook_stop(void) {
    InterlockedExchange(&tick.enabled, 0);
    enum SteamSlotResult result = steam_slot_stop(&tick.hook);
    return result == STEAM_SLOT_OK && InterlockedCompareExchange(&tick.active, 0, 0) ? STEAM_SLOT_BUSY : result;
}
LONG steam_tick_hook_calls(void) { return InterlockedCompareExchange(&tick.calls, 0, 0); }
int steam_tick_hook_ready(void) { return InterlockedCompareExchange(&tick.hook.installed, 0, 0) != 0; }
#endif
