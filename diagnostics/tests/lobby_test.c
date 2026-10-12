/* Unit checks for the lobby: wire codecs (common/src/lobby_protocol.h), the server room
   state machine (asmp-server/src/lobby.c) and the exclusive port check of the relay. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lobby_protocol.h"
#include "../../asmp-server/src/lobby.h"
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

#define HOST_ID      0u
#define HOST_SESSION 11u
#define GUEST_ID     1u
#define GUEST_SESS   21u
#define THIRD_ID     2u
#define THIRD_SESS   31u
#define LEVEL_MAP    "maps\\Level_01.map"
#define SURVIVE_MAP  "maps\\survive_01.map"

static MpLobby snapshot_of(const Lobby* lobby)
{
    MpLobby snapshot;
    lobby_snapshot(lobby, &snapshot);
    return snapshot;
}

static uint32_t flags_of(const Lobby* lobby, uint32_t id)
{
    MpLobby snapshot = snapshot_of(lobby);
    const MpLobbyEntry* entry = mp_lobby_entry(&snapshot, id);
    CHECK(entry != NULL);
    return entry->flags;
}

/* Encodes the current snapshot and decodes it again; both must agree. */
static void check_round_trip(const Lobby* lobby)
{
    MpLobby original = snapshot_of(lobby);
    uint8_t packet[MP_LOBBY_MAX_SIZE];
    int length = mp_lobby_encode(packet, sizeof(packet), &original);
    CHECK(length > 0);

    MpLobby decoded;
    CHECK(mp_lobby_decode(packet, length, &decoded));
    CHECK(decoded.room == original.room && decoded.revision == original.revision);
    CHECK(decoded.phase == original.phase && decoded.count == original.count);
    CHECK(decoded.compat == MP_COMPAT_ID && !strcmp(decoded.map, original.map));
    for (unsigned int i = 0; i < decoded.count; ++i)
    {
        CHECK(decoded.entries[i].id == original.entries[i].id);
        CHECK(decoded.entries[i].flags == original.entries[i].flags);
        CHECK(!strcmp(decoded.entries[i].name, original.entries[i].name));
    }

    /* Any truncation or trailing byte is rejected. */
    CHECK(!mp_lobby_decode(packet, length - 1, &decoded));
    if (length < (int)sizeof(packet))
    {
        packet[length] = 0;
        CHECK(!mp_lobby_decode(packet, length + 1, &decoded));
    }
}

static void check_request_codecs(void)
{
    uint8_t packet[64];

    MpLobbyReady ready = {7u, 3u, 1u};
    MpLobbyReady ready_out;
    mp_lobby_ready_encode(packet, &ready);
    CHECK(mp_lobby_ready_decode(packet, MP_LOBBY_REQUEST_SIZE, &ready_out));
    CHECK(ready_out.room == 7u && ready_out.settings_revision == 3u && ready_out.ready == 1u);
    CHECK(!mp_lobby_ready_decode(packet, MP_LOBBY_REQUEST_SIZE - 1, &ready_out));
    packet[15] = 2; /* ready must be 0 or 1 */
    CHECK(!mp_lobby_ready_decode(packet, MP_LOBBY_REQUEST_SIZE, &ready_out));

    MpLobbyStart start = {7u, 9u};
    MpLobbyStart start_out;
    mp_lobby_start_encode(packet, &start);
    CHECK(mp_lobby_start_decode(packet, MP_LOBBY_START_SIZE, &start_out));
    CHECK(start_out.room == 7u && start_out.revision == 9u);

    MpLobbyLoaded loaded = {7u, 2u};
    MpLobbyLoaded loaded_out;
    mp_lobby_loaded_encode(packet, &loaded);
    CHECK(mp_lobby_loaded_decode(packet, MP_LOBBY_LOADED_SIZE, &loaded_out));
    CHECK(loaded_out.run_epoch == 2u);
    loaded.run_epoch = 0; /* No start has epoch 0. */
    mp_lobby_loaded_encode(packet, &loaded);
    CHECK(!mp_lobby_loaded_decode(packet, MP_LOBBY_LOADED_SIZE, &loaded_out));

    MpLobbySettings settings = {7u, 1u, SURVIVE_MAP};
    MpLobbySettings settings_out;
    int length = mp_lobby_settings_encode(packet, sizeof(packet), &settings);
    CHECK(length > 0 && mp_lobby_settings_decode(packet, length, &settings_out));
    CHECK(!strcmp(settings_out.map, SURVIVE_MAP));
    strcpy_s(settings.map, sizeof(settings.map), "maps\\mainmenu.map");
    CHECK(!mp_lobby_settings_encode(packet, sizeof(packet), &settings));

    CHECK(mp_mode_for_map(LEVEL_MAP) == MP_MODE_CAMPAIGN);
    CHECK(mp_mode_for_map("maps/SURVIVE_02.MAP") == MP_MODE_SURVIVAL);
    CHECK(!mp_mode_for_map("maps\\mainmenu.map"));
}

