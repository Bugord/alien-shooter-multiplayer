#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../src/state_client.h"
#include "../../asmp-server/src/server.h"
#include "epnet_client.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void pump(MpServer* server, StateClient* a, StateClient* b, unsigned long milliseconds,
    const MpSteamState* sa, const MpSteamState* sb) {
    DWORD start = GetTickCount();
    do {
        DWORD now = GetTickCount();
        if (sa) state_client_publish(a, sa, now);
        if (sb) state_client_publish(b, sb, now);
        state_client_update(a, now); state_client_update(b, now);
        mp_server_tick(server); Sleep(1);
    } while (GetTickCount() - start < milliseconds);
}
static void raw_pump(MpServer* server, StateClient* a, StateClient* b, epnet_client_t* raw) {
    DWORD start = GetTickCount();
    do {
        epnet_client_update(raw, 0.001);
        state_client_update(a, GetTickCount()); state_client_update(b, GetTickCount());
        mp_server_tick(server); Sleep(1);
    } while (GetTickCount() - start < 100);
}
int main(void) {
    MpSteamState original = {0}, decoded;
    original.sequence = 0xFFFFFFFFu; original.active = 1; original.tick = 12345;
    original.x = -123.5f; original.y = 567.25f; original.z = 0.125f;
    original.health = -17; original.weapon_slot = 9; original.current_ammo = 999999;
    original.animation = 2; original.direction = 11;
    original.velocity = 0.125f; original.moving = 1;
    original.torso_present = 1; original.torso_direction = 220;
    for (int i = 0; i < 9; ++i) original.stored_ammo[i] = 0xFFFFFFFFu - (uint32_t)i;
    uint8_t packet[MP_STEAM_STATE_SIZE];
    mp_steam_encode(packet, &original);
    CHECK(packet[3] == 2 && packet[4] == 255 && packet[28] == 255);
    CHECK(mp_steam_decode(packet, sizeof(packet), &decoded));
    CHECK(!memcmp(&original, &decoded, sizeof(original)));
    CHECK(!mp_steam_decode(packet, sizeof(packet) - 1, &decoded));
    packet[3] = 1; CHECK(!mp_steam_decode(packet, sizeof(packet), &decoded));
    original.torso_direction = 256; mp_steam_encode(packet, &original);
    CHECK(!mp_steam_decode(packet, sizeof(packet), &decoded)); original.torso_direction = 220;
    original.torso_present = 2; mp_steam_encode(packet, &original);
    CHECK(!mp_steam_decode(packet, sizeof(packet), &decoded)); original.torso_present = 1;
    CHECK(!mp_steam_decode(packet, 84, &decoded)); /* Old protocol size. */
    original.moving = 2; mp_steam_encode(packet, &original);
    CHECK(!mp_steam_decode(packet, sizeof(packet), &decoded)); original.moving = 1;
    original.velocity = NAN; mp_steam_encode(packet, &original);
    CHECK(!mp_steam_decode(packet, sizeof(packet), &decoded));
    original.velocity = INFINITY; mp_steam_encode(packet, &original);
    CHECK(!mp_steam_decode(packet, sizeof(packet), &decoded)); original.velocity = 0.125f;
    original.direction = 256; mp_steam_encode(packet, &original);
    CHECK(!mp_steam_decode(packet, sizeof(packet), &decoded)); original.direction = 11;
    original.x = NAN; mp_steam_encode(packet, &original);
    CHECK(!mp_steam_decode(packet, sizeof(packet), &decoded)); original.x = -123.5f;
    original.weapon_slot = 10; mp_steam_encode(packet, &original);
    CHECK(!mp_steam_decode(packet, sizeof(packet), &decoded)); original.weapon_slot = 9;
    CHECK(mp_steam_newer(0, UINT32_MAX) && !mp_steam_newer(7, 7) && !mp_steam_newer(6, 7));

    unsigned short port = (unsigned short)(55000 + GetCurrentProcessId() % 5000);
    MpServer* server = mp_server_create(port, 4); CHECK(server);
    StateClient* a = state_client_create("127.0.0.1", port, "First", NULL); CHECK(a);
    StateClient* b = state_client_create("127.0.0.1", port, "Second", NULL); CHECK(b);
    pump(server, a, b, 1200, &original, &original);
    CHECK(state_client_connected(a) && state_client_connected(b));
    CHECK(state_client_peer(b, 0, GetTickCount(), &decoded));
    CHECK(decoded.active && decoded.health == -17 && decoded.current_ammo == 999999 && decoded.weapon_slot == 9);
    CHECK(decoded.x == -123.5f && decoded.stored_ammo[8] == original.stored_ammo[8]);
    CHECK(decoded.velocity == 0.125f && decoded.moving && decoded.torso_present && decoded.torso_direction == 220);
    CHECK(state_client_peer(a, 1, GetTickCount(), &decoded));
    CHECK(!state_client_peer(a, 0, GetTickCount(), &decoded));
    epnet_client_t* raw = epnet_client_create(); CHECK(raw);
    CHECK(!epnet_client_connect(raw, "127.0.0.1", port));
    raw_pump(server, a, b, raw);
    CHECK(epnet_client_get_state(raw) == EPNET_STATE_CONNECTED);
    original.sequence = UINT32_MAX;
    mp_steam_encode(packet, &original);
    epnet_client_send(raw, MPT_C_STEAM_STATE, packet, sizeof(packet));
    raw_pump(server, a, b, raw);
    CHECK(!state_client_peer(b, 2, GetTickCount(), &decoded)); /* Not registered yet. */
    uint8_t bad_request[] = {15, 'X'};
    epnet_client_send(raw, 0x10, bad_request, sizeof(bad_request));
    raw_pump(server, a, b, raw);
    epnet_client_send(raw, MPT_C_STEAM_STATE, packet, sizeof(packet));
    raw_pump(server, a, b, raw);
    CHECK(!state_client_peer(b, 2, GetTickCount(), &decoded));
    uint8_t request[] = {3, 'R', 'a', 'w'};
    epnet_client_send(raw, 0x10, request, sizeof(request));
    raw_pump(server, a, b, raw);
    epnet_client_send(raw, MPT_C_STEAM_STATE, packet, sizeof(packet) - 1);
    raw_pump(server, a, b, raw);
    CHECK(!state_client_peer(b, 2, GetTickCount(), &decoded));
    epnet_client_send(raw, MPT_C_STEAM_STATE, packet, sizeof(packet));
    raw_pump(server, a, b, raw);
    CHECK(state_client_peer(b, 2, GetTickCount(), &decoded) && decoded.sequence == UINT32_MAX);
    original.sequence = 0; original.health = 222;
    mp_steam_encode(packet, &original);
    epnet_client_send(raw, MPT_C_STEAM_STATE, packet, sizeof(packet));
    raw_pump(server, a, b, raw);
    CHECK(state_client_peer(b, 2, GetTickCount(), &decoded) && decoded.sequence == 0 && decoded.health == 222);
    original.health = 444; mp_steam_encode(packet, &original); /* Same sequence must be ignored. */
    epnet_client_send(raw, MPT_C_STEAM_STATE, packet, sizeof(packet));
    original.sequence = UINT32_MAX; mp_steam_encode(packet, &original);
    epnet_client_send(raw, MPT_C_STEAM_STATE, packet, sizeof(packet));
    raw_pump(server, a, b, raw);
    CHECK(state_client_peer(b, 2, GetTickCount(), &decoded) && decoded.health == 222);
    epnet_client_destroy(raw);
    original.health = 301; original.current_ammo = 73; original.weapon_slot = 2;
    original.x += 10;
    pump(server, a, b, 150, &original, NULL);
    CHECK(state_client_peer(b, 0, GetTickCount(), &decoded));
    CHECK(decoded.health == 301 && decoded.current_ammo == 73 && decoded.weapon_slot == 2 && decoded.x == original.x);
    /* A stalled hook clears activity; a disconnected peer expires. */
    pump(server, a, b, 400, NULL, NULL);
    CHECK(state_client_peer(b, 0, GetTickCount(), &decoded) && !decoded.active);
    state_client_destroy(a); a = NULL;
    pump(server, a, b, 1100, NULL, NULL);
    CHECK(!state_client_peer(b, 0, GetTickCount(), &decoded));
    /* Reused client slot has a new session, permitting sequence restart. */
    a = state_client_create("127.0.0.1", port, "Rejoined", NULL); CHECK(a);
    pump(server, a, b, 1000, &original, NULL);
    CHECK(state_client_peer(b, 0, GetTickCount(), &decoded) && decoded.active && decoded.health == 301);
    state_client_destroy(a); state_client_destroy(b); mp_server_destroy(server);
    puts("State sync checks passed: signed values, codec validation, two clients through UDP server, menu/stall, expiry, reconnect.");
    return 0;
}
