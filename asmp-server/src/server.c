/**
 * @file server.c
 * @brief Top level server-side multiplayer logic.
 *
 * Layer: asmp-server (relay). Accepts clients over epnet, validates their
 * state, shot and monster packets, and relays them with the sender ID and
 * session. It also keeps the room (lobby.c): members, host, ready flags and
 * the start/load barrier. Single-threaded: mp_server_tick does all the work.
 *
 */
#include <stdbool.h>
#include <string.h>
#include "server.h"
#include "protocol.h"
#include "monster_protocol.h"
#include "lobby_protocol.h"
#include "lobby.h"
#include "utils/mem/mem.h"
#include "utils/time/time.h"
#include "epnet.h"
#include "epnet_server.h"
#include <stdio.h> // TODO: Remove.
#include <stdlib.h>
#include <winsock2.h>

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
    Lobby lobby;
    bool lobby_dirty;
    unsigned long lobby_sent_at;
} MpServer;


static void process_received_packets_(MpServer* server);
static void send_roster_(MpServer* server);

/* Tells one client who the monster authority is (id UINT32_MAX when nobody). */
static void send_monster_authority_(MpServer* server, unsigned int recipient)
{
    uint8_t packet[12];
    mp_put_u32(packet, MP_MONSTER_VERSION);
    mp_put_u32(packet + 4, server->monster_claimed ? server->monster_host : UINT32_MAX);
    mp_put_u32(packet + 8, server->monster_claimed ? server->monster_session : 0);
    epnet_server_send(server->ns, (uint8_t)recipient, MPT_S_MONSTER_AUTHORITY, packet,
                      sizeof(packet));
}

/* True when the packet comes from the current authority and carries its session. */
static int monster_owner_(MpServer* server, unsigned int sender, const uint8_t* data, int length)
{
    return length >= 4 && server->monster_claimed && sender == server->monster_host &&
           server->players[sender].session == server->monster_session &&
           mp_get_u32(data) == server->monster_session;
}

/* True when the sender's last state is fresh and in exactly this world. */
static int monster_world_(MpServer* server, unsigned int sender, uint32_t low, uint32_t high,
                          uint32_t epoch)
{
    Player* p = &server->players[sender];
    return p->state_received &&
           mp_monster_world(p->latest.world_low, p->latest.world_high, p->latest.world_epoch) &&
           server->tick_time_ms - p->state_at <= MP_MONSTER_EXPIRY_MS &&
           p->latest.world_low == low && p->latest.world_high == high &&
           p->latest.world_epoch == epoch;
}

/* Relays one monster state to a client, only while the authority is connected, in the
   same session and its world matches. */
static void send_monster_state_(MpServer* server, unsigned int recipient, unsigned int slot)
{
    if (!server->monster_received[slot] ||
        server->tick_time_ms - server->monster_at[slot] > MP_MONSTER_EXPIRY_MS ||
        !server->players[server->monster_host].is_connected ||
        server->players[server->monster_host].session != server->monster_session ||
        !monster_world_(server, server->monster_host, server->monster[slot].world_low,
                        server->monster[slot].world_high, server->monster[slot].world_epoch))
        return;
    uint8_t packet[MP_MONSTER_STATE_RELAY_SIZE];
    mp_put_u32(packet, server->monster_host);
    mp_put_u32(packet + 4, server->monster_session);
    mp_monster_state_encode(packet + 8, &server->monster[slot]);
    epnet_server_send(server->ns, (uint8_t)recipient, MPT_S_MONSTER_STATE, packet, sizeof(packet));
}

/* Relays every monster slot to a client. */
static void send_all_monsters_(MpServer* server, unsigned int recipient)
{
    for (unsigned int slot = 0; slot < MP_MONSTER_CAPACITY; ++slot)
        send_monster_state_(server, recipient, slot);
}

/* Sends the current room snapshot to one client. */
static void send_lobby_(MpServer* server, unsigned int recipient)
{
    MpLobby snapshot;
    lobby_snapshot(&server->lobby, &snapshot);

    uint8_t packet[MP_LOBBY_MAX_SIZE];
    int length = mp_lobby_encode(packet, sizeof(packet), &snapshot);
    if (length)
        epnet_server_send(server->ns, (uint8_t)recipient, MPT_S_LOBBY, packet, (size_t)length);
}

/* Sends the room snapshot to every registered client. */
static void broadcast_lobby_(MpServer* server)
{
    for (unsigned int i = 0; i < server->max_clients; ++i)
    {
        if (server->players[i].is_connected)
            send_lobby_(server, i);
    }
    server->lobby_dirty = false;
    server->lobby_sent_at = server->tick_time_ms;
}

