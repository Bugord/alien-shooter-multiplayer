/**
 * @file client.c
 * @brief Top level client-side multiplayer logic implementation.
 *
 */
#include <string.h>
#include "client.h"
#include "utils/mem/mem.h"
#include "utils/time/time.h"
#include "utils/console/console.h" // TODO: Remove.
#include "epnet.h"
#include "epnet_client.h"


/**
 * @brief Substates related to connection state.
 */
typedef enum MpClientStateConnectedSubstate
{
    STATE_CONNECTING_SUBSTATE_IDLE = 0,
    STATE_CONNECTING_SUBSTATE_CONNECTING,
    STATE_CONNECTING_SUBSTATE_CONNECTED,
    STATE_CONNECTING_SUBSTATE_TOP_LEVEL_CONNECTING,
    STATE_CONNECTING_SUBSTATE_TOP_LEVEL_JUST_CONNECTED,
} MpClientStateConnectedSubstate;


typedef struct Player
{
    MpPlayer mp_player;
    unsigned long user_sync_updated_time_ms;
    unsigned long actor_sync_updated_time_ms;
} Player;

typedef struct MpClient
{
    MpClientState state;
    epnet_client_t* nc;
    unsigned long tick_time_ms;
    unsigned long prev_tick_time_ms;
    MpServerConfiguration server_configuration;
    Player local_player;
    void (*user_sync_callback)(MpClient* client, int id, MpUser* user);
    void (*actor_sync_callback)(MpClient* client, int id, MpActor* actor);
    void (*actor_shoot_callback)(MpClient* client, int id, float x, float y);
} MpClient;


/**
 * @brief Sends top-level client connection request and switches to the next
 *        substate.
 *
 * @param client Pointer to client instance.
 */
static void handle_state_connecting_substate_connected_(MpClient* client);

/**
 * @brief Waits for top-level connection response.
 *
 * @param client Pointer to client instance.
 */
static bool handle_state_connecting_substate_top_level_connecting_(
    MpClient* client);

/**
 * @brief Handles connecting state.
 *
 * @param client Pointer to client instance.
 */
static void handle_state_connecting_(MpClient* client);

/**
 * @brief Handles connected state.
 *
 * @param client Pointer to client instance.
 */
static void handle_state_connected_(MpClient* client);

/**
 * @brief Processes events received since last tick.
 *
 * @param client Pointer to client instance.
 */
static void process_received_packets_(MpClient* client);


static void handle_state_connecting_substate_connected_(MpClient* client)
{
    /* Build top-level client connection request */
    uint8_t buf[sizeof(MpCPacketConnectionRequest) + MP_MAX_NAME_LEN];
    MpCPacketConnectionRequest* packet = (MpCPacketConnectionRequest*)buf;
    packet->name_len = strlen(client->local_player.mp_player.mp_user.name);
    strncpy_s(packet->name, MP_MAX_NAME_LEN,
              client->local_player.mp_player.mp_user.name, MP_MAX_NAME_LEN);
    epnet_client_send(client->nc, MPT_C_CONNECTION_REQUEST, buf,
                      sizeof(*packet) + packet->name_len);
}

static bool handle_state_connecting_substate_top_level_connecting_(
    MpClient* client)
{
    epnet_event_t ev;
    while (epnet_client_poll_events(client->nc, &ev))
    {
        if (ev.type == EPNET_EVENT_PACKET &&
            ev.data.packet.pkt_type == MPT_S_CONNECTION_RESPONSE)
        {
            if (ev.data.packet.len != (int)sizeof(MpSPacketConnectionResponse))
            {
                continue;
            }
            MpSPacketConnectionResponse* response =
                (MpSPacketConnectionResponse*)ev.data.packet.data;
            client->server_configuration = response->server_configuration;
            return true;
        }
        if (ev.type == EPNET_EVENT_DISCONNECTED)
        {
            client->state = MP_CLIENT_STATE_CONNECTION_FAILED;
            return false;
        }
    }
    return false;
}

