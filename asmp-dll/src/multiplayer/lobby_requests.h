/* Turns the room intent of the game thread into lobby requests on the worker: each
   request is repeated every LOBBY_REQUEST_REPEAT_MS until the snapshot shows its
   result, because the transport may drop packets. Layer: multiplayer/ (session
   orchestration). Pure decisions; the caller passes a sender. Worker thread only. */

#ifndef ASMP_LOBBY_REQUESTS_H
#define ASMP_LOBBY_REQUESTS_H

#include <windows.h>
#include "session.h"

#define LOBBY_REQUEST_REPEAT_MS 500u

/* Sends one request; returns 1 when it was handed to the transport. */
typedef struct LobbySender
{
    void* context;
    int (*ready)(void* context, const MpLobbyReady* request);
    int (*start)(void* context, const MpLobbyStart* request);
    int (*loaded)(void* context, const MpLobbyLoaded* request);
} LobbySender;

typedef struct LobbyRequests
{
    unsigned int generation; /* Timers restart with each connection. */
    DWORD ready_sent_at;
    DWORD start_sent_at;
    DWORD loaded_sent_at;
    int ready_sent;
    int start_sent;
    int loaded_sent;
} LobbyRequests;

/* Decides which requests to send now for this snapshot. `local_id` is this client's
   relay id. Returns how many requests were sent. */
unsigned int lobby_requests_step(LobbyRequests* requests, const LobbyIntent* intent,
                                 const MpLobby* lobby, uint32_t local_id, DWORD now,
                                 const LobbySender* sender);

#endif
