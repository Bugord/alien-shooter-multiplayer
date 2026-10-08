#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/diag_tick.h"

static int failures, original_calls, original_return;
static unsigned char* player;
static void* expected_game;
static int __fastcall fake_tick(void* game, void* unused)
{
    (void)unused;
    if (game != expected_game) ++failures;
    ++original_calls;
    *(float*)(player + STEAM_ENTITY_X_OFFSET) += 1.0f;
    --*(int32_t*)(player + STEAM_ENTITY_HEALTH_OFFSET);
    return original_return;
}
static int __fastcall foreign_tick(void* game, void* unused)
{
    (void)game; (void)unused;
    return 42;
}
typedef int (__fastcall* TickFn)(void*, void*);
static TickFn stress_tick;
static DWORD WINAPI produce_frames(LPVOID unused)
{
    (void)unused;
    for (unsigned int i = 0; i < 20000; ++i) stress_tick(expected_game, NULL);
    return 0;
}
static void check(int condition, const char* label)
{
    if (!condition) { fprintf(stderr, "FAIL %s\n", label); ++failures; }
}
static DWORD protection(void* address)
{
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(address, &info, sizeof(info))) return 0;
    return info.Protect;
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    unsigned char* image = VirtualAlloc(NULL, 0x276000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    unsigned char* game = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x22C8);
    unsigned char* army = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x100);
    player = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0xBC);
    void* volatile* slot = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!image || !game || !army || !player || !slot) return 2;
    uintptr_t base = (uintptr_t)image;
    expected_game = game;
    *(uintptr_t*)(image + STEAM_GAME_PTR_RVA) = (uintptr_t)game;
    *(uintptr_t*)game = base + STEAM_GAME_VTABLE_RVA;
    *(uintptr_t*)(game + STEAM_ARMY_ARRAY_OFFSET) = (uintptr_t)army;
    *(uintptr_t*)(army + STEAM_ARMY_PLAYER_OFFSET) = (uintptr_t)player;
    *(uintptr_t*)player = base + STEAM_MAN_VTABLE_RVA;
    *(int32_t*)(player + STEAM_ENTITY_HEALTH_OFFSET) = 110;
    union { int (__fastcall* fast)(void*, void*); void* pointer; TickFn tick; } original, foreign, hook;
    original.fast = fake_tick;
    foreign.fast = foreign_tick;
    *slot = original.pointer;
    DWORD ignored;
    check(VirtualProtect((void*)slot, sizeof(void*), PAGE_READONLY, &ignored), "protect test vtable");
    if (!strcmp(argv[1], "mismatch")) {
        check(diag_tick_install(slot, foreign.pointer, base) == DIAG_TICK_SLOT_CHANGED, "reject unexpected original");
        check(*slot == original.pointer && protection((void*)slot) == PAGE_READONLY, "preserve mismatched slot and protection");
    } else if (!strcmp(argv[1], "invalid")) {
        check(diag_tick_install((void* volatile*)((uintptr_t)slot + 1), original.pointer, base) ==
            DIAG_TICK_INVALID_SLOT, "reject unaligned slot");
        check(*slot == original.pointer, "no write on invalid slot");
    } else {
        check(diag_tick_install(slot, original.pointer, base) == DIAG_TICK_OK, "install");
        check(protection((void*)slot) == PAGE_READONLY, "restore protection after install");
        hook.pointer = *slot;
        check(hook.pointer != original.pointer, "publish hook pointer");
        check(diag_tick_install(slot, original.pointer, base) == DIAG_TICK_ALREADY_USED, "reject reinstall");
        check(hook.tick(game, (void*)123) == 0 && original_calls == 1, "ECX and original invocation");
        FrameSample frame;
        check(diag_tick_drain(&frame, 1) == 1, "one frame captured");
        check(frame.result == PROBE_OK && frame.snapshot.x == 1.0f && frame.snapshot.health == 109 &&
            frame.tick == 1, "capture after original update");
        original_return = 7;
        check(hook.tick(game, NULL) == 7 && original_calls == 2, "preserve termination return");
        check(diag_tick_drain(&frame, 1) == 0, "no gameplay read on loop termination");
        original_return = 0;
        for (unsigned int i = 0; i < FRAME_QUEUE_CAPACITY + 3; ++i) hook.tick(game, NULL);
        TickStats stats = diag_tick_stats();
        check(stats.captured == 1 + FRAME_QUEUE_CAPACITY && stats.dropped == 3, "bounded queue overflow counted");
        LONG previous_tick = 2;
        for (unsigned int i = 0; i < FRAME_QUEUE_CAPACITY; ++i) {
            check(diag_tick_drain(&frame, 1) == 1 && frame.tick == previous_tick + 1, "FIFO frame order");
            previous_tick = frame.tick;
        }
        check(diag_tick_drain(&frame, 1) == 0, "queue drained");
        if (!strcmp(argv[1], "concurrent")) {
            LONG captured_before = diag_tick_stats().captured;
            LONG consumed = 0;
            stress_tick = hook.tick;
            HANDLE producer = CreateThread(NULL, 0, produce_frames, NULL, 0, NULL);
            if (!producer) return 2;
            DWORD started = GetTickCount();
            do {
                FrameSample batch[64];
                unsigned int n = diag_tick_drain(batch, 64);
                for (unsigned int i = 0; i < n; ++i) {
                    check(batch[i].tick > previous_tick && batch[i].result == PROBE_OK &&
                        batch[i].snapshot.x == (float)batch[i].tick, "concurrent FIFO and complete snapshot");
                    previous_tick = batch[i].tick;
                    ++consumed;
                }
                if (GetTickCount() - started > 10000) return 2;
            } while (WaitForSingleObject(producer, 0) != WAIT_OBJECT_0);
            CloseHandle(producer);
            while (diag_tick_drain(&frame, 1)) {
                check(frame.tick > previous_tick && frame.snapshot.x == (float)frame.tick,
                    "remaining concurrent frames");
                previous_tick = frame.tick;
                ++consumed;
            }
            stats = diag_tick_stats();
            check(consumed > 0 && consumed == stats.captured - captured_before, "all accepted concurrent frames delivered");
            check(stats.captured + stats.dropped == stats.calls - 1, "capture and drop accounting");
        }
        if (!strcmp(argv[1], "foreign")) {
            check(VirtualProtect((void*)slot, sizeof(void*), PAGE_READWRITE, &ignored), "simulate another hook");
            InterlockedExchangePointer(slot, foreign.pointer);
            check(VirtualProtect((void*)slot, sizeof(void*), PAGE_READONLY, &ignored), "protect foreign slot");
            check(diag_tick_stop() == DIAG_TICK_SLOT_CHANGED && *slot == foreign.pointer, "stop preserves another hook");
        } else {
            check(diag_tick_stop() == DIAG_TICK_OK && *slot == original.pointer, "restore original");
            check(diag_tick_stop() == DIAG_TICK_OK, "idempotent stop");
            check(!diag_tick_stats().installed, "uninstalled status");
        }
        check(protection((void*)slot) == PAGE_READONLY, "restore protection after stop");
        check(hook.tick(game, NULL) == 0, "previously fetched callback remains callable after stop");
        check(diag_tick_drain(&frame, 1) == 0, "capture disabled after stop");
    }
    if (strcmp(argv[1], "foreign")) {
        check(diag_tick_install(slot, original.pointer, base) == DIAG_TICK_OK, "retry after failed install or reinstall after stop");
        hook.pointer = *slot; hook.tick(game, NULL);
        check(diag_tick_stats().captured == 1, "reinstall clears old queue/counters");
        check(diag_tick_stop() == DIAG_TICK_OK, "reinstalled stop");
    }
    VirtualFree((void*)slot, 0, MEM_RELEASE);
    VirtualFree(image, 0, MEM_RELEASE);
    HeapFree(GetProcessHeap(), 0, game);
    HeapFree(GetProcessHeap(), 0, army);
    HeapFree(GetProcessHeap(), 0, player);
    printf("Tick hook checks (%s): %s\n", argv[1], failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
