/**
 * @file server.c
 * @brief Top level server-side multiplayer logic.
 *
 */
#include <stdbool.h>
#include <string.h>
#include "server.h"
#include "protocol.h"
#include "monster_protocol.h"
#include "utils/mem/mem.h"
#include "utils/time/time.h"
#include "epnet.h"
#include "epnet_server.h"
#include <stdio.h> // TODO: Remove.
#include <stdlib.h>

typedef struct Player
{
    char name[MP_MAX_NAME_LEN + 1];
    bool is_connected;
    bool transport_connected;
    uint32_t session, state_sequence;
    bool state_received;
    uint32_t shot_sequence;
    bool shot_received;
    MpState latest;
    unsigned long state_at;
} Player;

typedef struct MpServer
{
    epnet_server_t* ns;
    unsigned int max_clients;
    char map_name[MP_MAX_MAP_NAME_LEN];
    bool roster_dirty;
    unsigned long tick_time_ms;
    unsigned long prev_tick_time_ms;
    Player* players;
    unsigned long roster_sent_time_ms;
    bool monster_claimed;
    bool monster_received[MP_MONSTER_CAPACITY], monster_event_received[MP_MONSTER_CAPACITY];
    uint32_t monster_host, monster_session;
    uint32_t monster_event_sequence[MP_MONSTER_CAPACITY];
    MpMonsterState monster[MP_MONSTER_CAPACITY];
    unsigned long monster_at[MP_MONSTER_CAPACITY], monster_relay_at;
} MpServer;


static void process_received_packets_(MpServer* server);
static void send_roster_(MpServer* server);
static void send_monster_authority_(MpServer* server, unsigned int recipient) {
    uint8_t packet[12]; mp_put_u32(packet, MP_MONSTER_VERSION);
    mp_put_u32(packet + 4, server->monster_claimed ? server->monster_host : UINT32_MAX);
    mp_put_u32(packet + 8, server->monster_claimed ? server->monster_session : 0);
    epnet_server_send(server->ns, (uint8_t)recipient, MPT_S_MONSTER_AUTHORITY, packet, sizeof(packet));
}
static int monster_owner_(MpServer* server, unsigned int sender, const uint8_t* data, int length) {
    return length >= 4 && server->monster_claimed && sender == server->monster_host &&
        server->players[sender].session == server->monster_session && mp_get_u32(data) == server->monster_session;
}
static int monster_world_(MpServer* server, unsigned int sender, uint32_t low, uint32_t high, uint32_t epoch) {
    Player* p = &server->players[sender];
    return p->state_received && mp_monster_world(p->latest.world_low, p->latest.world_high, p->latest.world_epoch) &&
        server->tick_time_ms - p->state_at <= MP_MONSTER_EXPIRY_MS &&
        p->latest.world_low == low && p->latest.world_high == high && p->latest.world_epoch == epoch;
}
static void send_monster_state_(MpServer* server, unsigned int recipient, unsigned int slot) {
    if (!server->monster_received[slot] || server->tick_time_ms - server->monster_at[slot] > MP_MONSTER_EXPIRY_MS ||
        !server->players[server->monster_host].is_connected ||
        server->players[server->monster_host].session != server->monster_session ||
        !monster_world_(server, server->monster_host, server->monster[slot].world_low, server->monster[slot].world_high, server->monster[slot].world_epoch)) return;
    uint8_t packet[MP_MONSTER_STATE_RELAY_SIZE];
    mp_put_u32(packet, server->monster_host); mp_put_u32(packet + 4, server->monster_session);
    mp_monster_state_encode(packet + 8, &server->monster[slot]);
    epnet_server_send(server->ns, (uint8_t)recipient, MPT_S_MONSTER_STATE, packet, sizeof(packet));
}

static void send_all_monsters_(MpServer* server, unsigned int recipient) {
    for (unsigned int slot = 0; slot < MP_MONSTER_CAPACITY; ++slot) send_monster_state_(server, recipient, slot);
}

