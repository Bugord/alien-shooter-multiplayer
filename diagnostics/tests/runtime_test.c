#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../asmp-dll/src/multiplayer/runtime.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static uintptr_t base, entries[8];
static unsigned char game[0x2300], army[0x30], player[0xBC], vid[0x490], entity[0xBC], torso[0x90], weapon_vid[8];
static char trace[256]; static volatile LONG trace_count;
static unsigned int fail_stage, display_calls, observed, created, removed, world_generation;
static int display_fail, stop_fail, rollback_live;
static HANDLE live_event, game_worker;
static TickCallback callback;
static RuntimeFrame last;
static PeerState peers[MP_MAX_PEERS];
static void record(char value) { LONG i = InterlockedIncrement(&trace_count) - 1; CHECK(i < 255); trace[i] = value; }
static void* __fastcall create(void* g, void* u, void* v, float x, float y, float z, int dir, void* parent) {
    (void)u; (void)x; (void)y; (void)z; (void)dir; (void)parent; CHECK(g == game);
    memset(entity, 0, sizeof(entity)); *(uintptr_t*)entity = base + STEAM_MAN_VTABLE_RVA;
    *(uintptr_t*)(entity + 0x1C) = (uintptr_t)v; *(uintptr_t*)(entity + 0x40) = (uintptr_t)torso;
    *(uintptr_t*)(torso + 0x1C) = *(uintptr_t*)((unsigned char*)v + 0x5C);
    entries[1] = (uintptr_t)entity; *(unsigned int*)(game + 0x60) = 2; ++created; return entity;
}
static void* __fastcall destroy(void* e, void* u, int release) {
    (void)u; CHECK(e == entity && release == 1); record('R'); entries[1] = 0; ++removed; return e;
}
static void __fastcall move(void* e, void* u, float x, float y, float z) { (void)e; (void)u; (void)x; (void)y; (void)z; }
static unsigned char __fastcall rotate(void* e, void* u, unsigned int d) { (void)u; ((unsigned char*)e)[0x50] = (unsigned char)d; return 1; }
static int __fastcall action(void* e, void* u, unsigned int kind, intptr_t a, intptr_t b, intptr_t c) {
    (void)e; (void)u; (void)a; (void)b; (void)c; return kind == 0x38;
}
static int __fastcall weapon(void* e, void* u, int slot) { (void)e; (void)u; (void)slot; return 1; }
static void __fastcall health(void* e, void* u, int value) { (void)u; *(int*)((unsigned char*)e + 0x58) = value; }
static int bind(uintptr_t image, ActorEngine* out) {
    CHECK(image == base); record('b'); if (fail_stage == 1) return 0;
    ActorEngine api = {create, destroy, move, rotate, action, weapon, health}; *out = api; return 1;
}
static uintptr_t menu_item(uintptr_t g, unsigned int v, unsigned int d) { (void)g; (void)v; (void)d; return 0; }
static int text(uintptr_t e, char* out, unsigned int capacity) { (void)e; (void)out; (void)capacity; return 0; }
static void status(uintptr_t e, int s) { (void)e; (void)s; }
static int load_map(uintptr_t g, const char* map) { (void)g; (void)map; return 0; }
static int enable_session(uintptr_t image) {
    CHECK(image == base); record('s'); if (fail_stage == 2) return 0;
    SessionEngine api = {menu_item, text, status, status, load_map}; return session_initialize(&api);
}
static int action_install(uintptr_t image, ReplicaFilter filter, ShotCallback notify) {
    CHECK(image == base && filter && notify); record('a'); return fail_stage != 3;
}
static int action_stop(void) { record('A'); return !stop_fail; }
static int world_install(uintptr_t image) { CHECK(image == base); record('w'); return fail_stage != 4; }
static int world_stop(void) { record('W'); return 1; }
static uint32_t generation(void) { return world_generation; }
static void observe(const RuntimeFrame* frame) { last = *frame; ++observed; }
static void tick(DWORD now) { callback(game, (LONG)observed + 1, now); }
static DWORD WINAPI rollback_game(LPVOID unused) {
    (void)unused; unsigned int i = 0;
    while (runtime_state() != RT_OFF) {
        DWORD now = 100 + i++ * 2100u;
        multiplayer_publish(peers, now); tick(now);
        if (created) SetEvent(live_event);
        Sleep(1);
    }
    return 0;
}
static enum SlotResult tick_install(void* volatile* slot, void* expected, TickCallback notify) {
    CHECK((uintptr_t)slot == base + STEAM_GAME_VTABLE_RVA + STEAM_GAME_TICK_SLOT * 4u &&
        (uintptr_t)expected == base + STEAM_GAME_TICK_RVA); record('t'); callback = notify;
    if (rollback_live) {
        game_worker = CreateThread(NULL, 0, rollback_game, NULL, 0, NULL); CHECK(game_worker);
        CHECK(WaitForSingleObject(live_event, 3000) == WAIT_OBJECT_0);
    }
    return fail_stage == 5 ? SLOT_CHANGED : SLOT_OK;
}
static enum SlotResult tick_stop(void) { record('T'); return SLOT_OK; }
static enum DisplayResult display_install(uintptr_t image, DrawCallback paint) {
    CHECK(image == base && paint); ++display_calls;
    return display_fail ? DISPLAY_FAILED : display_calls < 3 ? DISPLAY_PENDING : DISPLAY_OK;
}
static int display_stop(void) { record('D'); return 1; }
static RuntimeBindings bindings = {bind, enable_session, action_install, action_stop, world_install, world_stop,
    generation, tick_install, tick_stop, display_install, display_stop};