/* Hand-made snapshots that break one rule each must not validate. */
static void check_invalid_snapshots(const Lobby* lobby)
{
    MpLobby bad = snapshot_of(lobby);
    bad.host_session = 999u; /* Host entry exists, session differs. */
    CHECK(!mp_lobby_valid(&bad));

    bad = snapshot_of(lobby);
    bad.entries[0].flags = MP_LOBBY_ENTRY_PARTICIPANT; /* Participant in an open room. */
    CHECK(!mp_lobby_valid(&bad));

    bad = snapshot_of(lobby);
    bad.entries[1].id = bad.entries[0].id; /* Duplicate id. */
    CHECK(!mp_lobby_valid(&bad));

    bad = snapshot_of(lobby);
    bad.mode = MP_MODE_SURVIVAL; /* Mode does not match the level map. */
    CHECK(!mp_lobby_valid(&bad));

    bad = snapshot_of(lobby);
    bad.phase = 9u;
    CHECK(!mp_lobby_valid(&bad));
}

static void join_two(Lobby* lobby)
{
    CHECK(lobby_init(lobby, 1234u, 4u, LEVEL_MAP));
    CHECK(lobby_join(lobby, HOST_ID, HOST_SESSION, "Host") == LOBBY_CHANGED);
    CHECK(lobby_join(lobby, GUEST_ID, GUEST_SESS, "Guest") == LOBBY_CHANGED);
}

static void set_guest_ready(Lobby* lobby, uint32_t ready)
{
    MpLobbyReady request = {lobby->room, lobby->settings_revision, ready};
    CHECK(lobby_set_ready(lobby, GUEST_ID, GUEST_SESS, &request) == LOBBY_CHANGED);
}

static enum LobbyResult host_start(Lobby* lobby, unsigned long now)
{
    MpLobbyStart request = {lobby->room, lobby->revision};
    return lobby_start(lobby, HOST_ID, HOST_SESSION, &request, now);
}

static enum LobbyResult report_loaded(Lobby* lobby, uint32_t id, uint32_t session)
{
    MpLobbyLoaded request = {lobby->room, lobby->run_epoch};
    return lobby_mark_loaded(lobby, id, session, &request);
}