/* Applies one lobby request from a registered client. Invalid packets are dropped;
   a change is broadcast with the next tick. */
static void process_lobby_request_(MpServer* server, uint8_t sender, uint8_t type,
                                   const uint8_t* data, int length)
{
    Player* p = &server->players[sender];
    enum LobbyResult result = LOBBY_UNCHANGED;

    if (type == MPT_C_LOBBY_READY)
    {
        MpLobbyReady request;
        if (mp_lobby_ready_decode(data, length, &request))
            result = lobby_set_ready(&server->lobby, sender, p->session, &request);
    }
    else if (type == MPT_C_LOBBY_SETTINGS)
    {
        MpLobbySettings request;
        if (mp_lobby_settings_decode(data, length, &request))
            result = lobby_change_settings(&server->lobby, sender, p->session, &request);
    }
    else if (type == MPT_C_LOBBY_START)
    {
        MpLobbyStart request;
        if (mp_lobby_start_decode(data, length, &request))
            result =
                lobby_start(&server->lobby, sender, p->session, &request, server->tick_time_ms);
    }
    else if (type == MPT_C_LOBBY_LOADED)
    {
        MpLobbyLoaded request;
        if (mp_lobby_loaded_decode(data, length, &request))
            result = lobby_mark_loaded(&server->lobby, sender, p->session, &request);
    }

    if (result == LOBBY_CHANGED)
        server->lobby_dirty = true;
}

/* Registers the client's name and answers with welcome, authority, monster states and
   the room snapshot. */
static void process_hello_(MpServer* server, uint8_t sender, const char* name)
{
    Player* p = &server->players[sender];
    memcpy(p->name, name, sizeof(p->name));
    p->is_connected = true;
    server->roster_dirty = true;
    if (lobby_join(&server->lobby, sender, p->session, p->name) == LOBBY_CHANGED)
        server->lobby_dirty = true;
    /* A repeated hello (lost welcome) is answered again. */
    uint8_t welcome[13 + MP_MAX_MAP_NAME_LEN];
    int length =
        mp_welcome_encode(welcome, sizeof(welcome), sender, server->max_clients, server->map_name);
    if (length)
        epnet_server_send(server->ns, sender, MPT_S_WELCOME, welcome, (size_t)length);
    send_monster_authority_(server, sender);
    send_all_monsters_(server, sender);
    send_lobby_(server, sender);
    printf("Client %d is %s\n", sender, p->name);
}

/* Validates every packet (length, session, sequence) and relays state, shots and monsters.
   Invalid packets are dropped. */