static RuntimeConfig config;
static void reset(unsigned int failure) {
    CHECK(runtime_state() == RT_OFF); memset(trace, 0, sizeof(trace)); trace_count = 0;
    created = removed = display_calls = observed = world_generation = 0; fail_stage = failure;
    display_fail = stop_fail = rollback_live = 0; memset(peers, 0, sizeof(peers));
    *(unsigned int*)(game + 0x60) = 1; entries[1] = 0;
}
static void stop(void) {
    CHECK(!runtime_stop(0)); /* A captured game thread must acknowledge. */
    tick(100000); CHECK(runtime_stop(100) && runtime_state() == RT_OFF);
}
int main(void) {
    base = (uintptr_t)VirtualAlloc(NULL, 0x276000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); CHECK(base);
    *(uintptr_t*)(base + STEAM_GAME_PTR_RVA) = (uintptr_t)game; *(uintptr_t*)game = base + STEAM_GAME_VTABLE_RVA;
    *(const char**)(game + 0x20) = "maps\\Level_01.map";
    *(uintptr_t*)(game + STEAM_ARMY_ARRAY_OFFSET) = (uintptr_t)army; *(uintptr_t*)(army + STEAM_ARMY_PLAYER_OFFSET) = (uintptr_t)player;
    *(unsigned int*)(game + 0x64) = 8; *(uintptr_t**)(game + 0x68) = entries; entries[0] = (uintptr_t)player;
    *(uintptr_t*)player = base + STEAM_MAN_VTABLE_RVA; *(uintptr_t*)(player + 0x1C) = (uintptr_t)vid;
    *(uintptr_t*)(player + 0x40) = (uintptr_t)torso; *(int*)(player + 0x58) = 110;
    *(unsigned int*)(vid + 0x10) = 7; *(uintptr_t*)(vid + 0x5C) = (uintptr_t)weapon_vid; *(unsigned int*)(weapon_vid + 4) = 11;
    config.observer = observe;
    const char* expected[] = {"", "b", "bs", "bsaA", "bsawAW", "bsawtDAWT"};
    for (unsigned int failure = 1; failure <= 5; ++failure) {
        reset(failure); CHECK(!runtime_start_with_bindings(base, &config, &bindings));
        CHECK(runtime_state() == RT_OFF && !strcmp(trace, expected[failure]));
        CHECK(runtime_stop(0));
    }
    reset(0); CHECK(runtime_start_with_bindings(base, &config, &bindings));
    CHECK(!runtime_start_with_bindings(base, &config, &bindings));
    for (unsigned int i = 1; i <= 30; ++i) tick(i); CHECK(display_calls == 1);
    for (unsigned int i = 31; i <= 60; ++i) tick(i); CHECK(display_calls == 2);
    tick(61); CHECK(display_calls == 3 && last.display == DISPLAY_OK && observed == 61);
    CHECK(last.multiplayer.world_epoch == 1); ++world_generation; tick(62);
    CHECK(last.multiplayer.world_epoch == 2); /* Same GAME, key, player and timestamp. */
    stop(); CHECK(strstr(trace, "DAWT"));
    reset(0); display_fail = 1; CHECK(runtime_start_with_bindings(base, &config, &bindings));
    for (unsigned int i = 1; i <= 100; ++i) tick(i); CHECK(display_calls == 1 && last.display == DISPLAY_FAILED);
    stop();
    reset(0); CHECK(runtime_start_with_bindings(base, &config, &bindings));
    peers[0].present = peers[0].session = 1; peers[0].state.active = 1; peers[0].state.health = 110;
    mp_world_key("maps\\Level_01.map", &peers[0].state.world_low, &peers[0].state.world_high); peers[0].state.world_epoch = 1;
    peers[0].state.weapon_slot = 1;
    multiplayer_publish(peers, 100); tick(100); multiplayer_publish(peers, 2200); tick(2200);
    CHECK(created == 1); LONG before_stop = trace_count;
    CHECK(!runtime_stop(0) && runtime_state() == RT_STOPPING && trace_count == before_stop && !removed);
    tick(2300); CHECK(removed == 1); stop_fail = 1;
    CHECK(!runtime_stop(100) && runtime_state() == RT_STOPPING);
    CHECK(!runtime_start_with_bindings(base, &config, &bindings));
    stop_fail = 0; CHECK(runtime_stop(100)); CHECK(strstr(trace, "RDAWT"));
    reset(5); rollback_live = 1;
    peers[0].present = peers[0].session = 1; peers[0].state.active = 1; peers[0].state.health = 110;
    peers[0].state.weapon_slot = 1; peers[0].state.world_epoch = 1;
    mp_world_key("maps\\Level_01.map", &peers[0].state.world_low, &peers[0].state.world_high);
    live_event = CreateEventW(NULL, TRUE, FALSE, NULL); CHECK(live_event);
    CHECK(!runtime_start_with_bindings(base, &config, &bindings));
    CHECK(WaitForSingleObject(game_worker, 3000) == WAIT_OBJECT_0);
    CHECK(created == 1 && removed == 1 && runtime_state() == RT_OFF && strstr(trace, "RDAWT"));
    CloseHandle(game_worker); CloseHandle(live_event); VirtualFree((void*)base, 0, MEM_RELEASE);
    puts("Runtime checks passed: rollback at every stage, active-entity rollback, display throttling/latch, map generations, paused/foreign stop, cleanup ordering and reinstall.");
    return 0;
}
