/**
 * @file server.c
 * @brief Top level server-side multiplayer logic.
 *
 */
#include <stdbool.h>
#include <string.h>
#include "server.h"
#include "multiplayer_protocol.h"
#include "utils/mem/mem.h"
#include "utils/time/time.h"
#include "epnet.h"
#include "epnet_server.h"
#include <stdio.h> // TODO: Remove.

typedef struct Player
{
    MpPlayer player;
    bool is_connected;
    unsigned long user_sync_updatd_time_ms;
    unsigned long actor_sync_updated_time_ms;
} Player;

typedef struct MpServer
{
    epnet_server_t* ns;
    MpServerConfiguration server_configuration;
    unsigned long tick_time_ms;
    unsigned long prev_tick_time_ms;
    Player* players;
    unsigned long prev_users_sync_sent_time_ms;
    unsigned long prev_actors_sync_sent_time_ms;
} MpServer;


/**
 * @brief process connection request packet.
 *
 * @param server A pointer to server instance.
 * @param sender Sender client id.
 * @param packet A pointer to connection request packet.
 */
static void process_connection_request_(MpServer* server, uint8_t sender,
                                        MpCPacketConnectionRequest* packet);

/**
 * @brief Process events received since last tick.
 *
 * @param server A pointer to server instance.
 */
static void process_received_packets_(MpServer* server);


/**
 * @brief Builds and sends users sync packets to all connected clients.
 *
 * @param server A pointer to server instance.
 * Each client will receive user data about other connected clients.
 */
static void send_users_sync_(MpServer* server);

/**
 * @brief Builds and sends actors sync packets to all connected clients.
 *
 * Each client will receive information about other connected clients.
 */
static void send_actors_sync_(MpServer* server);