static void process_hello_(MpServer* server, uint8_t sender, const char* name)
{
    Player* p = &server->players[sender];
    memcpy(p->name, name, sizeof(p->name));
    p->is_connected = true;
    server->roster_dirty = true;
    /* A repeated hello (lost welcome) is answered again. */
    uint8_t welcome[13 + MP_MAX_MAP_NAME_LEN];
    int length = mp_welcome_encode(welcome, sizeof(welcome), sender, server->max_clients, server->map_name);
    if (length) epnet_server_send(server->ns, sender, MPT_S_WELCOME, welcome, (size_t)length);
    send_monster_authority_(server, sender);
    send_all_monsters_(server, sender);
    printf("Client %d is %s\n", sender, p->name);
}

static void process_received_packets_(MpServer* server)
{
    epnet_srv_event_t ev;
    while (epnet_server_poll_events(server->ns, &ev))
    {
        switch (ev.type)
        {
        case EPNET_SRV_EVENT_CLIENT_JOIN:
        {
            if (ev.client_id < server->max_clients) {
                Player* p = &server->players[ev.client_id];
                uint32_t session = p->session + 1;
                memset(p, 0, sizeof(*p));
                p->session = session ? session : 1;
                p->transport_connected = true;
            }
            printf("Client %d joined (low-level)\n", ev.client_id);
            break;
        }
        case EPNET_SRV_EVENT_CLIENT_LEAVE:
        {
            printf("Client %d left\n", ev.client_id);
            if (ev.client_id < server->max_clients)
            {
                server->players[ev.client_id].is_connected = false;
                server->players[ev.client_id].transport_connected = false;
                server->roster_dirty = true;
                bool empty = true;
                for (unsigned int i = 0; i < server->max_clients; ++i)
                    if (server->players[i].transport_connected) empty = false;
                if (empty) {
                    server->monster_claimed = false;
                    memset(server->monster_received, 0, sizeof(server->monster_received));
                    memset(server->monster_event_received, 0, sizeof(server->monster_event_received));
                    memset(&server->monster, 0, sizeof(server->monster));
                }
            }
            break;
        }
        case EPNET_SRV_EVENT_PACKET:
        {
            uint8_t sender = ev.client_id;
            if (sender >= server->max_clients) break;
            int length = ev.data.packet.len;
            if (ev.data.packet.pkt_type != MPT_C_HELLO &&
                !server->players[sender].is_connected) break;
            switch (ev.data.packet.pkt_type)
            {
            case MPT_C_HELLO:
            {
                char name[MP_MAX_NAME_LEN + 1];
                if (!mp_hello_decode(ev.data.packet.data, length, name)) break;
                process_hello_(server, sender, name);
                break;
            }
            case MPT_C_STATE:
            {
                MpState state;
                Player* p = &server->players[sender];
                if (!mp_state_decode(ev.data.packet.data, length, &state) ||
                    (p->state_received && !mp_sequence_newer(state.sequence, p->state_sequence))) break;
                p->state_sequence = state.sequence;
                p->state_received = true;
                if (server->monster_claimed && sender == server->monster_host && p->session == server->monster_session &&
                    (!mp_monster_world(state.world_low, state.world_high, state.world_epoch) ||
                     state.world_low != p->latest.world_low || state.world_high != p->latest.world_high ||
                     state.world_epoch != p->latest.world_epoch)) {
                    memset(server->monster_received, 0, sizeof(server->monster_received));
                    memset(server->monster_event_received, 0, sizeof(server->monster_event_received));
                }
                p->latest = state; p->state_at = server->tick_time_ms;
                uint8_t relay[MP_STATE_RELAY_SIZE];
                mp_put_u32(relay, sender);
                mp_put_u32(relay + 4, p->session);
                memcpy(relay + 8, ev.data.packet.data, MP_STATE_SIZE);
                for (uint8_t i = 0; i < server->max_clients; ++i)
                    if (i != sender && server->players[i].is_connected)
                        epnet_server_send(server->ns, i, MPT_S_STATE, relay, sizeof(relay));
                break;
            }
            case MPT_C_MONSTER_CLAIM:
            {
                const uint8_t* data = ev.data.packet.data;
                Player* p = &server->players[sender];
                if (length != 8 || mp_get_u32(data) != MP_MONSTER_VERSION || mp_get_u32(data + 4) != p->session) break;
                if (!server->monster_claimed) {
                    server->monster_claimed = true; server->monster_host = sender; server->monster_session = p->session;
                    memset(server->monster_received, 0, sizeof(server->monster_received));
                    memset(server->monster_event_received, 0, sizeof(server->monster_event_received));
                    printf("Monster authority id=%u session=%u\n", sender, p->session);
                }
                for (unsigned int i = 0; i < server->max_clients; ++i)
                    if (server->players[i].is_connected) send_monster_authority_(server, i);
                break;
            }
            case MPT_C_MONSTER_STATE:
            {
                const uint8_t* data = ev.data.packet.data; MpMonsterState state;
                if (length != MP_MONSTER_STATE_CLIENT_SIZE || !monster_owner_(server, sender, data, length) ||
                    !mp_monster_state_decode(data + 4, MP_MONSTER_STATE_SIZE, &state) ||
                    !monster_world_(server, sender, state.world_low, state.world_high, state.world_epoch) ||
                    (server->monster_received[state.monster_id - 1u] && !mp_sequence_newer(state.sequence, server->monster[state.monster_id - 1u].sequence))) break;
                server->monster[state.monster_id - 1u] = state; server->monster_received[state.monster_id - 1u] = true; server->monster_at[state.monster_id - 1u] = server->tick_time_ms;
                for (unsigned int i = 0; i < server->max_clients; ++i)
                    if (server->players[i].is_connected) send_monster_state_(server, i, state.monster_id - 1u);
                break;
            }
            case MPT_C_MONSTER_EVENT:
            {
                const uint8_t* data = ev.data.packet.data; MpMonsterEvent event;
                if (length != MP_MONSTER_EVENT_CLIENT_SIZE || !monster_owner_(server, sender, data, length) ||
                    !mp_monster_event_decode(data + 4, MP_MONSTER_EVENT_SIZE, &event) ||
                    !monster_world_(server, sender, event.world_low, event.world_high, event.world_epoch) ||
                    !server->monster_received[event.monster_id - 1u] || server->tick_time_ms - server->monster_at[event.monster_id - 1u] > MP_MONSTER_EXPIRY_MS ||
                    event.world_low != server->monster[event.monster_id - 1u].world_low || event.world_high != server->monster[event.monster_id - 1u].world_high ||
                    event.world_epoch != server->monster[event.monster_id - 1u].world_epoch ||
                    (event.kind == MP_MONSTER_ATTACK && !server->monster[event.monster_id - 1u].alive) ||
                    (server->monster_event_received[event.monster_id - 1u] && !mp_sequence_newer(event.sequence, server->monster_event_sequence[event.monster_id - 1u]))) break;
                server->monster_event_received[event.monster_id - 1u] = true; server->monster_event_sequence[event.monster_id - 1u] = event.sequence;
                uint8_t packet[MP_MONSTER_EVENT_RELAY_SIZE];
                mp_put_u32(packet, sender); mp_put_u32(packet + 4, server->monster_session);
                memcpy(packet + 8, data + 4, MP_MONSTER_EVENT_SIZE);
                for (unsigned int i = 0; i < server->max_clients; ++i)
                    if (server->players[i].is_connected)
                        epnet_server_send(server->ns, (uint8_t)i, MPT_S_MONSTER_EVENT, packet, sizeof(packet));
                break;
            }
            case MPT_C_SHOT:
            {
                MpShot shot;
                Player* p = &server->players[sender];
                /* A shot is meaningful only after the sender's first valid state. */
                if (!p->state_received || !mp_shot_decode(ev.data.packet.data, length, &shot) ||
                    (p->shot_received && !mp_sequence_newer(shot.sequence, p->shot_sequence))) break;
                p->shot_sequence = shot.sequence; p->shot_received = true;
                uint8_t relay[MP_SHOT_RELAY_SIZE];
                mp_put_u32(relay, sender); mp_put_u32(relay + 4, p->session);
                memcpy(relay + 8, ev.data.packet.data, MP_SHOT_SIZE);
                for (uint8_t i = 0; i < server->max_clients; ++i)
                    if (i != sender && server->players[i].is_connected)
                        epnet_server_send(server->ns, i, MPT_S_SHOT, relay, sizeof(relay));
                break;
            }
            default:
            {
                break;
            }
            }
            break;
        }
        case EPNET_SRV_EVENT_RELIABLE:
        {
            break;
        }
        }
    }
}

