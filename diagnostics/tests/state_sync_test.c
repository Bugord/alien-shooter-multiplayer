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
    const MpState* sa, const MpState* sb) {
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
    MpState original = {0}, decoded;
    mp_world_key("maps\\Level_01.map", &original.world_low, &original.world_high);
    original.world_epoch = 1;
    original.sequence = 0xFFFFFFFFu; original.active = 1; original.tick = 12345;
    original.x = -123.5f; original.y = 567.25f; original.z = 0.125f;
    original.health = -17; original.weapon_slot = 9; original.current_ammo = 999999;
    original.animation = 2; original.direction = 11;
    original.velocity = 0.125f; original.moving = 1;
    original.torso_present = 1; original.torso_direction = 220;
    for (int i = 0; i < 9; ++i) original.stored_ammo[i] = 0xFFFFFFFFu - (uint32_t)i;
    uint8_t packet[MP_STATE_SIZE];
    mp_state_encode(packet, &original);
    CHECK(packet[3] == 3 && packet[4] == 255 && packet[28] == 255);
    CHECK(mp_state_decode(packet, sizeof(packet), &decoded));
    CHECK(!memcmp(&original, &decoded, sizeof(original)));
    CHECK(!mp_state_decode(packet, sizeof(packet) - 1, &decoded));
    packet[3] = 1; CHECK(!mp_state_decode(packet, sizeof(packet), &decoded));
    original.torso_direction = 256; mp_state_encode(packet, &original);
    CHECK(!mp_state_decode(packet, sizeof(packet), &decoded)); original.torso_direction = 220;
    original.torso_present = 2; mp_state_encode(packet, &original);
    CHECK(!mp_state_decode(packet, sizeof(packet), &decoded)); original.torso_present = 1;
    CHECK(!mp_state_decode(packet, 84, &decoded)); /* Old protocol size. */
    original.moving = 2; mp_state_encode(packet, &original);
    CHECK(!mp_state_decode(packet, sizeof(packet), &decoded)); original.moving = 1;
    original.velocity = NAN; mp_state_encode(packet, &original);
    CHECK(!mp_state_decode(packet, sizeof(packet), &decoded));
    original.velocity = INFINITY; mp_state_encode(packet, &original);
    CHECK(!mp_state_decode(packet, sizeof(packet), &decoded)); original.velocity = 0.125f;
    original.direction = 256; mp_state_encode(packet, &original);
    CHECK(!mp_state_decode(packet, sizeof(packet), &decoded)); original.direction = 11;
    original.x = NAN; mp_state_encode(packet, &original);
    CHECK(!mp_state_decode(packet, sizeof(packet), &decoded)); original.x = -123.5f;
    original.weapon_slot = 10; mp_state_encode(packet, &original);
    CHECK(!mp_state_decode(packet, sizeof(packet), &decoded)); original.weapon_slot = 9;
    CHECK(mp_sequence_newer(0, UINT32_MAX) && !mp_sequence_newer(7, 7) && !mp_sequence_newer(6, 7));

    uint32_t low, high;
    CHECK(mp_is_level_path("maps/LEVEL_01.MAP") && mp_is_level_path("maps\\survive_01.map"));
    CHECK(!mp_is_level_path("maps/mainmenu.map") && !mp_is_level_path("maps/../Level_01.map"));
    CHECK(!mp_is_level_path("maps/Level_01.map.extra") && !mp_is_level_path("maps/level_"));
    mp_world_key("MAPS/level_01.map", &low, &high);
    CHECK(low == original.world_low && high == original.world_high);
    MpShot shot = {0}, shot_decoded;
    shot.world_low = low; shot.world_high = high; shot.world_epoch = 1;
    shot.weapon = 2; shot.x = -17; shot.y = 500; shot.sequence = UINT32_MAX;
    uint8_t shot_packet[MP_SHOT_SIZE];
    mp_shot_encode(shot_packet, &shot);
    CHECK(mp_shot_decode(shot_packet, sizeof(shot_packet), &shot_decoded));
    CHECK(!memcmp(&shot, &shot_decoded, sizeof(shot)));
    CHECK(!mp_shot_decode(shot_packet, sizeof(shot_packet)-1, &shot_decoded));
    shot_packet[3] = 2; CHECK(!mp_shot_decode(shot_packet, sizeof(shot_packet), &shot_decoded));
    shot.weapon = 10; mp_shot_encode(shot_packet, &shot);
    CHECK(!mp_shot_decode(shot_packet, sizeof(shot_packet), &shot_decoded)); shot.weapon = 2;
    /* Handshake and roster codecs. */
    uint8_t hs[256]; char text[MP_MAX_NAME_LEN + 1]; char map[MP_MAX_MAP_NAME_LEN]; uint32_t id, capacity;
    int n = mp_hello_encode(hs, sizeof(hs), "Alice");
    CHECK(n == 10 && mp_hello_decode(hs, n, text) && !strcmp(text, "Alice"));
    CHECK(!mp_hello_decode(hs, n - 1, text) && !mp_hello_decode(hs, n + 1, text));
    hs[3] = 9; CHECK(!mp_hello_decode(hs, n, text)); hs[3] = 1;
    hs[4] = 0; CHECK(!mp_hello_decode(hs, 5, text)); /* Empty name. */
    CHECK(!mp_hello_encode(hs, sizeof(hs), "") && !mp_hello_encode(hs, sizeof(hs), "SixteenCharsName") && !mp_hello_encode(hs, sizeof(hs), "bad"));
    n = mp_welcome_encode(hs, sizeof(hs), 3, 4, "maps/Level_01.map");
    CHECK(n && mp_welcome_decode(hs, n, &id, &capacity, map) && id == 3 && capacity == 4 && !strcmp(map, "maps/Level_01.map"));
    CHECK(!mp_welcome_decode(hs, n - 1, &id, &capacity, map));
    CHECK(!mp_welcome_encode(hs, sizeof(hs), 4, 4, "maps/Level_01.map") || !mp_welcome_decode(hs, 13 + 17, &id, &capacity, map));
    CHECK(!mp_welcome_encode(hs, sizeof(hs), 0, 4, "maps/mainmenu.map")); /* Not a level. */
    MpRosterEntry roster[MP_ROSTER_MAX_ENTRIES], back[MP_ROSTER_MAX_ENTRIES]; unsigned int count;
    memset(roster, 0, sizeof(roster));
    roster[0].id = 0; roster[0].session = 5; strcpy_s(roster[0].name, sizeof(roster[0].name), "One");
    roster[1].id = 7; roster[1].session = 1; strcpy_s(roster[1].name, sizeof(roster[1].name), "Seven");
    n = mp_roster_encode(hs, sizeof(hs), roster, 2);
    CHECK(n && mp_roster_decode(hs, n, back, &count) && count == 2 && back[1].id == 7 && !strcmp(back[1].name, "Seven"));
    CHECK(!mp_roster_decode(hs, n - 1, back, &count) && !mp_roster_decode(hs, n + 1, back, &count));
    roster[1].id = 0; CHECK(!mp_roster_encode(hs, sizeof(hs), roster, 2) || (n = mp_roster_encode(hs, sizeof(hs), roster, 2), !mp_roster_decode(hs, n, back, &count))); /* Duplicate id. */
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
    PeerState named;
    CHECK(state_client_peer_info(b, 0, GetTickCount(), &named) && !strcmp(named.name, "First"));
    CHECK(state_client_peer_info(a, 1, GetTickCount(), &named) && !strcmp(named.name, "Second"));
    CHECK(state_client_map(a) && !strcmp(state_client_map(a), "maps\\Level_01.map"));
    ShotEvent event;
    CHECK(!state_client_send_shot(a, &shot, GetTickCount())); /* Dead owner. */
    MpState firing = original; firing.health = 110;
    DWORD shot_at = GetTickCount(); state_client_publish(a, &firing, shot_at);
    CHECK(!state_client_send_shot(a, &shot, shot_at + 251)); /* Stalled sampling. */
    ++shot.world_epoch;
    CHECK(!state_client_send_shot(a, &shot, shot_at)); /* Same map reloaded. */
    --shot.world_epoch; ++shot.world_low;
    CHECK(!state_client_send_shot(a, &shot, shot_at)); /* Different map. */
    --shot.world_low;
    firing.active = 0; state_client_publish(a, &firing, shot_at);
    CHECK(!state_client_send_shot(a, &shot, shot_at)); /* Shop/menu. */
    firing.active = 1; state_client_publish(a, &firing, shot_at);
    CHECK(state_client_send_shot(a, &shot, shot_at));
    pump(server, a, b, 100, &firing, &original);
    CHECK(state_client_take_shot(b, &event) && event.id == 0 && event.session && event.shot.x == -17 && event.shot.weapon == 2);
    CHECK(!state_client_take_shot(b, &event) && !state_client_take_shot(a, &event));
    epnet_client_t* raw = epnet_client_create(); CHECK(raw);
    CHECK(!epnet_client_connect(raw, "127.0.0.1", port));
    raw_pump(server, a, b, raw);
    CHECK(epnet_client_get_state(raw) == EPNET_STATE_CONNECTED);
    original.sequence = UINT32_MAX;
    mp_state_encode(packet, &original);
    epnet_client_send(raw, MPT_C_STATE, packet, sizeof(packet));
    raw_pump(server, a, b, raw);
    CHECK(!state_client_peer(b, 2, GetTickCount(), &decoded)); /* Not registered yet. */
    uint8_t bad_request[] = {0, 0, 0, 1, 15, 'X'};
    epnet_client_send(raw, MPT_C_HELLO, bad_request, sizeof(bad_request));
    raw_pump(server, a, b, raw);
    epnet_client_send(raw, MPT_C_STATE, packet, sizeof(packet));
    raw_pump(server, a, b, raw);
    CHECK(!state_client_peer(b, 2, GetTickCount(), &decoded));
    uint8_t request[32]; int request_length = mp_hello_encode(request, sizeof(request), "Raw");
    epnet_client_send(raw, MPT_C_HELLO, request, (size_t)request_length);
    raw_pump(server, a, b, raw);
    epnet_client_send(raw, MPT_C_STATE, packet, sizeof(packet) - 1);
    raw_pump(server, a, b, raw);
    CHECK(!state_client_peer(b, 2, GetTickCount(), &decoded));
    epnet_client_send(raw, MPT_C_STATE, packet, sizeof(packet));
    raw_pump(server, a, b, raw);
    CHECK(state_client_peer(b, 2, GetTickCount(), &decoded) && decoded.sequence == UINT32_MAX);
    original.sequence = 0; original.health = 222;
    mp_state_encode(packet, &original);
    epnet_client_send(raw, MPT_C_STATE, packet, sizeof(packet));
    raw_pump(server, a, b, raw);
    CHECK(state_client_peer(b, 2, GetTickCount(), &decoded) && decoded.sequence == 0 && decoded.health == 222);
    original.health = 444; mp_state_encode(packet, &original); /* Same sequence must be ignored. */
    epnet_client_send(raw, MPT_C_STATE, packet, sizeof(packet));
    original.sequence = UINT32_MAX; mp_state_encode(packet, &original);
    epnet_client_send(raw, MPT_C_STATE, packet, sizeof(packet));
    raw_pump(server, a, b, raw);
    CHECK(state_client_peer(b, 2, GetTickCount(), &decoded) && decoded.health == 222);
    shot.sequence = UINT32_MAX; mp_shot_encode(shot_packet, &shot);
    epnet_client_send(raw, MPT_C_SHOT, shot_packet, sizeof(shot_packet)); raw_pump(server, a, b, raw);
    CHECK(state_client_take_shot(b, &event) && event.id == 2 && event.shot.sequence == UINT32_MAX);
    epnet_client_send(raw, MPT_C_SHOT, shot_packet, sizeof(shot_packet)); raw_pump(server, a, b, raw);
    CHECK(!state_client_take_shot(b, &event));
    shot.sequence = 0; mp_shot_encode(shot_packet, &shot);
    epnet_client_send(raw, MPT_C_SHOT, shot_packet, sizeof(shot_packet)); raw_pump(server, a, b, raw);
    CHECK(state_client_take_shot(b, &event) && event.shot.sequence == 0);
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
    CHECK(state_client_peer_info(b, 0, GetTickCount(), &named) && !strcmp(named.name, "Rejoined")); /* Replaced, not stale. */
    state_client_destroy(a); state_client_destroy(b); mp_server_destroy(server);
    puts("State sync checks passed: signed values, codec validation, two clients through UDP server, menu/stall, expiry, reconnect.");
    return 0;
}
