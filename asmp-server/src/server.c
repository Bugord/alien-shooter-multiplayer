/**
 * @file server.c
 * @brief Top level server-side multiplayer logic.
 *
 */
#include <stdbool.h>
#include <string.h>
#include "server.h"
#include "protocol.h"
#include "utils/mem/mem.h"
#include "utils/time/time.h"
#include "epnet.h"
#include "epnet_server.h"
#include <stdio.h> // TODO: Remove.

typedef struct Player
{
    char name[MP_MAX_NAME_LEN + 1];
    bool is_connected;
    uint32_t session, state_sequence;
    bool state_received;
    uint32_t shot_sequence;
    bool shot_received;
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
} MpServer;


static void process_received_packets_(MpServer* server);
static void send_roster_(MpServer* server);

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
                server->roster_dirty = true;
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
                uint8_t relay[MP_STATE_RELAY_SIZE];
                mp_put_u32(relay, sender);
                mp_put_u32(relay + 4, p->session);
                memcpy(relay + 8, ev.data.packet.data, MP_STATE_SIZE);
                for (uint8_t i = 0; i < server->max_clients; ++i)
                    if (i != sender && server->players[i].is_connected)
                        epnet_server_send(server->ns, i, MPT_S_STATE, relay, sizeof(relay));
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

    if (server->roster_dirty ||
        (server->tick_time_ms - server->roster_sent_time_ms) >= MP_ROSTER_INTERVAL_MS)
    {
        send_roster_(server);
        server->roster_dirty = false;
        server->roster_sent_time_ms = server->tick_time_ms;
    }
}
