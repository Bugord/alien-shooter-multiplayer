#ifndef ASMP_STATE_CLIENT_H
#define ASMP_STATE_CLIENT_H
#include <stdio.h>
#include "../../../../common/src/protocol.h"
#include "../../../../common/src/monster_protocol.h"
#include "../../../../common/src/lobby_protocol.h"
#include "transport.h"
typedef struct StateClient StateClient;

/* What the connection has reached so far, and how it ended. */
typedef enum LobbyLink
{
    LINK_NONE = 0,
    LINK_CONNECTING,  /* Waiting for the transport to answer. */
    LINK_HANDSHAKE,   /* Transport connected; waiting for the welcome. */
    LINK_READY,       /* Welcomed by the relay. */
    LINK_NO_ANSWER,   /* The address never answered (address, NAT or firewall). */
    LINK_REFUSED,     /* Answered, then dropped before the welcome. */
    LINK_LOST,        /* Was ready, then disconnected. */
    LINK_INCOMPATIBLE /* The relay is another build (lobby compat word differs). */
} LobbyLink;

StateClient* state_client_create(const char* host, unsigned short port, const char* name,
                                 FILE* log);
/* Takes ownership of an already connecting transport (any backend). */
StateClient* state_client_create_with_transport(Transport* transport, const char* name, FILE* log);
void state_client_destroy(StateClient* client);
/* All operations belong to the worker thread, never the game hook. */
void state_client_publish(StateClient* client, const MpState* state, unsigned long now);
void state_client_update(StateClient* client, unsigned long now);
const char* state_client_map(const StateClient* client);
int state_client_connected(const StateClient* client);
int state_client_peer(const StateClient* client, unsigned int id, unsigned long now,
                      MpState* state);
void state_client_stats(const StateClient* client);
#define MP_MAX_PEERS 16u
typedef struct PeerState
{
    MpState state;
    uint32_t session;
    int present;
    char name[16];
} PeerState;
typedef struct ShotEvent
{
    MpShot shot;
    uint32_t id, session;
} ShotEvent;
int state_client_peer_info(const StateClient*, unsigned int, unsigned long, PeerState*);
/* Reject captures from an inactive, expired or replaced local world. */
int state_client_send_shot(StateClient*, const MpShot*, unsigned long now);
int state_client_take_shot(StateClient*, ShotEvent*);
typedef struct MonsterState
{
    MpMonsterState state;
    uint32_t authority_id, authority_session;
    int present;
} MonsterState;
typedef struct MonsterEvent
{
    MpMonsterEvent event;
    uint32_t authority_id, authority_session;
} MonsterEvent;
/* Opt in before/after handshake. Only the first claimant can publish; the
   server freezes its id+session until every transport connection leaves. */
void state_client_monster_claim(StateClient*);
int state_client_monster_authority(const StateClient*, uint32_t*, uint32_t*, int*);
int state_client_publish_monster(StateClient*, const MpMonsterState*, unsigned long now);
int state_client_monster_at(const StateClient*, unsigned int slot, unsigned long now,
                            MonsterState*);
int state_client_monster(const StateClient*, unsigned long now, MonsterState*);
int state_client_send_monster_event(StateClient*, const MpMonsterEvent*, unsigned long now);
int state_client_take_monster_event(StateClient*, MonsterEvent*);
/* Lobby: the latest room snapshot and requests to the relay. Requests return 0 when
   the client is not welcomed yet; the caller repeats them until the snapshot shows
   the result. `local_id` receives this client's relay id. */
LobbyLink state_client_link(const StateClient*);
int state_client_lobby(const StateClient*, MpLobby* out, uint32_t* local_id);
int state_client_send_lobby_ready(StateClient*, const MpLobbyReady*);
int state_client_send_lobby_settings(StateClient*, const MpLobbySettings*);
int state_client_send_lobby_start(StateClient*, const MpLobbyStart*);
int state_client_send_lobby_loaded(StateClient*, const MpLobbyLoaded*);
#endif
