/* Lobby over real localhost UDP: the relay server (asmp-server/src/server.c) and two
   network clients (state_client.c through transport_udp.c) go through join, ready,
   start, the load barrier and the host leaving; a third client checks "no answer". */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../asmp-dll/src/multiplayer/client/state_client.h"
#include "../../asmp-server/src/server.h"

#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

/* See state_sync_test.c: Windows may reserve blocks of UDP ports. */
#define TEST_PORT_ATTEMPTS 16
#define TEST_PORT_STEP     101
#define WAIT_LIMIT_MS      3000u
#define NO_ANSWER_LIMIT_MS 9000u

static MpServer* create_test_server(unsigned short first_port, unsigned short* port)
{
    for (unsigned int attempt = 0; attempt < TEST_PORT_ATTEMPTS; ++attempt)
    {
        *port = (unsigned short)(first_port + attempt * TEST_PORT_STEP);
        MpServer* server = mp_server_create(*port, 4);
        if (server)
            return server;
    }
    return NULL;
}

typedef int (*Condition)(StateClient* host, StateClient* guest);

/* Pumps server and clients until `done` holds; returns 0 after WAIT_LIMIT_MS. */
static int pump_until(MpServer* server, StateClient* host, StateClient* guest, Condition done)
{
    DWORD start = GetTickCount();
    while (GetTickCount() - start < WAIT_LIMIT_MS)
    {
        DWORD now = GetTickCount();
        state_client_update(host, now);
        state_client_update(guest, now);
        mp_server_tick(server);
        if (done(host, guest))
            return 1;
        Sleep(1);
    }
    return 0;
}

static MpLobby lobby_of(StateClient* client)
{
    MpLobby lobby;
    memset(&lobby, 0, sizeof(lobby));
    state_client_lobby(client, &lobby, NULL);
    return lobby;
}

static int both_in_room(StateClient* host, StateClient* guest)
{
    return lobby_of(host).count == 2u && lobby_of(guest).count == 2u;
}

static int guest_ready_seen(StateClient* host, StateClient* guest)
{
    (void)guest;
    MpLobby lobby = lobby_of(host);
    const MpLobbyEntry* entry = mp_lobby_entry(&lobby, 1u);
    return entry && (entry->flags & MP_LOBBY_ENTRY_READY);
}

static int both_starting(StateClient* host, StateClient* guest)
{
    return lobby_of(host).phase == MP_LOBBY_STARTING && lobby_of(guest).phase == MP_LOBBY_STARTING;
}

static int both_in_game(StateClient* host, StateClient* guest)
{
    return lobby_of(host).phase == MP_LOBBY_IN_GAME && lobby_of(guest).phase == MP_LOBBY_IN_GAME;
}

static int guest_sees_closed(StateClient* host, StateClient* guest)
{
    (void)host;
    return lobby_of(guest).phase == MP_LOBBY_CLOSED;
}

static void send_loaded(StateClient* client)
{
    MpLobby lobby = lobby_of(client);
    MpLobbyLoaded loaded = {lobby.room, lobby.run_epoch};
    CHECK(state_client_send_lobby_loaded(client, &loaded));
}

static void check_room_flow(MpServer* server, unsigned short port)
{
    StateClient* host = state_client_create("127.0.0.1", port, "Host", NULL);
    CHECK(host != NULL);
    /* Let the host register first so it owns the room. */
    DWORD start = GetTickCount();
    while (!state_client_lobby(host, NULL, NULL) && GetTickCount() - start < WAIT_LIMIT_MS)
    {
        state_client_update(host, GetTickCount());
        mp_server_tick(server);
        Sleep(1);
    }

    StateClient* guest = state_client_create("127.0.0.1", port, "Guest", NULL);
    CHECK(guest != NULL);
    CHECK(pump_until(server, host, guest, both_in_room));

    MpLobby lobby;
    uint32_t host_id = UINT32_MAX;
    uint32_t guest_id = UINT32_MAX;
    CHECK(state_client_lobby(host, &lobby, &host_id) && state_client_lobby(guest, NULL, &guest_id));
    CHECK(lobby.host_id == host_id && host_id == 0u && guest_id == 1u);
    CHECK(lobby.phase == MP_LOBBY_OPEN && lobby.compat == MP_COMPAT_ID);
    CHECK(state_client_link(guest) == LINK_READY);

    MpLobbyReady ready = {lobby.room, lobby.settings_revision, 1u};
    CHECK(state_client_send_lobby_ready(guest, &ready));
    CHECK(pump_until(server, host, guest, guest_ready_seen));

    lobby = lobby_of(host);
    MpLobbyStart start_request = {lobby.room, lobby.revision};
    CHECK(state_client_send_lobby_start(host, &start_request));
    CHECK(state_client_send_lobby_start(host, &start_request)); /* Duplicate. */
    CHECK(pump_until(server, host, guest, both_starting));
    CHECK(lobby_of(guest).run_epoch == 1u);

    send_loaded(host);
    send_loaded(guest);
    CHECK(pump_until(server, host, guest, both_in_game));
    CHECK(lobby_of(guest).run_epoch == 1u); /* The duplicate did not start twice. */

    state_client_destroy(host);
    CHECK(pump_until(server, NULL, guest, guest_sees_closed));
    state_client_destroy(guest);
}

/* A port with no server never answers; the link reports it distinctly. */
static void check_no_answer(unsigned short unused_port)
{
    StateClient* lonely = state_client_create("127.0.0.1", unused_port, "Lonely", NULL);
    CHECK(lonely != NULL && state_client_link(lonely) == LINK_CONNECTING);
    DWORD start = GetTickCount();
    while (state_client_link(lonely) == LINK_CONNECTING &&
           GetTickCount() - start < NO_ANSWER_LIMIT_MS)
    {
        state_client_update(lonely, GetTickCount());
        Sleep(5);
    }
    CHECK(state_client_link(lonely) == LINK_NO_ANSWER);
    CHECK(!state_client_lobby(lonely, NULL, NULL));
    state_client_destroy(lonely);
}

int main(void)
{
    unsigned short port = 0;
    MpServer* server =
        create_test_server((unsigned short)(50000 + GetCurrentProcessId() % 4000), &port);
    CHECK(server != NULL);
    check_room_flow(server, port);
    mp_server_destroy(server);
    check_no_answer(port);
    puts("Lobby transport checks passed: host ownership, ready, duplicate start, load barrier, "
         "host leave closes the room, no-answer link.");
    return 0;
}