static void check_host_and_ready(void)
{
    Lobby lobby;
    join_two(&lobby);
    MpLobby snapshot = snapshot_of(&lobby);
    CHECK(snapshot.host_id == HOST_ID && snapshot.host_session == HOST_SESSION);
    CHECK(snapshot.phase == MP_LOBBY_OPEN && snapshot.count == 2u);
    CHECK(lobby_join(&lobby, GUEST_ID, GUEST_SESS, "Guest") == LOBBY_UNCHANGED);
    check_round_trip(&lobby);
    check_invalid_snapshots(&lobby);

    /* Start needs every guest ready; only the host may start. */
    CHECK(host_start(&lobby, 100u) == LOBBY_UNCHANGED);
    set_guest_ready(&lobby, 1u);
    CHECK(flags_of(&lobby, GUEST_ID) == MP_LOBBY_ENTRY_READY);
    MpLobbyStart guest_start = {lobby.room, lobby.revision};
    CHECK(lobby_start(&lobby, GUEST_ID, GUEST_SESS, &guest_start, 100u) == LOBBY_UNCHANGED);

    /* A stale ready (old settings revision) and a foreign room are ignored. */
    MpLobbyReady stale = {lobby.room, lobby.settings_revision + 1u, 0u};
    CHECK(lobby_set_ready(&lobby, GUEST_ID, GUEST_SESS, &stale) == LOBBY_UNCHANGED);
    MpLobbyReady foreign = {lobby.room + 1u, lobby.settings_revision, 0u};
    CHECK(lobby_set_ready(&lobby, GUEST_ID, GUEST_SESS, &foreign) == LOBBY_UNCHANGED);

    /* Changing the map is host-only and resets ready. */
    MpLobbySettings settings = {lobby.room, lobby.settings_revision, SURVIVE_MAP};
    CHECK(lobby_change_settings(&lobby, GUEST_ID, GUEST_SESS, &settings) == LOBBY_UNCHANGED);
    CHECK(lobby_change_settings(&lobby, HOST_ID, HOST_SESSION, &settings) == LOBBY_CHANGED);
    snapshot = snapshot_of(&lobby);
    CHECK(snapshot.mode == MP_MODE_SURVIVAL && snapshot.settings_revision == 2u);
    CHECK(flags_of(&lobby, GUEST_ID) == 0u);
    CHECK(lobby_change_settings(&lobby, HOST_ID, HOST_SESSION, &settings) == LOBBY_UNCHANGED);
    check_round_trip(&lobby);
}

static void check_start_barrier(void)
{
    Lobby lobby;
    join_two(&lobby);
    set_guest_ready(&lobby, 1u);

    MpLobbyStart stale = {lobby.room, lobby.revision - 1u};
    CHECK(lobby_start(&lobby, HOST_ID, HOST_SESSION, &stale, 100u) == LOBBY_UNCHANGED);
    MpLobbyStart first = {lobby.room, lobby.revision};
    CHECK(lobby_start(&lobby, HOST_ID, HOST_SESSION, &first, 100u) == LOBBY_CHANGED);
    /* The duplicate of the same start names an outdated revision. */
    CHECK(lobby_start(&lobby, HOST_ID, HOST_SESSION, &first, 101u) == LOBBY_UNCHANGED);

    MpLobby snapshot = snapshot_of(&lobby);
    CHECK(snapshot.phase == MP_LOBBY_STARTING && snapshot.run_epoch == 1u);
    CHECK(flags_of(&lobby, HOST_ID) == MP_LOBBY_ENTRY_PARTICIPANT);
    CHECK(flags_of(&lobby, GUEST_ID) == MP_LOBBY_ENTRY_PARTICIPANT);
    check_round_trip(&lobby);

    /* A late joiner watches but does not hold the barrier. */
    CHECK(lobby_join(&lobby, THIRD_ID, THIRD_SESS, "Late") == LOBBY_CHANGED);
    CHECK(flags_of(&lobby, THIRD_ID) == 0u);
    CHECK(report_loaded(&lobby, THIRD_ID, THIRD_SESS) == LOBBY_UNCHANGED);

    MpLobbyLoaded wrong_epoch = {lobby.room, 2u};
    CHECK(lobby_mark_loaded(&lobby, HOST_ID, HOST_SESSION, &wrong_epoch) == LOBBY_UNCHANGED);
    CHECK(report_loaded(&lobby, HOST_ID, HOST_SESSION) == LOBBY_CHANGED);
    CHECK(report_loaded(&lobby, HOST_ID, HOST_SESSION) == LOBBY_UNCHANGED);
    CHECK(snapshot_of(&lobby).phase == MP_LOBBY_STARTING);
    CHECK(report_loaded(&lobby, GUEST_ID, GUEST_SESS) == LOBBY_CHANGED);
    CHECK(snapshot_of(&lobby).phase == MP_LOBBY_IN_GAME);
    CHECK(lobby_tick(&lobby, 100u + MP_LOBBY_LOAD_TIMEOUT_MS) == LOBBY_UNCHANGED);
    check_round_trip(&lobby);

    /* A participant leaving a running game does not stop it. */
    CHECK(lobby_leave(&lobby, GUEST_ID, GUEST_SESS) == LOBBY_CHANGED);
    CHECK(snapshot_of(&lobby).phase == MP_LOBBY_IN_GAME);
}

