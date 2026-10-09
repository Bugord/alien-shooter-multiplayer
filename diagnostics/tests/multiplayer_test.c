#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../asmp-dll/src/multiplayer/multiplayer.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned char game[0x2300], army[0x30], player[0xBC], vid[0x490];
static unsigned char entity[0xBC], torso[0x74], weapons[10][8];
static uintptr_t entries[16], base;
static unsigned int created, destroyed, shots, selected;
static int shot_x, shot_y, health, armed, fail_weapon, fail_factory, fault_apply, fault_remove, missing_weapon, fault_shot;
static unsigned int granted, local_granted;
static PeerState peers[MP_MAX_PEERS];
static Snapshot local;
static void* __fastcall create(void* g, void* u, void* v, float x, float y, float z, int dir, void* parent) {
    (void)u; (void)parent; CHECK(g == game);
    if (fail_factory) return NULL;
    memset(entity, 0, sizeof(entity));
    *(uintptr_t*)entity = base + STEAM_MAN_VTABLE_RVA;
    *(uintptr_t*)(entity + 0x1C) = (uintptr_t)v;
    *(float*)(entity + 0x30) = x; *(float*)(entity + 0x34) = y; *(float*)(entity + 0x38) = z;
    entity[0x50] = (unsigned char)dir;
    entries[1] = (uintptr_t)entity; *(unsigned int*)(game + 0x60) = 2; ++created;
    return entity;
}
static void* __fastcall destroy(void* e, void* u, int release) {
    (void)u; CHECK(e == entity && release == 1 && entries[1] == (uintptr_t)e);
    if (fault_remove) RaiseException(0xE0000001u, 0, 0, NULL);
    entries[1] = 0; ++destroyed; return e;
}
static void __fastcall move(void* e, void* u, float x, float y, float z) {
    (void)u; CHECK(e == entity);
    *(float*)(entity + 0x30) = x; *(float*)(entity + 0x34) = y; *(float*)(entity + 0x38) = z;
}
static unsigned char __fastcall rotate(void* e, void* u, unsigned int dir) {
    (void)u; CHECK(e == entity || e == torso); ((unsigned char*)e)[0x50] = (unsigned char)dir; return 1;
}
static int __fastcall action(void* e, void* u, unsigned int kind, intptr_t a, intptr_t b, intptr_t c) {
    (void)u; (void)c;
    if (e == player) { /* The local player: armory grants only. */
        if (kind == 0x38) return 0;
        CHECK(kind == 0x36 && a >= 260 && a < 270); ++local_granted; return 0;
    }
    CHECK(e == entity);
    if (kind == 0x61) { CHECK(a == 0); return 0; }
    if (kind == 0x38) return !missing_weapon;
    if (kind == 0x36) { CHECK(a >= 260 && a < 270); ++granted; return 0; }
    if (kind == 0x5C) { if (fault_shot) RaiseException(0xE0000001u, 0, 0, NULL); return *(int*)(entity + 0x84) / 64; }
    if (kind == 0x5D) { *(int*)(entity + 0x84) += (int)a * 64; return 0; }
    if (kind == 0x25) { CHECK(*(int*)(entity + 0x84) >= 2 * 64); ++shots; shot_x = (int)a; shot_y = (int)b; return 0; }
    CHECK(0); return 0;
}
static int __fastcall weapon(void* e, void* u, int slot) {
    (void)u; CHECK(e == entity && slot >= 0 && slot < 10);
    if (fail_weapon) return 0;
    uintptr_t v = *(uintptr_t*)(entity + 0x1C);
    *(uintptr_t*)(v + 0x5C) = (uintptr_t)weapons[slot];
    *(uintptr_t*)(torso + 0x1C) = (uintptr_t)weapons[slot];
    armed = slot; ++selected; return 1;
}
static void __fastcall set_health(void* e, void* u, int value) {
    (void)u; CHECK(e == entity);
    if (fault_apply) RaiseException(0xE0000001u, 0, 0, NULL);
    health = value;
}
static MultiplayerFrame step(DWORD now) {
    multiplayer_publish(peers, now);
    return multiplayer_tick(&local, PROBE_OK, now);
}
static void attach_torso(void) {
    *(uintptr_t*)(entity + 0x40) = (uintptr_t)torso;
    uintptr_t v = *(uintptr_t*)(entity + 0x1C);
    *(uintptr_t*)(torso + 0x1C) = *(uintptr_t*)(v + 0x5C);
}
int main(void) {
    base = (uintptr_t)VirtualAlloc(NULL, 0x276000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); CHECK(base);
    *(uintptr_t*)(base + STEAM_GAME_PTR_RVA) = (uintptr_t)game;
    *(uintptr_t*)game = base + STEAM_GAME_VTABLE_RVA;
    *(const char**)(game + 0x20) = "maps\\Level_01.map";
    *(unsigned int*)(game + 0x60) = 1; *(unsigned int*)(game + 0x64) = 16;
    *(uintptr_t**)(game + 0x68) = entries; entries[0] = (uintptr_t)player;
    *(uintptr_t*)(game + STEAM_ARMY_ARRAY_OFFSET) = (uintptr_t)army;
    *(uintptr_t*)(army + STEAM_ARMY_PLAYER_OFFSET) = (uintptr_t)player;
    *(uintptr_t*)player = base + STEAM_MAN_VTABLE_RVA;
    *(uintptr_t*)(player + 0x1C) = (uintptr_t)vid;
    *(uintptr_t*)(player + 0x40) = (uintptr_t)torso;
    *(int*)(player + 0x58) = 110;
    *(unsigned int*)(vid + 0x10) = 7;
    for (unsigned int i = 0; i < 10; ++i) *(unsigned int*)(&weapons[i][4]) = 10 + i;
    *(uintptr_t*)(vid + 0x5C) = (uintptr_t)weapons[1];
    local.game = (uintptr_t)game; local.player = (uintptr_t)player; local.army = (uintptr_t)army; local.health = 110;
    mp_world_key("maps\\Level_01.map", &local.world_low, &local.world_high);
    local.in_level = 1;
    MpState* s = &peers[0].state;
    peers[0].present = 1; peers[0].session = 1;
    s->world_low = local.world_low; s->world_high = local.world_high; s->world_epoch = 7;
    s->active = 1; s->health = 301; s->current_ammo = 73; s->weapon_slot = 2;
    s->x = 555; s->y = 444; s->velocity = .125f; s->moving = 1;
    s->direction = 168; s->torso_present = 1; s->torso_direction = 220;
    ActorEngine api = { create, destroy, move, rotate, action, weapon, set_health };
    CHECK(multiplayer_initialize(base, &api));
    CHECK(step(100).world_epoch == 1); CHECK(local_granted == 10); /* All weapon slots, once. */
    step(120); step(2120); CHECK(created == 1);
    step(2140); CHECK(selected == 0); /* Native torso has not spawned yet. */
    attach_torso(); step(2160);
    CHECK(armed == 2 && health == 301 && *(int*)(entity + 0x84) == 73 * 64);
    CHECK(*(float*)(entity + 0x30) == 555 && entity[0x50] == 168 && torso[0x50] == 220);
    CHECK(multiplayer_is_replica((uintptr_t)entity) && !multiplayer_is_replica((uintptr_t)player));
    ShotEvent event = {0}; event.id = 0; event.session = 1;
    event.shot.world_low = s->world_low; event.shot.world_high = s->world_high;
    event.shot.world_epoch = 7; event.shot.weapon = 3; event.shot.x = -17; event.shot.y = 901;
    multiplayer_receive_shot(&event, 2161);
    CHECK(step(2180).shots_applied == 1 && shots == 1 && armed == 3 && shot_x == -17 && shot_y == 901);
    event.session = 2; multiplayer_receive_shot(&event, 2181);
    CHECK(step(2200).shots_discarded == 1 && shots == 1);
    event.session = 1; ++event.shot.world_epoch; multiplayer_receive_shot(&event, 2201);
    CHECK(step(2220).shots_discarded == 1 && shots == 1);
    --event.shot.world_epoch; s->current_ammo = 1;
    multiplayer_receive_shot(&event, 2221);
    CHECK(step(2230).shots_applied == 1 && shots == 2); /* Last two-unit attack. */
    /* A busy torso would ignore native 0x25: the event waits instead of being consumed. */
    *(unsigned int*)(torso + 0x54) = 5001; multiplayer_receive_shot(&event, 2231);
    MultiplayerFrame waiting = step(2231);
    CHECK(waiting.shots_applied == 0 && waiting.shots_discarded == 0 && shots == 2);
    *(unsigned int*)(torso + 0x54) = 5000; *(unsigned int*)(torso + 0x4C) = 8;
    *(int*)(torso + 0x0C) = 3; *(int*)(torso + 0x10) = 3;
    CHECK(step(2232).shots_applied == 0 && shots == 2); /* Attack animation still playing. */
    *(int*)(torso + 0x0C) = 4;
    CHECK(step(2233).shots_applied == 1 && shots == 3);
    *(unsigned int*)(torso + 0x4C) = 0;
    /* Interpolation: a small move is played back behind the newest sample, a large one snaps. */
    s->sequence = 1; s->tick = 40; s->x = 575; step(2234);
    CHECK(*(float*)(entity + 0x30) >= 555.0f && *(float*)(entity + 0x30) < 575.0f);
    s->sequence = 2; s->tick = 48; s->x = 2000; step(2236); CHECK(*(float*)(entity + 0x30) == 2000);
    s->sequence = 3; s->tick = 56; s->x = 555; step(2237); CHECK(*(float*)(entity + 0x30) == 555);
    /* Captures wait in game-thread staging until a tick moves them to the worker queue. */
    MpShot capture; CHECK(!multiplayer_take_local_shot(&capture));
    multiplayer_capture_shot(200, 300, 1); CHECK(!multiplayer_take_local_shot(&capture));
    step(2238); CHECK(multiplayer_take_local_shot(&capture) && capture.world_epoch == 1 && capture.x == 200);
    for (int i = 0; i < 65; ++i) multiplayer_capture_shot(i, i, 1);
    CHECK(step(2239).shots_discarded == 1); /* 64 staged, one counted drop. */
    unsigned int taken = 0; while (multiplayer_take_local_shot(&capture)) ++taken;
    CHECK(taken == 64);
    multiplayer_capture_shot(210, 310, 1);
    local.in_level = 0;
    local.world_low = local.world_high = 0;
    step(2240); CHECK(destroyed == 1 && !multiplayer_take_local_shot(&capture));
    mp_world_key("maps\\Level_01.map", &local.world_low, &local.world_high);
    local.in_level = 1;
    CHECK(step(2260).world_epoch == 2); step(2280); step(4260); attach_torso(); step(4280);
    CHECK(created == 2 && health == 301);
    local.health = 0; CHECK(step(4300).world_epoch == 2 && destroyed == 1);
    CHECK(step(4320).world_epoch == 2 && destroyed == 1);
    CHECK(multiplayer_tick(&local, PROBE_NO_PLAYER, 4330).world_epoch == 2 && destroyed == 1);
    local.health = 110; CHECK(step(4350).world_epoch == 2 && destroyed == 1);
    CHECK(local_granted == 30); /* New world, then respawn with a new grant. */
    multiplayer_tick(&local, PROBE_OK, 5400); CHECK(destroyed == 2); /* Expired worker data. */
    step(5420); step(5440); step(7440); attach_torso(); step(7460); CHECK(created == 3);
    s->weapon_slot = 5; fail_weapon = 1;
    MultiplayerFrame rejected = step(7480);
    CHECK(rejected.count == 1 && rejected.remote[0].actor.event == ACTOR_REJECTED &&
        rejected.remote[0].actor.reason == ACTOR_REASON_WEAPON);
    CHECK(step(7500).count == 0); /* Reported once per slot. */
    CHECK(destroyed == 2 && multiplayer_remote_state(0) == RS_SPAWNED && armed == 2);
    fail_weapon = 0; step(7510); CHECK(armed == 2); /* Retry waits one second. */
    step(8480); CHECK(armed == 5); /* A rejected arm is retried. */
    s->weapon_slot = 6; step(8490); CHECK(armed == 6);
    fault_shot = 1; multiplayer_receive_shot(&event, 8495);
    MultiplayerFrame shot_fault = step(8500); fault_shot = 0;
    CHECK(shot_fault.shots_discarded == 1 && shot_fault.count == 1 && shot_fault.remote[0].id == 0 &&
        shot_fault.remote[0].actor.event == ACTOR_FAULT && shot_fault.remote[0].state == RS_ABANDONED &&
        !shot_fault.remote[0].cleanup_failed && destroyed == 3);
    multiplayer_request_stop(); step(8520); CHECK(multiplayer_stopped());
    CHECK(multiplayer_stop());
    /* Reinitialization clears old queues, stop flags and world identity. */
    CHECK(multiplayer_initialize(base, &api));
    fail_factory = 1; step(10000); step(12000);
    CHECK(multiplayer_remote_state(0) == RS_BACKOFF && created == 3);
    step(13999); CHECK(created == 3);
    fail_factory = 0; step(14000); CHECK(created == 4);
    step(16999); CHECK(destroyed == 3);
    step(17000); CHECK(destroyed == 4 && multiplayer_remote_state(0) == RS_BACKOFF);
    step(21000); attach_torso(); missing_weapon = 1; step(21020);
    CHECK(created == 5 && granted == 1 && multiplayer_remote_state(0) == RS_SPAWNED);
    missing_weapon = 0; fault_apply = 1; step(21040); fault_apply = 0;
    CHECK(destroyed == 5 && multiplayer_remote_state(0) == RS_ABANDONED);
    step(22000); CHECK(created == 5); /* No writes in the faulting world. */
    mp_world_key("maps/Level_02.map", &local.world_low, &local.world_high);
    s->world_low = local.world_low; s->world_high = local.world_high;
    step(22020); step(24020); attach_torso(); step(24040);
    CHECK(created == 6 && multiplayer_remote_state(0) == RS_SPAWNED); /* Same GAME, different key. */
    fault_apply = fault_remove = 1;
    MultiplayerFrame fault = step(24060);
    CHECK(fault.count == 1 && fault.remote[0].cleanup_failed && multiplayer_remote_state(0) == RS_ABANDONED);
    fault_apply = 0; multiplayer_request_stop(); fault = step(24080);
    CHECK(!multiplayer_stopped() && !multiplayer_stop() && fault.remote[0].cleanup_failed);
    entries[1] = 0; fault_remove = 0; /* Native map unload owns remaining storage. */
    step(24100); CHECK(multiplayer_stopped() && multiplayer_stop());
    VirtualFree((void*)base, 0, MEM_RELEASE);
    puts("Multiplayer checks passed: native torso wait, combat, shots, generation/world checks, stale captures, expiry, rejected-actor cleanup.");
    return 0;
}
