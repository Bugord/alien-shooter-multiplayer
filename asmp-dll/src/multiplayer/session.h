#ifndef ASMP_SESSION_H
#define ASMP_SESSION_H

/* Session state machine of the multiplayer menu and room. The game thread owns the
   state; the worker sees commands and intents and publishes the link only through
   the session lock (see session.c). Return convention: int functions return 1 for
   success or "yes" and 0 otherwise; session_tick also returns -1 after a native fault. */

#include <windows.h>
#include <stdint.h>
#include "../game/probe.h"
#include "../../../common/src/lobby_protocol.h"

enum SessionState
{
    SS_OFFLINE,         /* No connection. */
    SS_REQUESTED,       /* A connect or host command is queued for the worker. */
    SS_CONNECTING,      /* The worker starts the server and/or connects. */
    SS_LOBBY,           /* In the room menu; waiting for the host to start. */
    SS_RELEASE_MENU,    /* The script releases the menu before the map loads. */
    SS_LOADING,         /* The map load is issued on this tick. */
    SS_WAIT_LEVEL,      /* The map is loading. */
    SS_WAIT_PLAYERS,    /* Loaded; waiting until every participant loaded. */
    SS_IN_GAME,         /* Gameplay allowed: states and shots are published. */
    SS_DISCONNECTING,   /* The disconnect command is being queued. */
    SS_RETURNING_MENU,  /* Connection ended in a level; main menu routes to ASMP. */
    SS_RETURNING_LOBBY, /* Start cancelled after loading; main menu routes back to the room. */
    SS_FAILED,          /* Connection failed or ended; the menu shows why. */
    SS_ABANDONED        /* A native fault: no more native calls. */
};

/* Codes in the health field of the menu status entities, shared with
   game/AlienShooter/Maps/asmp_play.lgc (SPMS_*). Requests are written by the script
   and consumed here; every other code is written here for the script to show. */
enum MenuStatus
{
    SM_IDLE = 0,
    SM_CONNECT_REQUESTED = 1, /* request: join the address */
    SM_BAD_NICKNAME = 2,
    SM_BAD_ADDRESS = 3,
    SM_CONNECTING = 4,
    SM_FAILED = 5,
    SM_RELEASE_MENU = 6,
    SM_CONNECTION_LOST = 7,
    SM_HOST_REQUESTED = 8, /* request: create a room on this PC */
    SM_NO_ANSWER = 9,      /* address, port forwarding or firewall */
    SM_INCOMPATIBLE = 10,  /* other mod build, or not an ASMP server */
    SM_SERVER_MISSING = 11,
    SM_PORT_IN_USE = 12,
    SM_HOST_LEFT = 13,
    SM_ENTER_LOBBY = 14, /* the script loads the room menu */
    SM_SERVER_FAILED = 15,
    SM_LOBBY_ACTION = 20, /* request: guest toggles ready, host starts */
    SM_LOBBY_LEAVE = 21,  /* request: leave the room */
    SM_LOBBY_NOT_READY = 22,
    SM_LOBBY_READY = 23,
    SM_LOBBY_HOST_WAITING = 24,
    SM_LOBBY_HOST_CAN_START = 25,
    SM_LOBBY_STARTING = 26,
    SM_LOBBY_IN_PROGRESS = 27,
    SM_RETURN_LOBBY = 28 /* root menu bridge: go back to the room */
};

/* Native menu and drawing functions (game/ui.c in production, mocks in tests).
   draw_text may be NULL; then the room panel is not drawn. */
typedef struct SessionEngine
{
    uintptr_t (*menu_item)(uintptr_t, unsigned int, unsigned int);
    int (*text)(uintptr_t, char*, unsigned int);
    void (*status)(uintptr_t, int);
    void (*button)(uintptr_t, int);
    int (*load_map)(uintptr_t, const char*);
    void (*draw_text)(float x, float y, const char* text, uint32_t color);
} SessionEngine;

enum SessionCommand
{
    SESSION_DISCONNECT = 0,
    SESSION_CONNECT = 1, /* join host:port */
    SESSION_HOST = 2     /* start our own server on port, then join it */
};

typedef struct ConnectionRequest
{
    unsigned int generation;
    enum SessionCommand command;
    int lobby; /* 1: wait in the room for a start; 0: diagnostic direct join */
    char host[16];
    char name[16];
    unsigned short port;
} ConnectionRequest;

/* Why a connection could not be made or ended. */
enum SessionFailure
{
    SF_NONE = 0,
    SF_NO_ANSWER,
    SF_REFUSED,
    SF_LOST,
    SF_INCOMPATIBLE,
    SF_SERVER_MISSING,
    SF_PORT_IN_USE,
    SF_SERVER_FAILED
};

/* Worker -> game: what the connection of one generation currently looks like. Each
   publish replaces the previous one. */
typedef struct SessionLink
{
    int connected;
    enum SessionFailure failure;
    char map[128]; /* Welcome map (direct join). */
    int lobby_known;
    MpLobby lobby;
    uint32_t local_id;
} SessionLink;

/* Game -> worker: what the room should become. The worker repeats requests until
   the snapshot shows them (multiplayer/lobby_requests.c). */
typedef struct LobbyIntent
{
    unsigned int generation;
    int ready_wanted; /* 1 when ready_value is meaningful */
    uint32_t ready_value;
    uint32_t ready_settings_revision;
    int start_wanted;
    uint32_t start_room;
    uint32_t start_revision;
    uint32_t loaded_room;
    uint32_t loaded_run_epoch; /* 0: nothing loaded */
} LobbyIntent;

int session_enable(uintptr_t base);
int session_initialize(const SessionEngine*);
enum SessionState session_state(void);

/* Before tick installation only: the launcher's direct connection (diagnostics). It
   loads the welcome map at once and skips the room. */
int session_direct(const char* host, unsigned short port, const char* name);

/* Worker: consumes commands, publishes the link and reads the room intent. */
int session_take_request(ConnectionRequest* request);
void session_publish_link(unsigned int generation, const SessionLink* link);
int session_read_intent(LobbyIntent* intent);

/* Game thread, after the native update: reads the menu, follows the room and loads
   the map. Returns 1 when it loaded a map, 0 otherwise, -1 after a native fault. */
int session_tick(const Snapshot*, enum ProbeResult, DWORD now);

/* Game thread, from the display hook: draws the room panel while in the room. */
void session_draw(void);

void session_stop(void);

#endif
