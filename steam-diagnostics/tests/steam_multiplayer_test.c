#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../asmp-dll/src/multiplayer/steam/steam_multiplayer.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned char game[0x2300], army[0x30], player[0xBC], vid[0x490];
static unsigned char entity[0xBC], torso[0x74], weapons[10][8];
static uintptr_t entries[16], base;
static unsigned int created, destroyed, shots, selected;
static int shot_x, shot_y, health, armed, fail_weapon;
static SteamPeerState peers[STEAM_MAX_PEERS];
static Snapshot local;
static void* __fastcall create(void* g, void* u, void* v, float x, float y, float z, int dir, void* parent) {
    (void)u; (void)parent; CHECK(g == game);
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
    (void)u; (void)c; CHECK(e == entity);
    if (kind == 0x61) { CHECK(a == 1); return 0; }
    if (kind == 0x38) return 1;
    if (kind == 0x5C) return *(int*)(entity + 0x84) / 64;
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
    (void)u; CHECK(e == entity); health = value;
}
static SteamMultiplayerFrame step(DWORD now) {
    steam_multiplayer_publish(peers, now);
    return steam_multiplayer_tick(&local, PROBE_OK, now);
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
    mp_steam_world_key("maps\\Level_01.map", &local.world_low, &local.world_high);
    MpSteamState* s = &peers[0].state;
    peers[0].present = 1; peers[0].session = 1;
    s->world_low = local.world_low; s->world_high = local.world_high; s->world_epoch = 7;
    s->active = 1; s->health = 301; s->current_ammo = 73; s->weapon_slot = 2;
    s->x = 555; s->y = 444; s->velocity = .125f; s->moving = 1;
    s->direction = 168; s->torso_present = 1; s->torso_direction = 220;
    ActorEngine api = { create, destroy, move, rotate, action, weapon, set_health };
    CHECK(steam_multiplayer_initialize(base, &api));
    CHECK(step(100).world_epoch == 1); step(120); step(2120); CHECK(created == 1);
    step(2140); CHECK(selected == 0); /* Native torso has not spawned yet. */
    attach_torso(); step(2160);
    CHECK(armed == 2 && health == 301 && *(int*)(entity + 0x84) == 73 * 64);
    CHECK(*(float*)(entity + 0x30) == 555 && entity[0x50] == 168 && torso[0x50] == 220);
    CHECK(steam_multiplayer_is_replica((uintptr_t)entity) && !steam_multiplayer_is_replica((uintptr_t)player));
    SteamShotEvent event = {0}; event.id = 0; event.session = 1;
    event.shot.world_low = s->world_low; event.shot.world_high = s->world_high;
    event.shot.world_epoch = 7; event.shot.weapon = 3; event.shot.x = -17; event.shot.y = 901;
    steam_multiplayer_receive_shot(&event, 2161);
    CHECK(step(2180).shots_applied == 1 && shots == 1 && armed == 3 && shot_x == -17 && shot_y == 901);
    event.session = 2; steam_multiplayer_receive_shot(&event, 2181);
    CHECK(step(2200).shots_discarded == 1 && shots == 1);
    event.session = 1; ++event.shot.world_epoch; steam_multiplayer_receive_shot(&event, 2201);
    CHECK(step(2220).shots_discarded == 1 && shots == 1);
    --event.shot.world_epoch; s->current_ammo = 1;
    steam_multiplayer_receive_shot(&event, 2221);
    CHECK(step(2230).shots_applied == 1 && shots == 2); /* Last two-unit attack. */
    steam_multiplayer_capture_shot(200, 300, 1);
    MpSteamShot capture; CHECK(steam_multiplayer_take_local_shot(&capture) && capture.world_epoch == 1 && capture.x == 200);
    steam_multiplayer_capture_shot(210, 310, 1);
    local.world_low = local.world_high = 0;
    step(2240); CHECK(destroyed == 1 && !steam_multiplayer_take_local_shot(&capture));
    mp_steam_world_key("maps\\Level_01.map", &local.world_low, &local.world_high);
    CHECK(step(2260).world_epoch == 2); step(2280); step(4260); attach_torso(); step(4280);
    CHECK(created == 2 && health == 301);
    steam_multiplayer_tick(&local, PROBE_OK, 5400); CHECK(destroyed == 2); /* Expired worker data. */
    step(5420); step(5440); step(7440); attach_torso(); step(7460); CHECK(created == 3);
    s->weapon_slot = 5; fail_weapon = 1; step(7480); step(7500);
    CHECK(destroyed == 3); /* Ordinary rejection must still release its entity. */
    steam_multiplayer_request_stop(); step(7520); CHECK(steam_multiplayer_stopped());
    VirtualFree((void*)base, 0, MEM_RELEASE);
    puts("Steam multiplayer checks passed: native torso wait, combat, shots, generation/world checks, stale captures, expiry, rejected-actor cleanup.");
    return 0;
}