static void send_roster_(MpServer* server)
{
    MpRosterEntry entries[MP_ROSTER_MAX_ENTRIES];
    unsigned int count = 0;
    for (unsigned int i = 0; i < server->max_clients; i++)
    {
        if (!server->players[i].is_connected) continue;
        entries[count].id = i; entries[count].session = server->players[i].session;
        memcpy(entries[count].name, server->players[i].name, sizeof(entries[count].name));
        ++count;
    }
    uint8_t packet[8 + MP_ROSTER_MAX_ENTRIES * (9 + MP_MAX_NAME_LEN)];
    int length = mp_roster_encode(packet, sizeof(packet), entries, count);
    if (!length) return;
    for (unsigned int i = 0; i < server->max_clients; i++)
        if (server->players[i].is_connected)
            epnet_server_send(server->ns, i, MPT_S_ROSTER, packet, (size_t)length);
}

MpServer* mp_server_create(unsigned short port, int max_clients)
{
    if (max_clients < 1 || max_clients > EPNET_MAX_CLIENTS) return 0;
    MpServer* server = mem_alloc(sizeof(*server));
    if (server)
    {
        mem_set(server, 0, sizeof(*server));
        epnet_init();
        server->ns = epnet_server_create(port, max_clients);
        if (server->ns)
        {
            server->players = mem_alloc(sizeof(*server->players) * max_clients);
            if (server->players)
            {
                mem_set(server->players, 0,
                        sizeof(*server->players) * max_clients);
                server->max_clients = (unsigned int)max_clients;
                strcpy_s(server->map_name, MP_MAX_MAP_NAME_LEN, "maps\\Level_01.map");
                char configured_map[128]; size_t configured_length = 0;
                if (!getenv_s(&configured_length, configured_map, sizeof(configured_map), "ASMP_MAP") && configured_length &&
                    strlen(configured_map) < sizeof(server->map_name) && mp_is_level_path(configured_map))
                    strcpy_s(server->map_name, sizeof(server->map_name), configured_map);
                server->prev_tick_time_ms = time_get_ms();
                return server;
            }
            epnet_server_destroy(server->ns);
        }
        epnet_shutdown();
        mem_free(server);
    }
    return 0;
}