static void handle_state_connecting_(MpClient* client)
{
    static MpClientStateConnectedSubstate connecting_substate =
        STATE_CONNECTING_SUBSTATE_IDLE;

    switch (connecting_substate)
    {
    case STATE_CONNECTING_SUBSTATE_IDLE:
    {
        connecting_substate = STATE_CONNECTING_SUBSTATE_CONNECTING;
        break;
    }
    case STATE_CONNECTING_SUBSTATE_CONNECTING:
    {
        epnet_event_t ev;
        while (epnet_client_poll_events(client->nc, &ev))
        {
            if (ev.type == EPNET_EVENT_CONNECTED)
            {
                connecting_substate = STATE_CONNECTING_SUBSTATE_CONNECTED;
                return;
            }
            if (ev.type == EPNET_EVENT_DISCONNECTED)
            {
                connecting_substate = STATE_CONNECTING_SUBSTATE_IDLE;
                client->state = MP_CLIENT_STATE_CONNECTION_FAILED;
                return;
            }
        }
        break;
    }
    case STATE_CONNECTING_SUBSTATE_CONNECTED:
    {
        /* Send top level connection request */
        handle_state_connecting_substate_connected_(client);
        connecting_substate = STATE_CONNECTING_SUBSTATE_TOP_LEVEL_CONNECTING;
        break;
    }
    case STATE_CONNECTING_SUBSTATE_TOP_LEVEL_CONNECTING:
    {
        /* Wait for top level connection response */
        if (handle_state_connecting_substate_top_level_connecting_(client))
        {
            connecting_substate =
                STATE_CONNECTING_SUBSTATE_TOP_LEVEL_JUST_CONNECTED;
        }
        break;
    }
    case STATE_CONNECTING_SUBSTATE_TOP_LEVEL_JUST_CONNECTED:
    {
        /* Reset connecting substate for future calls */
        connecting_substate = STATE_CONNECTING_SUBSTATE_IDLE;
        /* Transit to connected state */
        client->state = MP_CLIENT_STATE_CONNECTED;
        break;
    }
    }
}

static void handle_state_connected_(MpClient* client)
{
    if ((client->tick_time_ms -
         client->local_player.user_sync_updated_time_ms) >=
        client->server_configuration.user_sync_update_rate_ms)
    {
        /* Send actual user info */
        MpCPacketUserSync packet;
        packet.mp_user = client->local_player.mp_player.mp_user;
        epnet_client_send(client->nc, MPT_C_USER_SYNC, &packet,
                          sizeof(packet));
        client->local_player.user_sync_updated_time_ms = client->tick_time_ms;
    }

    if ((client->tick_time_ms -
         client->local_player.actor_sync_updated_time_ms) >=
        client->server_configuration.actor_sync_update_rate_ms)
    {
        /* Send actual actor info */
        MpCPacketActorSync packet;
        mem_copy(&packet.mp_actor, &client->local_player.mp_player.mp_actor,
                 sizeof(packet.mp_actor));
        epnet_client_send(client->nc, MPT_C_ACTOR_SYNC, &packet,
                          sizeof(packet));
        client->local_player.actor_sync_updated_time_ms = client->tick_time_ms;
    }
    process_received_packets_(client);
}

static void process_received_packets_(MpClient* client)
{
    epnet_event_t ev;
    while (epnet_client_poll_events(client->nc, &ev))
    {
        if (ev.type == EPNET_EVENT_DISCONNECTED)
        {
            client->state = MP_CLIENT_STATE_INITED;
            return;
        }
        if (ev.type != EPNET_EVENT_PACKET)
        {
            continue;
        }
        switch (ev.data.packet.pkt_type)
        {
        case MPT_S_USERS_SYNC:
        {
            MpSPacketUsersSync* mp_packet =
                (MpSPacketUsersSync*)ev.data.packet.data;
            for (uint8_t i = 0; i < mp_packet->num_items; ++i)
            {
                client->user_sync_callback(client, mp_packet->items[i].id,
                                           &mp_packet->items[i].mp_user);
            }
            break;
        }
        case MPT_S_ACTORS_SYNC:
        {
            MpSPacketActorsSync* mp_packet =
                (MpSPacketActorsSync*)ev.data.packet.data;
            for (uint8_t i = 0; i < mp_packet->num_items; ++i)
            {
                client->actor_sync_callback(client, mp_packet->items[i].id,
                                            &mp_packet->items[i].mp_actor);
            }
            break;
        }
        case MPT_S_SHOOT:
        {
            MpSPacketShoot* shoot = (MpSPacketShoot*)ev.data.packet.data;
            client->actor_shoot_callback(client, shoot->player_id, shoot->x,
                                         shoot->y);
            break;
        }
        default:
        {
            break;
        }
        }
    }
}

