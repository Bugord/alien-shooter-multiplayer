#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/state_client.h"
#include "../../asmp-server/src/server.h"
#include "epnet_client.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static MpState player;
static uint32_t raw_session;
static int raw_publish_player;
static uint32_t raw_player_sequence;
static DWORD raw_player_at;
static void pump(MpServer* server, StateClient* a, StateClient* b, StateClient* late,
    epnet_client_t* raw, unsigned long duration) {
    DWORD start = GetTickCount();
    do {
        DWORD now = GetTickCount();
        StateClient* clients[] = {a, b, late};
        for (unsigned int i = 0; i < 3u; ++i) if (clients[i]) {
            state_client_publish(clients[i], &player, now); state_client_update(clients[i], now);
        }
        if (raw) {
            epnet_client_update(raw, 0.001);
            epnet_event_t event;
            while (epnet_client_poll_events(raw, &event)) if (event.type == EPNET_EVENT_PACKET &&
                event.data.packet.pkt_type == MPT_S_ROSTER) {
                MpRosterEntry entries[MP_ROSTER_MAX_ENTRIES]; unsigned int count;
                if (mp_roster_decode(event.data.packet.data, event.data.packet.len, entries, &count))
                    for (unsigned int i = 0; i < count; ++i)
                        if (entries[i].id == epnet_client_get_id(raw)) raw_session = entries[i].session;
            }
            /* A real game host continually publishes player world presence.
               Keep the raw owner equivalent, including when an observer joins
               after its first packet; the relay does not cache player states. */
            if (raw_publish_player && raw_session && now - raw_player_at >= MP_STATE_SEND_MS) {
                MpState raw_player = player; raw_player.sequence = ++raw_player_sequence;
                uint8_t packet[MP_STATE_SIZE]; mp_state_encode(packet, &raw_player);
                epnet_client_send(raw, MPT_C_STATE, packet, sizeof(packet)); raw_player_at = now;
            }
        }
        mp_server_tick(server); Sleep(1);
    } while (GetTickCount() - start < duration);
}
static epnet_client_t* raw_join(MpServer* server, StateClient* a, StateClient* b, unsigned short port) {
    epnet_client_t* raw = epnet_client_create(); CHECK(raw);
    CHECK(!epnet_client_connect(raw, "127.0.0.1", port)); raw_session = 0;
    pump(server, a, b, NULL, raw, 200);
    CHECK(epnet_client_get_state(raw) == EPNET_STATE_CONNECTED);
    uint8_t packet[32]; int length = mp_hello_encode(packet, sizeof(packet), "Raw");
    epnet_client_send(raw, MPT_C_HELLO, packet, (size_t)length);
    pump(server, a, b, NULL, raw, 100); CHECK(raw_session); return raw;
}
static void raw_state(epnet_client_t* raw, uint32_t session, const MpMonsterState* state) {
    uint8_t packet[MP_MONSTER_STATE_CLIENT_SIZE]; mp_put_u32(packet, session);
    mp_monster_state_encode(packet + 4, state);
    epnet_client_send(raw, MPT_C_MONSTER_STATE, packet, sizeof(packet));
}
int main(void) {
    memset(&player, 0, sizeof(player)); player.active = 1; player.health = 100;
    player.weapon_slot = -1; player.world_epoch = 1;
    mp_world_key("maps\\survive_01.map", &player.world_low, &player.world_high);
    MpMonsterState monster = {0}, decoded;
    monster.sequence = 1; monster.world_low = player.world_low; monster.world_high = player.world_high;
    monster.world_epoch = 1; monster.monster_id = 1; monster.vid = 50; monster.tick = 100;
    monster.alive = 1; monster.health = 90; monster.x = -123.5f; monster.y = 200.25f;
    monster.direction = 64; monster.moving = 1; monster.velocity = 0.02f;
    uint8_t packet[MP_MONSTER_STATE_SIZE]; mp_monster_state_encode(packet, &monster);
    CHECK(packet[3] == MP_MONSTER_VERSION && packet[7] == 1 && packet[27] == 50);
    CHECK(mp_monster_state_decode(packet, sizeof(packet), &decoded) && !memcmp(&decoded, &monster, sizeof(monster)));
    CHECK(!mp_monster_state_decode(packet, sizeof(packet) - 1, &decoded));
    packet[3] = 2; CHECK(!mp_monster_state_decode(packet, sizeof(packet), &decoded));
    mp_monster_state_encode(packet, &monster); mp_put_u32(packet + 24, 290);
    CHECK(!mp_monster_state_decode(packet, sizeof(packet), &decoded));
    mp_monster_state_encode(packet, &monster); mp_put_u32(packet + 36, 0x7FC00000u);
    CHECK(!mp_monster_state_decode(packet, sizeof(packet), &decoded));
    mp_monster_state_encode(packet, &monster); mp_put_u32(packet + 60, 0);
    CHECK(!mp_monster_state_decode(packet, sizeof(packet), &decoded));
    MpMonsterEvent event = {0}, event_decoded;
    event.sequence = 1; event.world_low = monster.world_low; event.world_high = monster.world_high;
    event.world_epoch = 1; event.monster_id = 1; event.tick = 200; event.kind = MP_MONSTER_ATTACK;
    uint8_t event_packet[MP_MONSTER_EVENT_SIZE]; mp_monster_event_encode(event_packet, &event);
    CHECK(mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded) && !memcmp(&event, &event_decoded, sizeof(event)));
    mp_put_u32(event_packet + 28, 3); CHECK(!mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded));

    unsigned short port = (unsigned short)(51000 + GetCurrentProcessId() % 3000);
    CHECK(!_putenv_s("ASMP_MAP", "maps\\survive_01.map"));
    MpServer* server = mp_server_create(port, 4); CHECK(server);
    CHECK(!_putenv_s("ASMP_MAP", ""));
    StateClient* host = state_client_create("127.0.0.1", port, "Host", NULL); CHECK(host);
    StateClient* client = state_client_create("127.0.0.1", port, "Client", NULL); CHECK(client);
    state_client_monster_claim(host);
    pump(server, host, client, NULL, NULL, 1200);
    CHECK(!strcmp(state_client_map(host), "maps\\survive_01.map"));
    uint32_t authority_id, authority_session; int is_local;
    CHECK(state_client_monster_authority(host, &authority_id, &authority_session, &is_local) && is_local);
    CHECK(state_client_monster_authority(client, NULL, NULL, &is_local) && !is_local);
    state_client_monster_claim(client); /* Existing authority cannot be stolen. */
    CHECK(!state_client_publish_monster(client, &monster, GetTickCount()));
    CHECK(!state_client_send_monster_event(client, &event, GetTickCount()));
    CHECK(state_client_publish_monster(host, &monster, GetTickCount()));
    pump(server, host, client, NULL, NULL, 100);
    MonsterState observed; MonsterEvent received;
    CHECK(state_client_monster(client, GetTickCount(), &observed) && observed.present && observed.state.x == monster.x);
    CHECK(observed.authority_id == authority_id && observed.authority_session == authority_session);
    CHECK(!state_client_take_monster_event(client, &received)); /* Never infer attacks from states. */
    monster.x = 999; CHECK(state_client_publish_monster(host, &monster, GetTickCount()));
    pump(server, host, client, NULL, NULL, 100);
    CHECK(state_client_monster(client, GetTickCount(), &observed) && observed.state.x == -123.5f);
    ++monster.sequence; CHECK(state_client_publish_monster(host, &monster, GetTickCount()));
    pump(server, host, client, NULL, NULL, 100);
    CHECK(state_client_monster(client, GetTickCount(), &observed) && observed.state.x == 999);
    CHECK(state_client_send_monster_event(host, &event, GetTickCount()));
    pump(server, host, client, NULL, NULL, 100);
    CHECK(state_client_take_monster_event(client, &received) && received.event.kind == MP_MONSTER_ATTACK && received.event.sequence == 1);
    CHECK(state_client_send_monster_event(host, &event, GetTickCount()));
    pump(server, host, client, NULL, NULL, 100); CHECK(!state_client_take_monster_event(client, &received));

    epnet_client_t* raw = raw_join(server, host, client, port);
    ++monster.sequence; monster.x = 333; raw_state(raw, authority_session, &monster);
    pump(server, host, client, NULL, raw, 100);
    CHECK(state_client_monster(client, GetTickCount(), &observed) && observed.state.x == 999); /* Server rejects forged non-host. */
    epnet_client_disconnect(raw); epnet_client_destroy(raw);
    ++monster.sequence; monster.x = 444;
    CHECK(state_client_publish_monster(host, &monster, GetTickCount()));
    pump(server, host, client, NULL, NULL, 50);
    StateClient* late = state_client_create("127.0.0.1", port, "Late", NULL); CHECK(late);
    pump(server, host, client, late, NULL, 500);
    CHECK(state_client_monster(late, GetTickCount(), &observed) && observed.state.x == 444);
    CHECK(!state_client_take_monster_event(late, &received)); /* History is not an attack event. */
    CHECK(!state_client_monster(client, GetTickCount() + MP_MONSTER_EXPIRY_MS + 1u, &observed));
    player.active = 0; player.health = 0; pump(server, host, client, late, NULL, 50);
    ++monster.sequence; CHECK(state_client_publish_monster(host, &monster, GetTickCount()));
    pump(server, host, client, late, NULL, 50);
    CHECK(state_client_monster(client, GetTickCount(), &observed) && observed.state.alive);
    monster.alive = 0; monster.health = 0; monster.event_sequence = 2; ++monster.sequence;
    CHECK(state_client_publish_monster(host, &monster, GetTickCount()));
    pump(server, host, client, late, NULL, 100);
    CHECK(state_client_monster(client, GetTickCount(), &observed) && !observed.state.alive); /* Lost death event still despawns. */
    player.active = 1; player.health = 100; ++player.world_epoch;
    pump(server, host, client, late, NULL, 100);
    CHECK(!state_client_monster(client, GetTickCount(), &observed));
    CHECK(!state_client_publish_monster(host, &monster, GetTickCount()));
    CHECK(!state_client_send_monster_event(host, &event, GetTickCount()));
    monster.world_epoch = player.world_epoch; monster.alive = 1; monster.health = 90; monster.sequence = 1; monster.event_sequence = 0;
    CHECK(state_client_publish_monster(host, &monster, GetTickCount()));
    pump(server, host, client, late, NULL, 100);
    CHECK(state_client_monster(client, GetTickCount(), &observed) && observed.state.world_epoch == 2);
    state_client_destroy(host); host = NULL;
    pump(server, NULL, client, late, NULL, 1100);
    CHECK(!state_client_monster(client, GetTickCount(), &observed));
    host = state_client_create("127.0.0.1", port, "Rejoined", NULL); CHECK(host);
    state_client_monster_claim(host); pump(server, host, client, late, NULL, 800);
    CHECK(state_client_monster_authority(host, NULL, NULL, &is_local) && !is_local);
    CHECK(!state_client_publish_monster(host, &monster, GetTickCount())); /* Same slot, new session has no authority. */
    state_client_destroy(host); state_client_destroy(client); state_client_destroy(late);
    pump(server, NULL, NULL, NULL, NULL, 150);

    /* Once the room empties a new raw owner proves the server's checks directly,
       including packets that a production client deliberately refuses to send. */
    raw = raw_join(server, NULL, NULL, port);
    raw_publish_player = 1; raw_player_sequence = raw_player_at = 0;
    uint8_t claim[8]; mp_put_u32(claim, MP_MONSTER_VERSION); mp_put_u32(claim + 4, raw_session);
    epnet_client_send(raw, MPT_C_MONSTER_CLAIM, claim, sizeof(claim));
    client = state_client_create("127.0.0.1", port, "Observer", NULL); CHECK(client);
    DWORD wait_started = GetTickCount(); PeerState raw_owner;
    do {
        pump(server, NULL, client, NULL, raw, 10);
        if (state_client_monster_authority(client, &authority_id, &authority_session, &is_local) &&
            authority_id == epnet_client_get_id(raw) && authority_session == raw_session && !is_local &&
            state_client_peer_info(client, authority_id, GetTickCount(), &raw_owner) &&
            raw_owner.session == raw_session && raw_owner.state.world_epoch == player.world_epoch) break;
    } while (GetTickCount() - wait_started < 2000u);
    CHECK(state_client_monster_authority(client, &authority_id, &authority_session, &is_local) &&
        authority_id == epnet_client_get_id(raw) && authority_session == raw_session && !is_local);
    CHECK(state_client_peer_info(client, authority_id, GetTickCount(), &raw_owner) && raw_owner.session == raw_session);
    raw_state(raw, raw_session + 1u, &monster);
    pump(server, NULL, client, NULL, raw, 100); CHECK(!state_client_monster(client, GetTickCount(), &observed));
    --monster.world_epoch; raw_state(raw, raw_session, &monster);
    pump(server, NULL, client, NULL, raw, 100); CHECK(!state_client_monster(client, GetTickCount(), &observed));
    ++monster.world_epoch; raw_state(raw, raw_session, &monster);
    wait_started = GetTickCount();
    do {
        pump(server, NULL, client, NULL, raw, 10);
        if (state_client_monster(client, GetTickCount(), &observed)) break;
    } while (GetTickCount() - wait_started < 2000u);
    CHECK(state_client_monster(client, GetTickCount(), &observed) && observed.authority_session == raw_session);
    epnet_client_disconnect(raw); epnet_client_destroy(raw); state_client_destroy(client); mp_server_destroy(server);
    puts("Monster transport authority, session/world validation, explicit events, dedup, late join and expiry checks passed.");
    return 0;
}