static void check_cancellations(void)
{
    Lobby lobby;
    join_two(&lobby);
    set_guest_ready(&lobby, 1u);
    CHECK(host_start(&lobby, 1000u) == LOBBY_CHANGED);
    CHECK(lobby_tick(&lobby, 1000u + MP_LOBBY_LOAD_TIMEOUT_MS - 1u) == LOBBY_UNCHANGED);
    CHECK(lobby_tick(&lobby, 1000u + MP_LOBBY_LOAD_TIMEOUT_MS) == LOBBY_CHANGED);
    MpLobby snapshot = snapshot_of(&lobby);
    CHECK(snapshot.phase == MP_LOBBY_OPEN && snapshot.notice == MP_LOBBY_NOTICE_LOAD_TIMEOUT);
    CHECK(snapshot.run_epoch == 1u && flags_of(&lobby, GUEST_ID) == 0u);
    check_round_trip(&lobby);

    /* A participant leaving while loading cancels the start. */
    set_guest_ready(&lobby, 1u);
    CHECK(snapshot_of(&lobby).notice == MP_LOBBY_NOTICE_NONE);
    CHECK(host_start(&lobby, 2000u) == LOBBY_CHANGED);
    CHECK(snapshot_of(&lobby).run_epoch == 2u);
    CHECK(lobby_leave(&lobby, GUEST_ID, GUEST_SESS) == LOBBY_CHANGED);
    snapshot = snapshot_of(&lobby);
    CHECK(snapshot.phase == MP_LOBBY_OPEN && snapshot.notice == MP_LOBBY_NOTICE_PLAYER_LEFT);

    /* A leave with an old session belongs to an earlier connection. */
    CHECK(lobby_join(&lobby, GUEST_ID, GUEST_SESS + 1u, "Guest") == LOBBY_CHANGED);
    CHECK(lobby_leave(&lobby, GUEST_ID, GUEST_SESS) == LOBBY_UNCHANGED);

    /* The host leaving closes the room for everyone left. */
    CHECK(lobby_leave(&lobby, HOST_ID, HOST_SESSION) == LOBBY_CHANGED);
    snapshot = snapshot_of(&lobby);
    CHECK(snapshot.phase == MP_LOBBY_CLOSED && snapshot.host_id == UINT32_MAX);
    check_round_trip(&lobby);

    /* The last member leaving opens a fresh room with a new id and a new host. */
    uint32_t old_room = lobby.room;
    CHECK(lobby_leave(&lobby, GUEST_ID, GUEST_SESS + 1u) == LOBBY_CHANGED);
    CHECK(lobby.room != old_room && lobby.phase == MP_LOBBY_OPEN && !lobby.has_host);
    CHECK(lobby_join(&lobby, GUEST_ID, GUEST_SESS + 2u, "Next") == LOBBY_CHANGED);
    snapshot = snapshot_of(&lobby);
    CHECK(snapshot.host_id == GUEST_ID && snapshot.run_epoch == 0u);
}

/* A second server on the same port must refuse to start, not share the socket. */
static void check_port_conflict(void)
{
    unsigned short port = (unsigned short)(40000u + GetCurrentProcessId() % 9000u);
    MpServer* first = NULL;
    for (unsigned int attempt = 0; attempt < 16u && !first; ++attempt)
    {
        port = (unsigned short)(port + 101u);
        first = mp_server_create(port, 4);
    }
    CHECK(first != NULL);
    CHECK(!mp_server_port_free(port));
    CHECK(mp_server_create(port, 4) == NULL);
    mp_server_destroy(first);
    CHECK(mp_server_port_free(port));
}

int main(void)
{
    check_request_codecs();
    check_host_and_ready();
    check_start_barrier();
    check_cancellations();
    check_port_conflict();
    puts("Lobby checks passed: codecs, host and ready rules, settings reset, start barrier, "
         "duplicate start, load timeout, disconnect cancel, host leave and port conflict.");
    return 0;
}