void mp_server_destroy(MpServer* server)
{
    if (server)
    {
        if (server->ns)
        {
            epnet_server_destroy(server->ns);
        }
        if (server->players)
        {
            mem_free(server->players);
        }
        epnet_shutdown();
        mem_free(server);
    }
}

void mp_server_tick(MpServer* server)
{
    if (!server)
    {
        return;
    }
    server->tick_time_ms = time_get_ms();
    double dt = (server->tick_time_ms - server->prev_tick_time_ms) / 1000.0;
    server->prev_tick_time_ms = server->tick_time_ms;
    epnet_server_update(server->ns, dt);
    process_received_packets_(server);
    if (server->tick_time_ms - server->monster_relay_at >= 250u) {
        for (unsigned int i = 0; i < server->max_clients; ++i)
            if (server->players[i].is_connected) send_all_monsters_(server, i);
        server->monster_relay_at = server->tick_time_ms;
    }

    if (server->roster_dirty ||
        (server->tick_time_ms - server->roster_sent_time_ms) >= MP_ROSTER_INTERVAL_MS)
    {
        send_roster_(server);
        for (unsigned int i = 0; i < server->max_clients; ++i)
            if (server->players[i].is_connected) send_monster_authority_(server, i);
        server->roster_dirty = false;
        server->roster_sent_time_ms = server->tick_time_ms;
    }
}
