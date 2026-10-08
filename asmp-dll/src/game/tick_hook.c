#include "tick_hook.h"
#include "clock.h"
typedef int (__fastcall* TickFn)(void*, void*);
static struct {
    SlotHook hook;
    TickFn original;
    TickCallback notify;
    volatile LONG enabled, calls, active;
} tick;
static int __fastcall on_tick(void* game, void* unused) {
    (void)unused;
    int result = tick.original(game, NULL);
    LONG call = InterlockedIncrement(&tick.calls);
    if (!result) {
        InterlockedIncrement(&tick.active);
        if (InterlockedCompareExchange(&tick.enabled, 0, 0)) tick.notify(game, call, clock_ms());
        InterlockedDecrement(&tick.active);
    }
    return result;
}
enum SlotResult tick_hook_install(void* volatile* slot, void* original, TickCallback callback) {
    if (tick.hook.slot) return SLOT_USED;
    if (!callback) return SLOT_INVALID;
    union { void* pointer; TickFn function; } p;
    p.pointer = original; tick.original = p.function; tick.notify = callback;
    InterlockedExchange(&tick.calls, 0); InterlockedExchange(&tick.enabled, 1);
    p.function = on_tick;
    enum SlotResult result = slot_install(&tick.hook, slot, original, p.pointer);
    if (result != SLOT_OK) InterlockedExchange(&tick.enabled, 0);
    return result;
}
enum SlotResult tick_hook_stop(void) {
    InterlockedExchange(&tick.enabled, 0);
    enum SlotResult result = slot_stop(&tick.hook);
    return result == SLOT_OK && InterlockedCompareExchange(&tick.active, 0, 0) ? SLOT_BUSY : result;
}
LONG tick_hook_calls(void) { return InterlockedCompareExchange(&tick.calls, 0, 0); }
int tick_hook_ready(void) { return InterlockedCompareExchange(&tick.hook.installed, 0, 0) != 0; }
