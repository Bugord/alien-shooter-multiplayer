#include <windows.h>
#include <stdint.h>
#include "diag_tick.h"


static uintptr_t base;
static volatile LONG used, captured, dropped;
static SRWLOCK queue_lock = SRWLOCK_INIT;
static FrameSample queue[FRAME_QUEUE_CAPACITY];
static unsigned int head, count;
static DummyActor dummy;
static int dummy_enabled;
static volatile LONG dummy_stop, dummy_done = 1;
static void enqueue(const FrameSample* frame) {
    if (TryAcquireSRWLockExclusive(&queue_lock)) {
        if (count < FRAME_QUEUE_CAPACITY) {
            queue[(head + count) % FRAME_QUEUE_CAPACITY] = *frame; ++count; InterlockedIncrement(&captured);
        } else InterlockedIncrement(&dropped);
        ReleaseSRWLockExclusive(&queue_lock);
    } else InterlockedIncrement(&dropped);
}
void diag_tick_prepare_observer(void) {
    AcquireSRWLockExclusive(&queue_lock); head = count = 0; ReleaseSRWLockExclusive(&queue_lock);
    InterlockedExchange(&captured, 0); InterlockedExchange(&dropped, 0);
}
void diag_tick_observe(const RuntimeFrame* input) {
    FrameSample frame = {0};
    frame.milliseconds = input->milliseconds; frame.tick = input->tick;
    frame.session_result = input->session_result; frame.result = input->result;
    frame.snapshot = input->snapshot; frame.multiplayer = input->multiplayer;
    frame.session = input->session; frame.display = input->display;
    enqueue(&frame);
}
static void on_tick(void* game, LONG tick, DWORD now) {
    (void)game;
    FrameSample frame = {0}; frame.milliseconds = now; frame.tick = tick;
    frame.result = probe_read(base, &frame.snapshot);
    if (dummy_enabled && !InterlockedCompareExchange(&dummy_done, 0, 0)) {
        int stopping = InterlockedCompareExchange(&dummy_stop, 0, 0) != 0;
        frame.actor = dummy_actor_tick(&dummy, base, &frame.snapshot, frame.result, now, stopping);
        if (frame.actor.event == ACTOR_FAULT || frame.actor.event == ACTOR_REJECTED) InterlockedExchange(&dummy_stop, 1);
        if ((stopping || frame.actor.event == ACTOR_FAULT) && !dummy.native.entity) InterlockedExchange(&dummy_done, 1);
    }
    enqueue(&frame);
}
enum DiagTickResult diag_tick_install(void* volatile* slot, void* expected, uintptr_t image_base) {
    if (InterlockedCompareExchange(&used, 1, 0)) return DIAG_TICK_ALREADY_USED;
    if (!image_base) { InterlockedExchange(&used, 0); return DIAG_TICK_INVALID_SLOT; }
    base = image_base; diag_tick_prepare_observer();
    enum SlotResult result = tick_hook_install(slot, expected, on_tick);
    if (result != SLOT_OK) InterlockedExchange(&used, 0);
    return (enum DiagTickResult)result;
}
enum DiagTickResult diag_tick_stop(void) {
    enum SlotResult result = tick_hook_stop();
    if (result == SLOT_OK) {
        InterlockedExchange(&used, 0); base = 0; dummy_enabled = 0;
        InterlockedExchange(&dummy_stop, 0); InterlockedExchange(&dummy_done, 1);
    }
    return (enum DiagTickResult)result;
}

unsigned int diag_tick_drain(FrameSample* output, unsigned int capacity)
{
    AcquireSRWLockExclusive(&queue_lock);
    unsigned int n = count < capacity ? count : capacity;
    for (unsigned int i = 0; i < n; ++i) output[i] = queue[(head + i) % FRAME_QUEUE_CAPACITY];
    head = (head + n) % FRAME_QUEUE_CAPACITY;
    count -= n;
    ReleaseSRWLockExclusive(&queue_lock);
    return n;
}

TickStats diag_tick_stats(void)
{
    TickStats stats;
    stats.calls = tick_hook_calls();
    stats.captured = InterlockedCompareExchange(&captured, 0, 0);
    stats.dropped = InterlockedCompareExchange(&dropped, 0, 0);
    stats.installed = tick_hook_ready();
    return stats;
}

int diag_tick_enable_dummy(uintptr_t image_base)
{
    ActorEngine engine;
    if (InterlockedCompareExchange(&used, 0, 0) || !actor_engine_bind(image_base, &engine)) return 0;
    dummy_actor_init(&dummy, &engine);
    dummy_enabled = 1;
    InterlockedExchange(&dummy_stop, 0);
    InterlockedExchange(&dummy_done, 0);
    return 1;
}
void diag_tick_request_dummy_stop(void) { InterlockedExchange(&dummy_stop, 1); }
int diag_tick_dummy_stopped(void) { return InterlockedCompareExchange(&dummy_done, 0, 0) != 0; }
