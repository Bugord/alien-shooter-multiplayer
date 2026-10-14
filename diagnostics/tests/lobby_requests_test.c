/* Checks for asmp-dll/src/multiplayer/lobby_requests.c: ready, start and loaded
   requests are sent only when the snapshot does not show them yet, repeated every
   LOBBY_REQUEST_REPEAT_MS, dropped when stale, and reset with a new connection. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../asmp-dll/src/multiplayer/lobby_requests.h"

#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define HOST_ID  0u
#define GUEST_ID 1u

static int readies, starts, loadeds;
static MpLobbyReady last_ready;
static MpLobbyStart last_start;
static MpLobbyLoaded last_loaded;

static int send_ready(void* context, const MpLobbyReady* request)
{
    (void)context;
    last_ready = *request;
    ++readies;
    return 1;
}

static int send_start(void* context, const MpLobbyStart* request)
{
    (void)context;
    last_start = *request;
    ++starts;
    return 1;
}

static int send_loaded(void* context, const MpLobbyLoaded* request)
{
    (void)context;
    last_loaded = *request;
    ++loadeds;
    return 1;
}

static const LobbySender sender = {NULL, send_ready, send_start, send_loaded};

static MpLobby make_room(uint32_t phase)
{
    MpLobby room;
    memset(&room, 0, sizeof(room));
    room.room = 9u;
    room.revision = 4u;
    room.settings_revision = 2u;
    room.phase = phase;
    room.host_id = HOST_ID;
    room.host_session = 1u;
    room.run_epoch = phase == MP_LOBBY_OPEN ? 0u : 3u;
    room.count = 2u;
    room.entries[0].id = HOST_ID;
    room.entries[0].session = 1u;
    room.entries[1].id = GUEST_ID;
    room.entries[1].session = 2u;
    if (phase == MP_LOBBY_STARTING)
    {
        room.entries[0].flags = MP_LOBBY_ENTRY_PARTICIPANT;
        room.entries[1].flags = MP_LOBBY_ENTRY_PARTICIPANT;
    }
    return room;
}

static void check_ready(void)
{
    LobbyRequests requests;
    memset(&requests, 0, sizeof(requests));
    LobbyIntent intent = {1u, 1, 1u, 2u, 0, 0u, 0u, 0u, 0u};
    MpLobby room = make_room(MP_LOBBY_OPEN);

    CHECK(lobby_requests_step(&requests, &intent, &room, GUEST_ID, 1000, &sender) == 1u);
    CHECK(last_ready.ready == 1u && last_ready.room == 9u && last_ready.settings_revision == 2u);
    CHECK(lobby_requests_step(&requests, &intent, &room, GUEST_ID, 1499, &sender) == 0u);
    CHECK(lobby_requests_step(&requests, &intent, &room, GUEST_ID, 1500, &sender) == 1u);
    CHECK(readies == 2);

    room.entries[1].flags = MP_LOBBY_ENTRY_READY; /* Shown: stop sending. */
    CHECK(lobby_requests_step(&requests, &intent, &room, GUEST_ID, 3000, &sender) == 0u);

    room.entries[1].flags = 0;
    room.settings_revision = 3u; /* Settings changed after the click: agree again. */
    CHECK(lobby_requests_step(&requests, &intent, &room, GUEST_ID, 4000, &sender) == 0u);

    room.settings_revision = 2u;
    CHECK(lobby_requests_step(&requests, &intent, &room, HOST_ID, 5000, &sender) == 0u);
    CHECK(readies == 2); /* The host never sends ready. */
}

static void check_start(void)
{
    LobbyRequests requests;
    memset(&requests, 0, sizeof(requests));
    LobbyIntent intent = {2u, 0, 0u, 0u, 1, 9u, 4u, 0u, 0u};
    MpLobby room = make_room(MP_LOBBY_OPEN);

    CHECK(lobby_requests_step(&requests, &intent, &room, GUEST_ID, 1000, &sender) == 0u);
    CHECK(lobby_requests_step(&requests, &intent, &room, HOST_ID, 1000, &sender) == 1u);
    CHECK(last_start.room == 9u && last_start.revision == 4u);
    CHECK(lobby_requests_step(&requests, &intent, &room, HOST_ID, 1200, &sender) == 0u);
    room.revision = 5u; /* The room changed: the old start is stale. */
    CHECK(lobby_requests_step(&requests, &intent, &room, HOST_ID, 2000, &sender) == 0u);
    room = make_room(MP_LOBBY_STARTING);
    room.revision = 4u;
    CHECK(lobby_requests_step(&requests, &intent, &room, HOST_ID, 3000, &sender) == 0u);
    CHECK(starts == 1);
}

static void check_loaded(void)
{
    LobbyRequests requests;
    memset(&requests, 0, sizeof(requests));
    LobbyIntent intent = {3u, 0, 0u, 0u, 0, 0u, 0u, 9u, 3u};
    MpLobby room = make_room(MP_LOBBY_STARTING);

    CHECK(lobby_requests_step(&requests, &intent, &room, GUEST_ID, 1000, &sender) == 1u);
    CHECK(last_loaded.room == 9u && last_loaded.run_epoch == 3u);
    CHECK(lobby_requests_step(&requests, &intent, &room, GUEST_ID, 1500, &sender) == 1u);
    room.entries[1].flags |= MP_LOBBY_ENTRY_LOADED;
    CHECK(lobby_requests_step(&requests, &intent, &room, GUEST_ID, 2000, &sender) == 0u);

    room.entries[1].flags = 0; /* A late joiner is no participant. */
    CHECK(lobby_requests_step(&requests, &intent, &room, GUEST_ID, 3000, &sender) == 0u);
    room.entries[1].flags = MP_LOBBY_ENTRY_PARTICIPANT;
    room.run_epoch = 4u; /* Another start. */
    CHECK(lobby_requests_step(&requests, &intent, &room, GUEST_ID, 4000, &sender) == 0u);
    CHECK(loadeds == 2);

    /* A new connection restarts the repeat timers. */
    room.run_epoch = 3u;
    intent.generation = 4u;
    CHECK(lobby_requests_step(&requests, &intent, &room, GUEST_ID, 4001, &sender) == 1u);
    CHECK(requests.generation == 4u);

    /* An id missing from the room sends nothing. */
    CHECK(lobby_requests_step(&requests, &intent, &room, 7u, 9000, &sender) == 0u);
}

int main(void)
{
    check_ready();
    check_start();
    check_loaded();
    puts("Lobby request checks passed: ready, start and loaded repeat until shown, stale "
         "settings or revision drop, host/participant rules, new connection reset.");
    return 0;
}