static void process_received_packets_(MpServer* server)
{
    epnet_srv_event_t ev;
    while (epnet_server_poll_events(server->ns, &ev))
    {
        switch (ev.type)
        {
        case EPNET_SRV_EVENT_CLIENT_JOIN:
        {
            if (ev.client_id < server->max_clients)
            {
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
                Player* leaving = &server->players[ev.client_id];
                if (leaving->is_connected &&
                    lobby_leave(&server->lobby, ev.client_id, leaving->session) == LOBBY_CHANGED)
                    server->lobby_dirty = true;
                server->players[ev.client_id].is_connected = false;
                server->players[ev.client_id].transport_connected = false;
                server->roster_dirty = true;
                bool empty = true;
                for (unsigned int i = 0; i < server->max_clients; ++i)
                    if (server->players[i].transport_connected)
                        empty = false;
                if (empty)
                {
                    server->monster_claimed = false;
                    memset(server->monster_received, 0, sizeof(server->monster_received));
                    memset(server->monster_event_received, 0,
                           sizeof(server->monster_event_received));
                    memset(&server->monster, 0, sizeof(server->monster));
                }
            }
            break;
        }
        case EPNET_SRV_EVENT_PACKET:
        {
            uint8_t sender = ev.client_id;
            if (sender >= server->max_clients)
                break;
            int length = ev.data.packet.len;
            if (ev.data.packet.pkt_type != MPT_C_HELLO && !server->players[sender].is_connected)
                break;
            switch (ev.data.packet.pkt_type)
            {
            case MPT_C_HELLO:
            {
                char name[MP_MAX_NAME_LEN + 1];
                if (!mp_hello_decode(ev.data.packet.data, length, name))
                    break;
                process_hello_(server, sender, name);
                break;
            }
            case MPT_C_STATE:
            {
                MpState state;
                Player* p = &server->players[sender];
                if (!mp_state_decode(ev.data.packet.data, length, &state) ||
                    (p->state_received && !mp_sequence_newer(state.sequence, p->state_sequence)))
                    break;
                p->state_sequence = state.sequence;
                p->state_received = true;
                if (server->monster_claimed && sender == server->monster_host &&
                    p->session == server->monster_session &&
                    (!mp_monster_world(state.world_low, state.world_high, state.world_epoch) ||
                     state.world_low != p->latest.world_low ||
                     state.world_high != p->latest.world_high ||
                     state.world_epoch != p->latest.world_epoch))
                {
                    memset(server->monster_received, 0, sizeof(server->monster_received));
                    memset(server->monster_event_received, 0,
                           sizeof(server->monster_event_received));
                }
                p->latest = state;
                p->state_at = server->tick_time_ms;
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
                if (length != 8 || mp_get_u32(data) != MP_MONSTER_VERSION ||
                    mp_get_u32(data + 4) != p->session)
                    break;
                if (!server->monster_claimed)
                {
                    server->monster_claimed = true;
                    server->monster_host = sender;
                    server->monster_session = p->session;
                    memset(server->monster_received, 0, sizeof(server->monster_received));
                    memset(server->monster_event_received, 0,
                           sizeof(server->monster_event_received));
                    printf("Monster authority id=%u session=%u\n", sender, p->session);
                }
                for (unsigned int i = 0; i < server->max_clients; ++i)
                    if (server->players[i].is_connected)
                        send_monster_authority_(server, i);
                break;
            }
            case MPT_C_MONSTER_STATE:
            {
                const uint8_t* data = ev.data.packet.data;
                MpMonsterState state;
                if (length != MP_MONSTER_STATE_CLIENT_SIZE ||
                    !monster_owner_(server, sender, data, length) ||
                    !mp_monster_state_decode(data + 4, MP_MONSTER_STATE_SIZE, &state) ||
                    !monster_world_(server, sender, state.world_low, state.world_high,
                                    state.world_epoch) ||
                    (server->monster_received[state.monster_id - 1u] &&
                     !mp_monster_state_progresses(&server->monster[state.monster_id - 1u], &state)))
                    break;
                server->monster[state.monster_id - 1u] = state;
                server->monster_received[state.monster_id - 1u] = true;
                server->monster_at[state.monster_id - 1u] = server->tick_time_ms;
                for (unsigned int i = 0; i < server->max_clients; ++i)
                    if (server->players[i].is_connected)
                        send_monster_state_(server, i, state.monster_id - 1u);
                break;
            }
            case MPT_C_MONSTER_EVENT:
            {
                const uint8_t* data = ev.data.packet.data;
                MpMonsterEvent event;
                if (length != MP_MONSTER_EVENT_CLIENT_SIZE ||
                    !monster_owner_(server, sender, data, length) ||
                    !mp_monster_event_decode(data + 4, MP_MONSTER_EVENT_SIZE, &event) ||
                    !monster_world_(server, sender, event.world_low, event.world_high,
                                    event.world_epoch) ||
                    !server->monster_received[event.monster_id - 1u] ||
                    server->tick_time_ms - server->monster_at[event.monster_id - 1u] >
                        MP_MONSTER_EXPIRY_MS ||
                    event.world_low != server->monster[event.monster_id - 1u].world_low ||
                    event.world_high != server->monster[event.monster_id - 1u].world_high ||
                    event.world_epoch != server->monster[event.monster_id - 1u].world_epoch ||
                    !mp_monster_event_matches(&server->monster[event.monster_id - 1u], &event) ||
                    (server->monster_event_received[event.monster_id - 1u] &&
                     !mp_sequence_newer(event.sequence,
                                        server->monster_event_sequence[event.monster_id - 1u])))
                    break;
                server->monster_event_received[event.monster_id - 1u] = true;
                server->monster_event_sequence[event.monster_id - 1u] = event.sequence;
                uint8_t packet[MP_MONSTER_EVENT_RELAY_SIZE];
                mp_put_u32(packet, sender);
                mp_put_u32(packet + 4, server->monster_session);
                memcpy(packet + 8, data + 4, MP_MONSTER_EVENT_SIZE);
                for (unsigned int i = 0; i < server->max_clients; ++i)
                    if (server->players[i].is_connected)
                        epnet_server_send(server->ns, (uint8_t)i, MPT_S_MONSTER_EVENT, packet,
                                          sizeof(packet));
                break;
            }
            case MPT_C_LOBBY_READY:
            case MPT_C_LOBBY_SETTINGS:
            case MPT_C_LOBBY_START:
            case MPT_C_LOBBY_LOADED:
            {
                process_lobby_request_(server, sender, ev.data.packet.pkt_type, ev.data.packet.data,
                                       length);
                break;
            }
            case MPT_C_SHOT:
            {
                MpShot shot;
                Player* p = &server->players[sender];
                /* A shot is meaningful only after the sender's first valid state. */
                if (!p->state_received || !mp_shot_decode(ev.data.packet.data, length, &shot) ||
                    (p->shot_received && !mp_sequence_newer(shot.sequence, p->shot_sequence)))
                    break;
                p->shot_sequence = shot.sequence;
                p->shot_received = true;
                uint8_t relay[MP_SHOT_RELAY_SIZE];
                mp_put_u32(relay, sender);
                mp_put_u32(relay + 4, p->session);
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

/* Sends the list of connected players (id, session, name) to every client. */
static void send_roster_(MpServer* server)
{
    MpRosterEntry entries[MP_ROSTER_MAX_ENTRIES];
    unsigned int count = 0;
    for (unsigned int i = 0; i < server->max_clients; i++)
    {
        if (!server->players[i].is_connected)
            continue;
        entries[count].id = i;
        entries[count].session = server->players[i].session;
        memcpy(entries[count].name, server->players[i].name, sizeof(entries[count].name));
        ++count;
    }
    uint8_t packet[8 + MP_ROSTER_MAX_ENTRIES * (9 + MP_MAX_NAME_LEN)];
    int length = mp_roster_encode(packet, sizeof(packet), entries, count);
    if (!length)
        return;
    for (unsigned int i = 0; i < server->max_clients; i++)
        if (server->players[i].is_connected)
            epnet_server_send(server->ns, i, MPT_S_ROSTER, packet, (size_t)length);
}

/* epnet binds with SO_REUSEADDR, so on Windows a second server would silently share
   the port with a running one. An exclusive bind fails if any socket holds it. The
   probe socket is closed again before epnet binds. Winsock must be started. */
static int port_free_(unsigned short port)
{
    SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe == INVALID_SOCKET)
        return 0;

    BOOL exclusive = TRUE;
    setsockopt(probe, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    int bound = bind(probe, (struct sockaddr*)&address, sizeof(address)) == 0;
    closesocket(probe);
    return bound;
}

int mp_server_port_free(unsigned short port)
{
    if (epnet_init())
        return 0;
    int is_free = port_free_(port);
    epnet_shutdown();
    return is_free;
}

/* Creates the server on `port`. The map comes from ASMP_MAP when valid. Returns NULL on
   failure, including a port another socket already holds; the caller frees it with
   mp_server_destroy. */
MpServer* mp_server_create(unsigned short port, int max_clients)
{
    if (max_clients < 1 || max_clients > EPNET_MAX_CLIENTS)
        return 0;
    if (!mp_server_port_free(port))
        return 0;
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
                mem_set(server->players, 0, sizeof(*server->players) * max_clients);
                server->max_clients = (unsigned int)max_clients;
                strcpy_s(server->map_name, MP_MAX_MAP_NAME_LEN, "maps\\Level_01.map");
                char configured_map[128];
                size_t configured_length = 0;
                if (!getenv_s(&configured_length, configured_map, sizeof(configured_map),
                              "ASMP_MAP") &&
                    configured_length && strlen(configured_map) < sizeof(server->map_name) &&
                    mp_is_level_path(configured_map))
                    strcpy_s(server->map_name, sizeof(server->map_name), configured_map);
                server->prev_tick_time_ms = time_get_ms();
                uint32_t seed = (uint32_t)server->prev_tick_time_ms ^ (uint32_t)(uintptr_t)server;
                if (lobby_init(&server->lobby, seed, server->max_clients, server->map_name))
                    return server;
                mem_free(server->players);
            }
            epnet_server_destroy(server->ns);
        }
        epnet_shutdown();
        mem_free(server);
    }
    return 0;
}

/* Frees the server (NULL is accepted). */
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

/* One server step: transport update, packet processing, 250 ms monster relay and the
   periodic roster. */
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
    if (server->tick_time_ms - server->monster_relay_at >= 250u)
    {
        for (unsigned int i = 0; i < server->max_clients; ++i)
            if (server->players[i].is_connected)
                send_all_monsters_(server, i);
        server->monster_relay_at = server->tick_time_ms;
    }

    if (lobby_tick(&server->lobby, server->tick_time_ms) == LOBBY_CHANGED)
        server->lobby_dirty = true;
    if (server->lobby_dirty || server->tick_time_ms - server->lobby_sent_at >= MP_LOBBY_INTERVAL_MS)
        broadcast_lobby_(server);

    if (server->roster_dirty ||
        (server->tick_time_ms - server->roster_sent_time_ms) >= MP_ROSTER_INTERVAL_MS)
    {
        send_roster_(server);
        for (unsigned int i = 0; i < server->max_clients; ++i)
            if (server->players[i].is_connected)
                send_monster_authority_(server, i);
        server->roster_dirty = false;
        server->roster_sent_time_ms = server->tick_time_ms;
    }
}
