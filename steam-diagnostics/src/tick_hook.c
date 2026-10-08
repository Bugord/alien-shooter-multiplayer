#include <windows.h>
#include <stdint.h>
#include "tick_hook.h"
#include "../../asmp-dll/src/multiplayer/steam/steam_session.h"

/* MAP_STEAM::tick has this in ECX, no stack arguments, and an int return.
   __fastcall supplies the same ECX; the unused EDX argument is ignored. */
/* MSVC C cannot declare __thiscall pointers. With no stack arguments the
   __fastcall(game, unused_edx) ABI has the same stack and ECX contract. */
typedef int (__fastcall* TickFn)(void* game, void* unused_edx);
static union { void* pointer; TickFn function; } original;
static void* volatile* target_slot;
static uintptr_t base;
static volatile LONG used, enabled, installed, calls, captured, dropped;
static SRWLOCK queue_lock = SRWLOCK_INIT;
static FrameSample queue[FRAME_QUEUE_CAPACITY];
static unsigned int head, count;
static DummyActor dummy;
static int dummy_enabled;
static volatile LONG dummy_stop, dummy_done = 1;
static int __fastcall on_tick(void* game, void* unused);

static void* hook_pointer(void)
{
    union { int (__fastcall* function)(void*, void*); void* pointer; } address;
    address.function = on_tick;
    return address.pointer;
}

/* Only a single aligned data pointer is exchanged, without patching code.
   Never overwrite a slot changed by another hook. Keep its page protection. */
static enum TickHookResult exchange_slot(void* from, void* to)
{
    MEMORY_BASIC_INFORMATION info;
    DWORD old_protection, ignored;
    if (!target_slot || (uintptr_t)target_slot % sizeof(void*) ||
        !VirtualQuery((void*)target_slot, &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return TICK_HOOK_INVALID_SLOT;
    DWORD writable = (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    if (!VirtualProtect((void*)target_slot, sizeof(void*), writable, &old_protection))
        return TICK_HOOK_PROTECT_FAILED;
    void* previous = InterlockedCompareExchangePointer(target_slot, to, from);
    if (previous == from) InterlockedExchange(&installed, to == hook_pointer());
    if (!VirtualProtect((void*)target_slot, sizeof(void*), old_protection, &ignored))
        return TICK_HOOK_PROTECTION_RESTORE_FAILED;
    return previous == from ? TICK_HOOK_OK : TICK_HOOK_SLOT_CHANGED;
}

enum TickHookResult tick_hook_install(void* volatile* slot, void* expected, uintptr_t image_base)
{
    if (InterlockedCompareExchange(&used, 1, 0)) return TICK_HOOK_ALREADY_USED;
    target_slot = slot;
    original.pointer = expected;
    base = image_base;
    if (!expected || !image_base) return TICK_HOOK_INVALID_SLOT;
    /* Initialize before publishing the callback pointer to the game thread. */
    InterlockedExchange(&enabled, 1);
    enum TickHookResult result = exchange_slot(expected, hook_pointer());
    if (result != TICK_HOOK_OK) InterlockedExchange(&enabled, 0);
    return result;
}

static int __fastcall on_tick(void* game, void* unused)
{
    (void)unused;
    int result = original.function(game, NULL);
    LONG tick = InterlockedIncrement(&calls);
    /* Nonzero ends WinMain's loop; don't dereference gameplay state on exit. */
    if (!result && InterlockedCompareExchange(&enabled, 0, 0)) {
        FrameSample frame = {0};
        frame.milliseconds = GetTickCount();
        frame.tick = tick;
        frame.session_result = steam_session_tick();
        frame.result = probe_read(base, &frame.snapshot);
        frame.multiplayer = steam_multiplayer_tick(&frame.snapshot, frame.result, frame.milliseconds);
        if (dummy_enabled && !InterlockedCompareExchange(&dummy_done, 0, 0)) {
            int stopping = InterlockedCompareExchange(&dummy_stop, 0, 0) != 0;
            frame.actor = dummy_actor_tick(&dummy, base, &frame.snapshot,
                frame.result, frame.milliseconds, stopping);
            if (frame.actor.event == ACTOR_FAULT) {
                /* Do not retry engine mutations after an exception. The map
                   still owns any surviving actor until it unloads. */
                InterlockedExchange(&dummy_done, 1);
            } else if (frame.actor.event == ACTOR_REJECTED) {
                InterlockedExchange(&dummy_stop, 1);
            }
            if (stopping && !dummy.entity) InterlockedExchange(&dummy_done, 1);
        }
        /* Never wait on the logging thread from the game thread. */
        if (TryAcquireSRWLockExclusive(&queue_lock)) {
            if (count < FRAME_QUEUE_CAPACITY) {
                queue[(head + count) % FRAME_QUEUE_CAPACITY] = frame;
                ++count;
                InterlockedIncrement(&captured);
            } else InterlockedIncrement(&dropped);
            ReleaseSRWLockExclusive(&queue_lock);
        } else InterlockedIncrement(&dropped);
    }
    return result;
}

enum TickHookResult tick_hook_stop(void)
{
    InterlockedExchange(&enabled, 0);
    if (!InterlockedCompareExchange(&installed, 0, 0)) return TICK_HOOK_OK;
    return exchange_slot(hook_pointer(), original.pointer);
}

unsigned int tick_hook_drain(FrameSample* output, unsigned int capacity)
{
    AcquireSRWLockExclusive(&queue_lock);
    unsigned int n = count < capacity ? count : capacity;
    for (unsigned int i = 0; i < n; ++i) output[i] = queue[(head + i) % FRAME_QUEUE_CAPACITY];
    head = (head + n) % FRAME_QUEUE_CAPACITY;
    count -= n;
    ReleaseSRWLockExclusive(&queue_lock);
    return n;
}

TickStats tick_hook_stats(void)
{
    TickStats stats;
    stats.calls = InterlockedCompareExchange(&calls, 0, 0);
    stats.captured = InterlockedCompareExchange(&captured, 0, 0);
    stats.dropped = InterlockedCompareExchange(&dropped, 0, 0);
    stats.installed = InterlockedCompareExchange(&installed, 0, 0);
    return stats;
}

int tick_hook_enable_dummy(uintptr_t image_base)
{
    ActorEngine engine;
    if (InterlockedCompareExchange(&used, 0, 0) || !actor_engine_bind(image_base, &engine)) return 0;
    dummy_actor_init(&dummy, &engine);
    dummy_enabled = 1;
    InterlockedExchange(&dummy_done, 0);
    return 1;
}
void tick_hook_request_dummy_stop(void) { InterlockedExchange(&dummy_stop, 1); }
int tick_hook_dummy_stopped(void) { return InterlockedCompareExchange(&dummy_done, 0, 0) != 0; }
