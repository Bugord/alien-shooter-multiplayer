#ifndef ASMP_STATE_CLIENT_H
#define ASMP_STATE_CLIENT_H
#include <stdio.h>
#include "../../../../common/src/steam_state_protocol.h"
typedef struct StateClient StateClient;
StateClient* state_client_create(const char* host, unsigned short port, const char* name, FILE* log);
void state_client_destroy(StateClient* client);
/* All operations belong to the worker thread, never the game hook. */
void state_client_publish(StateClient* client, const MpSteamState* state, unsigned long now);
void state_client_update(StateClient* client, unsigned long now);
const char* state_client_map(const StateClient* client);
int state_client_connected(const StateClient* client);
int state_client_peer(const StateClient* client, unsigned int id, unsigned long now, MpSteamState* state);
void state_client_stats(const StateClient* client);
#define STEAM_MAX_PEERS 16u
typedef struct SteamPeerState {
    MpSteamState state;
    uint32_t session;
    int present;
    char name[16];
} SteamPeerState;
typedef struct SteamShotEvent {
    MpSteamShot shot;
    uint32_t id, session;
} SteamShotEvent;
int state_client_peer_info(const StateClient*, unsigned int, unsigned long, SteamPeerState*);
/* Reject captures from an inactive, expired or replaced local world. */
int state_client_send_shot(StateClient*, const MpSteamShot*, unsigned long now);
int state_client_take_shot(StateClient*, SteamShotEvent*);
#endif
