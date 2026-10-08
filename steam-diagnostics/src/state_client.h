#ifndef ASMP_STATE_CLIENT_H
#define ASMP_STATE_CLIENT_H
#include <stdio.h>
#include "../../common/src/steam_state_protocol.h"
typedef struct StateClient StateClient;
StateClient* state_client_create(const char* host, unsigned short port, const char* name, FILE* log);
void state_client_destroy(StateClient* client);
/* All operations belong to the worker thread, never the game hook. */
void state_client_publish(StateClient* client, const MpSteamState* state, unsigned long now);
void state_client_update(StateClient* client, unsigned long now);
int state_client_connected(const StateClient* client);
int state_client_peer(const StateClient* client, unsigned int id, unsigned long now, MpSteamState* state);
void state_client_stats(const StateClient* client);
#endif
