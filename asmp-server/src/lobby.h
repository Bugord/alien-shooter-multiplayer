/* Room state of the relay server: members, host, ready flags, settings and the
   start/load barrier. Layer: asmp-server (relay). Pure logic without sockets, so
   tests drive it directly; server.c feeds it events and broadcasts snapshots.

   Return convention: every function that takes an event returns
   LOBBY_CHANGED when the snapshot changed (broadcast it) and LOBBY_UNCHANGED
   when the event was ignored or changed nothing visible. */

#ifndef ASMP_SERVER_LOBBY_H
#define ASMP_SERVER_LOBBY_H

#include <stdint.h>
#include "lobby_protocol.h"

enum LobbyResult
{
    LOBBY_UNCHANGED = 0,
    LOBBY_CHANGED = 1
};

typedef struct LobbyMember
{
    int present;
    uint32_t session;
    char name[MP_MAX_NAME_LEN + 1];
    int ready;
    int participant;
    int loaded;
} LobbyMember;

typedef struct Lobby
{
    uint32_t room;
    uint32_t room_seed; /* Mixed with a counter to give every new room its own id. */
    uint32_t rooms_created;
    uint32_t revision;
    uint32_t settings_revision;
    enum MpLobbyPhase phase;
    int has_host;
    uint32_t host_id;
    uint32_t host_session;
    uint32_t run_epoch;
    enum MpLobbyNotice notice;
    unsigned int capacity;
    char map[MP_MAX_MAP_NAME_LEN];
    unsigned long started_at; /* When the current start began loading. */
    LobbyMember members[MP_ROSTER_MAX_ENTRIES];
} Lobby;

/* Prepares an empty open room. `map` must be a valid level path; capacity 1..16. */
int lobby_init(Lobby* lobby, uint32_t seed, unsigned int capacity, const char* map);

/* A player finished the handshake. The first member of an empty room becomes its host. */
enum LobbyResult lobby_join(Lobby* lobby, uint32_t id, uint32_t session, const char* name);

/* A player left (transport gone). The host leaving closes the room; the last member
   leaving resets it to a fresh open room with a new id. */
enum LobbyResult lobby_leave(Lobby* lobby, uint32_t id, uint32_t session);

enum LobbyResult lobby_set_ready(Lobby* lobby, uint32_t id, uint32_t session,
                                 const MpLobbyReady* request);
enum LobbyResult lobby_change_settings(Lobby* lobby, uint32_t id, uint32_t session,
                                       const MpLobbySettings* request);
enum LobbyResult lobby_start(Lobby* lobby, uint32_t id, uint32_t session,
                             const MpLobbyStart* request, unsigned long now);
enum LobbyResult lobby_mark_loaded(Lobby* lobby, uint32_t id, uint32_t session,
                                   const MpLobbyLoaded* request);

/* Cancels a start that did not finish loading in MP_LOBBY_LOAD_TIMEOUT_MS. */
enum LobbyResult lobby_tick(Lobby* lobby, unsigned long now);

/* Fills the wire snapshot of the current room. */
void lobby_snapshot(const Lobby* lobby, MpLobby* out);

#endif