static void process_connection_request_(MpServer* server, uint8_t sender,
                                        MpCPacketConnectionRequest* packet)
{
    printf("Connection request from %d\n", sender);
    /* Store player name */
    strncpy_s(server->players[sender].player.mp_user.name, MP_MAX_NAME_LEN + 1,
              packet->name, MP_MAX_NAME_LEN);
    /* Mark as connected */
    server->players[sender].is_connected = true;
    /* Send connection response */
    MpSPacketConnectionResponse response;
    response.server_configuration = server->server_configuration;
    epnet_server_send(server->ns, sender, MPT_S_CONNECTION_RESPONSE, &response,
                      sizeof(response));
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
            printf("Client %d joined (low-level)\n", ev.client_id);
            break;
        }
        case EPNET_SRV_EVENT_CLIENT_LEAVE:
        {
            printf("Client %d left\n", ev.client_id);
            if (ev.client_id < server->server_configuration.max_clients)
            {
                server->players[ev.client_id].is_connected = false;
            }
            break;
        }
        case EPNET_SRV_EVENT_PACKET:
        {
            uint8_t sender = ev.client_id;
            switch (ev.data.packet.pkt_type)
            {
            case MPT_C_CONNECTION_REQUEST:
            {
                process_connection_request_(
                    server, sender,
                    (MpCPacketConnectionRequest*)ev.data.packet.data);
                break;
            }
            case MPT_C_USER_SYNC:
            {
                server->players[sender].player.mp_user =
                    ((MpCPacketUserSync*)ev.data.packet.data)->mp_user;
                server->players[sender].user_sync_updatd_time_ms =
                    server->tick_time_ms;
                break;
            }
            case MPT_C_ACTOR_SYNC:
            {
                server->players[sender].player.mp_actor =
                    ((MpCPacketActorSync*)ev.data.packet.data)->mp_actor;
                server->players[sender].actor_sync_updated_time_ms =
                    server->tick_time_ms;
                break;
            }
            case MPT_C_SHOOT:
            {
                /* Build and send shoot packet to all other clients */
                MpSPacketShoot msps;
                msps.player_id = sender;
                msps.x = ((MpCPacketShoot*)ev.data.packet.data)->x;
                msps.y = ((MpCPacketShoot*)ev.data.packet.data)->y;
                for (uint8_t i = 0;
                     i < server->server_configuration.max_clients; i++)
                {
                    if (i != sender && server->players[i].is_connected)
                    {
                        epnet_server_send(server->ns, i, MPT_S_SHOOT, &msps,
                                          sizeof(msps));
                    }
                }
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

static void send_users_sync_(MpServer* server)
{
    uint8_t buf[1024]; // TODO: Use precalculated size.
    MpSPacketUsersSync* packet = (MpSPacketUsersSync*)buf;

    for (uint8_t destination = 0;
         destination < server->server_configuration.max_clients; destination++)
    {
        /* Build actors info sync packet only for connected players */
        if (!server->players[destination].is_connected)
        {
            continue;
        }

        /* Reset num_items for the player */
        packet->num_items = 0;

        /* Build actors sync packet for player with 'destination' id */
        for (uint8_t i = 0; i < server->server_configuration.max_clients; i++)
        {
            /* Ignore disconnected players */
            if (!server->players[i].is_connected)
            {
                continue;
            }
            /* Ignore player who will receive this packet */
            if (i == destination)
            {
                continue;
            }
            /* Ignore players who did not send user sync yet */
            if (server->players[i].user_sync_updatd_time_ms == 0)
            {
                continue;
            }

            /* Add player's actor info to packet */
            packet->items[packet->num_items].id = i;
            packet->items[packet->num_items].mp_user =
                server->players[i].player.mp_user;
            packet->num_items++;
        }
        /* Send the packet */
        epnet_server_send(
            server->ns, destination, MPT_S_USERS_SYNC, packet,
            sizeof(*packet) + sizeof(packet->items[0]) * packet->num_items);
    }
}

static void send_actors_sync_(MpServer* server)
{
    uint8_t buf[1024]; // TODO: Use precalculated size.
    MpSPacketActorsSync* packet = (MpSPacketActorsSync*)buf;

    for (uint8_t destination = 0;
         destination < server->server_configuration.max_clients; destination++)
    {
        /* Build actors info sync packet only for connected players */
        if (!server->players[destination].is_connected)
        {
            continue;
        }

        /* Reset num_items for the player */
        packet->num_items = 0;

        /* Build actors sync packet for player with 'destination' id */
        for (uint8_t i = 0; i < server->server_configuration.max_clients; i++)
        {
            /* Ignore disconnected players */
            if (!server->players[i].is_connected)
            {
                continue;
            }
            /* Ignore player who will receive this packet */
            if (i == destination)
            {
                continue;
            }
            /* Ignore players who did not send actor sync yet */
            if (server->players[i].actor_sync_updated_time_ms == 0)
            {
                continue;
            }

            /* Add player's actor info to packet */
            packet->items[packet->num_items].id = i;
            packet->items[packet->num_items].mp_actor =
                server->players[i].player.mp_actor;
            packet->num_items++;
        }
        /* Send the packet */
        epnet_server_send(
            server->ns, destination, MPT_S_ACTORS_SYNC, packet,
            sizeof(*packet) + sizeof(packet->items[0]) * packet->num_items);
    }
}

MpServer* mp_server_create(unsigned short port, int max_clients)
{
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
                server->server_configuration.max_clients = max_clients;
                strcpy_s(server->server_configuration.map_name,
                         MP_MAX_MAP_NAME_LEN, "maps\\Level_01.map");
                server->server_configuration.user_sync_update_rate_ms =
                    MP_USER_SYNC_UPDATE_RATE_MS;
                server->server_configuration.actor_sync_update_rate_ms =
                    MP_ACTOR_SYNC_UPDATE_RATE_MS;
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

    if ((server->tick_time_ms - server->prev_users_sync_sent_time_ms) >=
        server->server_configuration.user_sync_update_rate_ms)
    {
        send_users_sync_(server);
        server->prev_users_sync_sent_time_ms = server->tick_time_ms;
    }

    if ((server->tick_time_ms - server->prev_actors_sync_sent_time_ms) >=
        server->server_configuration.actor_sync_update_rate_ms)
    {
        send_actors_sync_(server);
        server->prev_actors_sync_sent_time_ms = server->tick_time_ms;
    }
}