MpClient* mp_client_create(void)
{
    MpClient* client = mem_alloc(sizeof(*client));
    if (client)
    {
        mem_set(client, 0, sizeof(*client));
        epnet_init();
        client->nc = epnet_client_create();
        if (client->nc)
        {
            client->prev_tick_time_ms = time_get_ms();
            return client;
        }
        epnet_shutdown();
        mem_free(client);
    }
    return 0;
}

void mp_client_destroy(MpClient* client)
{
    if (client)
    {
        if (client->nc)
        {
            epnet_client_destroy(client->nc);
        }
        epnet_shutdown();
        mem_free(client);
    }
}

void mp_client_tick(MpClient* client)
{
    if (!client)
    {
        return;
    }
    client->tick_time_ms = time_get_ms();
    double dt = (client->tick_time_ms - client->prev_tick_time_ms) / 1000.0;
    client->prev_tick_time_ms = client->tick_time_ms;
    epnet_client_update(client->nc, dt);
    switch (client->state)
    {
    case MP_CLIENT_STATE_UNINITED:
    case MP_CLIENT_STATE_INITED:
    case MP_CLIENT_STATE_CONNECTION_FAILED:
    case MP_CLIENT_STATE_DISCONNECTING:
    {
        break;
    }
    case MP_CLIENT_STATE_CONNECTING:
    {
        handle_state_connecting_(client);
        break;
    }
    case MP_CLIENT_STATE_CONNECTED:
    {
        handle_state_connected_(client);
        break;
    }
    }
}

bool mp_client_connection_request(MpClient* client, const char* ip,
                                  uint16_t port, const char* name)
{
    /* Check args */
    if (!client || !name || !ip)
    {
        return false;
    }
    /* Check is current client state allows to connect */
    if (client->state != MP_CLIENT_STATE_INITED &&
        client->state != MP_CLIENT_STATE_CONNECTION_FAILED)
    {
        return false;
    }
    /* Store user name */
    if (strncpy_s(client->local_player.mp_player.mp_user.name,
                  MP_MAX_NAME_LEN + 1, name, MP_MAX_NAME_LEN) != 0)
    {
        return false;
    }
    /* Send connection request */
    if (epnet_client_connect(client->nc, ip, port) != 0)
    {
        return false;
    }
    client->state = MP_CLIENT_STATE_CONNECTING;
    return true;
}

void mp_client_disconnect(MpClient* client)
{
    if (!client)
    {
        return;
    }
    epnet_client_disconnect(client->nc);
    client->state = MP_CLIENT_STATE_INITED;
}

void mp_client_set_user_sync_callback(MpClient* client,
                                      void (*callback)(MpClient* client, int id,
                                                       MpUser* user))
{
    if (client)
    {
        client->user_sync_callback = callback;
    }
}

void mp_client_set_actor_sync_callback(MpClient* client,
                                       void (*callback)(MpClient* client,
                                                        int id, MpActor* actor))
{
    if (client)
    {
        client->actor_sync_callback = callback;
    }
}

void mp_client_set_actor_shoot_callback(MpClient* client,
                                        void (*callback)(MpClient* client,
                                                         int id, float x,
                                                         float y))
{
    if (client)
    {
        client->actor_shoot_callback = callback;
    }
}

void mp_client_send_shoot(MpClient* client, float x, float y)
{
    if (client)
    {
        MpCPacketShoot packet;
        packet.x = x;
        packet.y = y;
        epnet_client_send(client->nc, MPT_C_SHOOT, &packet, sizeof(packet));
    }
}

MpClientState mp_client_get_state(MpClient* client)
{
    if (!client)
    {
        return MP_CLIENT_STATE_UNINITED;
    }
    return client->state;
}

const MpServerConfiguration* mp_client_get_server_configuration(
    MpClient* client)
{
    if (!client)
    {
        return 0;
    }
    return &client->server_configuration;
}

int mp_client_get_max_players_number(MpClient* client)
{
    if (!client)
    {
        return 0;
    }
    return client->server_configuration.max_clients;
}

MpPlayer* mp_client_get_local_player(MpClient* client)
{
    if (!client)
    {
        return 0;
    }
    return &client->local_player.mp_player;
}
