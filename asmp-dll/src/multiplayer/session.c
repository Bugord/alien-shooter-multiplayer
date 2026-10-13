/* Session state machine for the multiplayer menu and room: reads the menu entities,
   turns "create" and "connect" into worker commands, follows the room snapshot, loads
   the map only when the host's start reaches this player, reports the load, and
   allows gameplay only after every participant loaded. It also returns to the menu
   or the room when a connection ends or a start is cancelled. Layer: multiplayer/
   (session/map state machine). The game thread owns the state; the worker sees
   commands, intents and links only through the lock. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "session.h"
#include "../game/ui.h"

#define SESSION_CONNECT_TIMEOUT_MS 15000u /* Server start (5 s) plus the connection. */
#define SESSION_LOAD_TIMEOUT_MS    10000u
#define SESSION_BARRIER_TIMEOUT_MS (MP_LOBBY_LOAD_TIMEOUT_MS + 10000u)
#define ROOM_MENU_GRACE_MS         3000u
#define MAIN_MENU_MAP              "maps\\mainmenu.map"
#define LOCAL_HOST_ADDRESS         "127.0.0.1"

/* Menu entities: VID index and the raw direction byte of the menu record. */
#define FORM_STATUS_VID    2u
#define FORM_STATUS_DIR    0u
#define ROOM_STATUS_VID    2u
#define ROOM_STATUS_DIR    25u /* ndir 1: the script finds it with MenuFind(2, 1). */
#define NICKNAME_VID       4u
#define NICKNAME_DIR       2u
#define ADDRESS_VID        4u
#define ADDRESS_DIR        3u
#define CONNECT_BUTTON_VID 705u
#define CONNECT_BUTTON_DIR 25u
#define ROOT_BRIDGE_VID    705u
#define ROOT_BRIDGE_DIR    204u

/* Room panel layout in screen pixels. */
#define PANEL_X           24.0f
#define PANEL_Y           24.0f
#define PANEL_LINE        16.0f
#define PANEL_WHITE       0xFFFFFFFFu
#define PANEL_GREEN       0xFF60FF60u
#define PANEL_YELLOW      0xFFFFE060u
#define PANEL_LINE_LENGTH 64

typedef struct MenuView
{
    uintptr_t game;
    uintptr_t form_status;
    uintptr_t room_status;
    uintptr_t nickname;
    uintptr_t address;
    uintptr_t button;
    uintptr_t root;
} MenuView;

typedef struct LinkView
{
    int observed; /* 0 when the lock was busy: "no news", not a lost connection */
    int current;  /* The link belongs to the current generation. */
    SessionLink link;
} LinkView;

static struct
{
    SessionEngine engine;
    volatile LONG enabled;
    enum SessionState state;
    enum SessionState after_disconnect;
    int load_menu_after_disconnect;
    int lobby_mode;
    unsigned int generation;
    DWORD entered_at;
    DWORD room_menu_missing_since;
    int room_menu_missing;
    enum MenuStatus return_status;
    DWORD game_thread;
    LobbyIntent intent; /* Game-thread copy; flushed to `shared_intent`. */

    /* The start this player is loading or playing. */
    struct
    {
        uint32_t room;
        uint32_t run_epoch;
        uint32_t world_low;
        uint32_t world_high;
        char map[128];
    } start;

    /* Game-thread copy of the room for drawing. */
    struct
    {
        int known;
        MpLobby lobby;
        uint32_t local_id;
    } room;

    /* Guarded by `lock`. */
    int pending;
    ConnectionRequest command;
    int link_ready;
    unsigned int link_generation;
    SessionLink link;
    LobbyIntent shared_intent;
} session;

static SRWLOCK lock = SRWLOCK_INIT;

/* Stores the menu/load functions (injected by tests). Returns 0 if one is missing. */
int session_initialize(const SessionEngine* api)
{
    if (session.enabled || !api || !api->menu_item || !api->text || !api->status || !api->button ||
        !api->load_map)
        return 0;
    memset(&session, 0, sizeof(session));
    session.engine = *api;
    InterlockedExchange(&session.enabled, 1);
    return 1;
}

/* Binds the production menu functions from game/ui.c. */
int session_enable(uintptr_t base)
{
    if (!ui_bind(base))
        return 0;
    SessionEngine api = {ui_menu_item, ui_text,     ui_menu_status,
                         ui_button,    ui_load_map, ui_screen_text};
    if (session_initialize(&api))
        return 1;
    ui_stop();
    return 0;
}

/* Queues a command for the worker with a new generation and clears the room intent.
   Skips (returns 0) when the lock is busy so the game thread never waits. */
static int request(enum SessionCommand command, const char* host, unsigned short port,
                   const char* name, int lobby)
{
    if (!TryAcquireSRWLockExclusive(&lock))
        return 0;
    if (!InterlockedCompareExchange(&session.enabled, 0, 0))
    {
        ReleaseSRWLockExclusive(&lock);
        return 0;
    }

    if (!++session.generation)
        ++session.generation;
    memset(&session.command, 0, sizeof(session.command));
    session.command.generation = session.generation;
    session.command.command = command;
    session.command.lobby = lobby;
    session.command.port = port;
    if (command != SESSION_DISCONNECT)
    {
        strcpy_s(session.command.host, sizeof(session.command.host), host);
        strcpy_s(session.command.name, sizeof(session.command.name), name);
    }
    session.pending = 1;
    session.link_ready = 0;

    memset(&session.intent, 0, sizeof(session.intent));
    session.intent.generation = session.generation;
    session.shared_intent = session.intent;
    ReleaseSRWLockExclusive(&lock);

    /* A new connection must not mistake its first start for one already loaded. */
    if (command != SESSION_DISCONNECT)
    {
        memset(&session.start, 0, sizeof(session.start));
        memset(&session.room, 0, sizeof(session.room));
    }
    return 1;
}

/* Starts a connection from the command line instead of the menu (diagnostics). */
int session_direct(const char* host, unsigned short port, const char* name)
{
    if (!session.enabled || session.state != SS_OFFLINE || !host || strlen(host) >= 16 || !name ||
        !*name || strlen(name) >= 16 || !port)
        return 0;
    if (!request(SESSION_CONNECT, host, port, name, 0))
        return 0;
    session.lobby_mode = 0;
    session.state = SS_REQUESTED;
    return 1;
}

/* Worker: takes the pending command, if any. */
int session_take_request(ConnectionRequest* out)
{
    AcquireSRWLockExclusive(&lock);
    int ready = session.pending;
    if (ready)
    {
        *out = session.command;
        session.pending = 0;
    }
    ReleaseSRWLockExclusive(&lock);
    return ready;
}

/* Worker: replaces the link of `generation`. Links of older generations are ignored. */
void session_publish_link(unsigned int generation, const SessionLink* link)
{
    AcquireSRWLockExclusive(&lock);
    if (session.enabled && generation == session.generation && link)
    {
        session.link = *link;
        session.link_generation = generation;
        session.link_ready = 1;
    }
    ReleaseSRWLockExclusive(&lock);
}

/* Worker: copies the latest room intent. */
int session_read_intent(LobbyIntent* out)
{
    AcquireSRWLockShared(&lock);
    int enabled = session.enabled != 0;
    if (enabled)
        *out = session.shared_intent;
    ReleaseSRWLockShared(&lock);
    return enabled;
}

/* Game thread: copies the link without waiting. */
static void read_link(LinkView* view)
{
    memset(view, 0, sizeof(*view));
    if (!TryAcquireSRWLockShared(&lock))
        return;
    view->observed = 1;
    view->current = session.link_ready && session.link_generation == session.generation;
    if (view->current)
        view->link = session.link;
    ReleaseSRWLockShared(&lock);
}

/* Game thread: hands the intent to the worker; a busy lock retries next tick. */
static void flush_intent(void)
{
    if (!TryAcquireSRWLockExclusive(&lock))
        return;
    if (session.intent.generation == session.generation)
        session.shared_intent = session.intent;
    ReleaseSRWLockExclusive(&lock);
}

/* Splits "host[:port]" in place. A missing port means MP_DEFAULT_PORT. Returns 0 for a
   bad port or an over-long host; an empty host is allowed only when `host_optional`. */
static int parse_address(char* address, unsigned short* port, int host_optional)
{
    *port = (unsigned short)MP_DEFAULT_PORT;
    char* colon = strrchr(address, ':');
    if (colon)
    {
        char* end;
        unsigned long value = strtoul(colon + 1, &end, 10);
        if (!value || value > 65535 || end == colon + 1 || *end)
            return 0;
        *colon = 0;
        *port = (unsigned short)value;
    }
    if (strlen(address) >= 16)
        return 0;
    return host_optional || address[0] != 0;
}

static void read_menu(uintptr_t game, MenuView* view)
{
    memset(view, 0, sizeof(*view));
    view->game = game;
    view->form_status = session.engine.menu_item(game, FORM_STATUS_VID, FORM_STATUS_DIR);
    view->room_status = session.engine.menu_item(game, ROOM_STATUS_VID, ROOM_STATUS_DIR);
    view->nickname = session.engine.menu_item(game, NICKNAME_VID, NICKNAME_DIR);
    view->address = session.engine.menu_item(game, ADDRESS_VID, ADDRESS_DIR);
    view->button = session.engine.menu_item(game, CONNECT_BUTTON_VID, CONNECT_BUTTON_DIR);
    view->root = session.engine.menu_item(game, ROOT_BRIDGE_VID, ROOT_BRIDGE_DIR);
}

static int read_status(uintptr_t entity)
{
    return *(int*)(entity + STEAM_ENTITY_HEALTH_OFFSET);
}

/* Shows a code in whichever status entity the current menu has. */
static void show(const MenuView* menu, enum MenuStatus code)
{
    if (menu->room_status)
        session.engine.status(menu->room_status, code);
    else if (menu->form_status)
        session.engine.status(menu->form_status, code);
}

static void enable_button(const MenuView* menu, int disabled)
{
    if (menu->button)
        session.engine.button(menu->button, disabled);
}

/* Asks the worker to disconnect and remembers where to go afterwards. */
static void disconnect(enum SessionState next, int load_menu)
{
    session.after_disconnect = next;
    session.load_menu_after_disconnect = load_menu;
    session.state = SS_DISCONNECTING;
}

/* Ends the connection while still in the menu and shows why. */
static void fail_in_menu(const MenuView* menu, enum MenuStatus code)
{
    show(menu, code);
    enable_button(menu, 0);
    disconnect(SS_FAILED, 0);
}

/* Ends the connection from inside a level; the main menu then shows `code`. */
static void fail_in_level(enum MenuStatus code)
{
    session.return_status = code;
    disconnect(SS_RETURNING_MENU, 1);
}

static enum MenuStatus failure_status(enum SessionFailure failure)
{
    switch (failure)
    {
    case SF_NO_ANSWER:
        return SM_NO_ANSWER;
    case SF_INCOMPATIBLE:
        return SM_INCOMPATIBLE;
    case SF_SERVER_MISSING:
        return SM_SERVER_MISSING;
    case SF_PORT_IN_USE:
        return SM_PORT_IN_USE;
    case SF_SERVER_FAILED:
        return SM_SERVER_FAILED;
    case SF_LOST:
        return SM_CONNECTION_LOST;
    default:
        return SM_FAILED;
    }
}

/* True when the worker reported that the connection of this generation failed or
   ended. The worker sets a failure for every ending, so "not connected yet" while
   connecting is not a failure. */
static int link_down(const LinkView* link)
{
    if (!link->observed || !link->current)
        return 0;
    return link->link.failure != SF_NONE;
}

/* Copies the room for drawing and the room steps. */
static void remember_room(const LinkView* link)
{
    if (!link->observed || !link->current)
        return;
    session.room.known = link->link.lobby_known;
    if (link->link.lobby_known)
    {
        session.room.lobby = link->link.lobby;
        session.room.local_id = link->link.local_id;
    }
}

static const MpLobbyEntry* local_entry(void)
{
    if (!session.room.known)
        return NULL;
    return mp_lobby_entry(&session.room.lobby, session.room.local_id);
}

static int local_is_host(void)
{
    const MpLobbyEntry* entry = local_entry();
    return entry && session.room.lobby.host_id == entry->id &&
           session.room.lobby.host_session == entry->session;
}

/* Reads name and address from the form and queues a host or join command. */
static void submit_form(const MenuView* menu, enum SessionCommand command)
{
    char name[16];
    char address[32];
    unsigned short port;

    if (!session.engine.text(menu->nickname, name, sizeof(name)) || !*name)
    {
        session.engine.status(menu->form_status, SM_BAD_NICKNAME);
        return;
    }
    int host_optional = command == SESSION_HOST;
    if (!session.engine.text(menu->address, address, sizeof(address)) ||
        !parse_address(address, &port, host_optional))
    {
        session.engine.status(menu->form_status, SM_BAD_ADDRESS);
        return;
    }

    const char* target = command == SESSION_HOST ? LOCAL_HOST_ADDRESS : address;
    if (!request(command, target, port, name, 1))
        return; /* Lock busy: the request code stays and is read again next tick. */

    session.lobby_mode = 1;
    session.state = SS_REQUESTED;
    session.engine.status(menu->form_status, SM_CONNECTING);
    enable_button(menu, 1);
}

/* Offline or failed: waits for "create" or "connect" in the form. */
static void tick_form(const MenuView* menu)
{
    if (!menu->form_status || !menu->nickname || !menu->address || !menu->button)
        return;

    int code = read_status(menu->form_status);
    if (code == SM_CONNECT_REQUESTED)
        submit_form(menu, SESSION_CONNECT);
    else if (code == SM_HOST_REQUESTED)
        submit_form(menu, SESSION_HOST);
}

/* Direct join (diagnostics): load the welcome map at once. */
static void begin_direct_load(const MenuView* menu, const LinkView* link)
{
    strcpy_s(session.start.map, sizeof(session.start.map), link->link.map);
    mp_world_key(session.start.map, &session.start.world_low, &session.start.world_high);
    if (menu->form_status)
    {
        session.engine.status(menu->form_status, SM_RELEASE_MENU);
        session.state = SS_RELEASE_MENU;
    }
    else
        session.state = SS_LOADING;
}

static void tick_connecting(const MenuView* menu, const LinkView* link, DWORD now)
{
    if (link_down(link))
    {
        fail_in_menu(menu, failure_status(link->link.failure));
        return;
    }
    if (now - session.entered_at >= SESSION_CONNECT_TIMEOUT_MS)
    {
        fail_in_menu(menu, SM_NO_ANSWER);
        return;
    }
    if (!link->observed || !link->current || !link->link.connected)
        return;

    if (!session.lobby_mode)
    {
        if (mp_is_level_path(link->link.map))
            begin_direct_load(menu, link);
        else
            fail_in_menu(menu, SM_FAILED);
        return;
    }
    if (!session.room.known)
        return;
    if (session.room.lobby.phase == MP_LOBBY_CLOSED)
    {
        fail_in_menu(menu, SM_HOST_LEFT);
        return;
    }
    show(menu, SM_ENTER_LOBBY);
    session.room_menu_missing = 0;
    session.state = SS_LOBBY;
}

/* Handles the room menu's request code. Returns 1 when the player left. */
static int consume_room_request(const MenuView* menu)
{
    int code = read_status(menu->room_status);
    if (code == SM_LOBBY_LEAVE)
    {
        session.engine.status(menu->room_status, SM_IDLE);
        disconnect(SS_OFFLINE, 0);
        return 1;
    }
    if (code != SM_LOBBY_ACTION)
        return 0;

    const MpLobbyEntry* entry = local_entry();
    if (entry && session.room.lobby.phase == MP_LOBBY_OPEN)
    {
        if (local_is_host())
        {
            session.intent.start_wanted = 1;
            session.intent.start_room = session.room.lobby.room;
            session.intent.start_revision = session.room.lobby.revision;
        }
        else
        {
            session.intent.ready_wanted = 1;
            session.intent.ready_value = (entry->flags & MP_LOBBY_ENTRY_READY) ? 0u : 1u;
            session.intent.ready_settings_revision = session.room.lobby.settings_revision;
        }
    }
    session.engine.status(menu->room_status, SM_IDLE);
    return 0;
}

/* True when every member other than the host is ready. */
static int guests_ready(const MpLobby* lobby)
{
    for (unsigned int i = 0; i < lobby->count; ++i)
    {
        const MpLobbyEntry* entry = &lobby->entries[i];
        if (entry->id == lobby->host_id)
            continue;
        if (!(entry->flags & MP_LOBBY_ENTRY_READY))
            return 0;
    }
    return 1;
}

static enum MenuStatus room_display(void)
{
    const MpLobbyEntry* entry = local_entry();
    const MpLobby* lobby = &session.room.lobby;
    int participant = entry && (entry->flags & MP_LOBBY_ENTRY_PARTICIPANT);

    if (lobby->phase == MP_LOBBY_IN_GAME || (lobby->phase == MP_LOBBY_STARTING && !participant))
        return SM_LOBBY_IN_PROGRESS;
    if (lobby->phase == MP_LOBBY_STARTING)
        return SM_LOBBY_STARTING;
    if (local_is_host())
        return guests_ready(lobby) ? SM_LOBBY_HOST_CAN_START : SM_LOBBY_HOST_WAITING;
    if (entry && (entry->flags & MP_LOBBY_ENTRY_READY))
        return SM_LOBBY_READY;
    return SM_LOBBY_NOT_READY;
}

/* The host's start reached this participant: freeze the start and load its map. */
static void begin_room_load(const MenuView* menu)
{
    const MpLobby* lobby = &session.room.lobby;
    session.start.room = lobby->room;
    session.start.run_epoch = lobby->run_epoch;
    strcpy_s(session.start.map, sizeof(session.start.map), lobby->map);
    mp_world_key(session.start.map, &session.start.world_low, &session.start.world_high);

    session.intent.start_wanted = 0;
    session.intent.ready_wanted = 0;
    session.intent.loaded_room = 0;
    session.intent.loaded_run_epoch = 0;

    show(menu, SM_RELEASE_MENU);
    session.state = SS_RELEASE_MENU;
}

/* Leaves the room when neither menu is shown for a while (e.g. Esc to main menu). */
static int room_menu_gone(const MenuView* menu, DWORD now)
{
    if (menu->room_status || menu->form_status)
    {
        session.room_menu_missing = 0;
        return 0;
    }
    if (!session.room_menu_missing)
    {
        session.room_menu_missing = 1;
        session.room_menu_missing_since = now;
        return 0;
    }
    return now - session.room_menu_missing_since >= ROOM_MENU_GRACE_MS;
}

static void tick_lobby(const MenuView* menu, const LinkView* link, DWORD now)
{
    if (link_down(link))
    {
        fail_in_menu(menu, failure_status(link->link.failure));
        return;
    }
    if (session.room.known && session.room.lobby.phase == MP_LOBBY_CLOSED)
    {
        fail_in_menu(menu, SM_HOST_LEFT);
        return;
    }
    if (room_menu_gone(menu, now))
    {
        disconnect(SS_OFFLINE, 0);
        return;
    }
    if (!menu->room_status)
    {
        /* The script still shows the form; it switches on this code. */
        if (menu->form_status)
            session.engine.status(menu->form_status, SM_ENTER_LOBBY);
        return;
    }
    if (consume_room_request(menu))
        return;

    const MpLobbyEntry* entry = local_entry();
    int participant = entry && (entry->flags & MP_LOBBY_ENTRY_PARTICIPANT);
    if (session.room.lobby.phase == MP_LOBBY_STARTING && participant &&
        session.room.lobby.run_epoch != session.start.run_epoch)
    {
        begin_room_load(menu);
        return;
    }
    if (read_status(menu->room_status) != (int)room_display())
        session.engine.status(menu->room_status, room_display());
}

/* True when the server cancelled the start this player is loading. */
static int start_cancelled(void)
{
    const MpLobby* lobby = &session.room.lobby;
    if (!session.lobby_mode || !session.room.known)
        return 0;
    return lobby->room == session.start.room && lobby->run_epoch == session.start.run_epoch &&
           lobby->phase == MP_LOBBY_OPEN;
}

/* True when the room of this start is gone: the host left, or the server opened a
   new room after everyone left. */
static int room_ended(void)
{
    const MpLobby* lobby = &session.room.lobby;
    if (!session.lobby_mode || !session.room.known)
        return 0;
    return lobby->phase == MP_LOBBY_CLOSED || lobby->room != session.start.room;
}

/* Loads the main menu and routes it back to the room. */
static int return_to_room(uintptr_t game)
{
    session.intent.loaded_room = 0;
    session.intent.loaded_run_epoch = 0;
    if (!session.engine.load_map(game, MAIN_MENU_MAP))
    {
        disconnect(SS_ABANDONED, 0);
        return -1;
    }
    session.state = SS_RETURNING_LOBBY;
    return 1;
}

static int in_started_world(const Snapshot* sample)
{
    return sample->in_level && sample->world_low == session.start.world_low &&
           sample->world_high == session.start.world_high;
}

static int tick_wait_level(const Snapshot* sample, const LinkView* link, DWORD now)
{
    if (link_down(link))
    {
        fail_in_level(SM_CONNECTION_LOST);
        return 0;
    }
    if (room_ended())
    {
        fail_in_level(SM_HOST_LEFT);
        return 0;
    }
    if (start_cancelled())
        return return_to_room(sample->game);
    if (in_started_world(sample))
    {
        if (!session.lobby_mode)
        {
            session.state = SS_IN_GAME;
            return 0;
        }
        session.intent.loaded_room = session.start.room;
        session.intent.loaded_run_epoch = session.start.run_epoch;
        session.entered_at = now;
        session.state = SS_WAIT_PLAYERS;
        return 0;
    }
    if (now - session.entered_at >= SESSION_LOAD_TIMEOUT_MS)
        fail_in_level(SM_CONNECTION_LOST);
    return 0;
}

static int tick_wait_players(const Snapshot* sample, const LinkView* link, DWORD now)
{
    const MpLobby* lobby = &session.room.lobby;
    if (link_down(link))
    {
        fail_in_level(SM_CONNECTION_LOST);
        return 0;
    }
    if (room_ended())
    {
        fail_in_level(SM_HOST_LEFT);
        return 0;
    }
    if (start_cancelled())
        return return_to_room(sample->game);
    if (!in_started_world(sample))
    {
        disconnect(SS_OFFLINE, 0);
        return 0;
    }
    if (lobby->phase == MP_LOBBY_IN_GAME && lobby->run_epoch == session.start.run_epoch)
    {
        session.state = SS_IN_GAME;
        return 0;
    }
    if (now - session.entered_at >= SESSION_BARRIER_TIMEOUT_MS)
        fail_in_level(SM_CONNECTION_LOST);
    return 0;
}

static void tick_in_game(const Snapshot* sample, const LinkView* link)
{
    if (link_down(link))
        fail_in_level(SM_CONNECTION_LOST);
    else if (room_ended())
        fail_in_level(SM_HOST_LEFT);
    else if (!in_started_world(sample))
        disconnect(SS_OFFLINE, 0);
}

static int tick_disconnecting(uintptr_t game)
{
    if (!request(SESSION_DISCONNECT, NULL, 0, NULL, 0))
        return 0;
    session.state = session.after_disconnect;
    if (!session.load_menu_after_disconnect)
        return 0;
    if (!session.engine.load_map(game, MAIN_MENU_MAP))
    {
        session.state = SS_ABANDONED;
        return -1;
    }
    return 1;
}

/* Script bridge after the main menu loads: the root menu routes to ASMP and the form
   then shows why the connection ended. It is not a world-exit heuristic. */
static void tick_returning_menu(const MenuView* menu)
{
    if (menu->root)
        session.engine.status(menu->root, SM_CONNECTION_LOST);
    if (!menu->form_status)
        return;
    session.engine.status(menu->form_status, session.return_status);
    enable_button(menu, 0);
    session.state = SS_FAILED;
}

/* After a cancelled start: route the main menu back to ASMP, then into the room. */
static void tick_returning_lobby(const MenuView* menu, const LinkView* link)
{
    if (link_down(link))
    {
        session.return_status = SM_CONNECTION_LOST;
        disconnect(SS_RETURNING_MENU, 0);
        return;
    }
    if (menu->root)
        session.engine.status(menu->root, SM_RETURN_LOBBY);
    if (!menu->form_status && !menu->room_status)
        return;
    show(menu, SM_ENTER_LOBBY);
    session.room_menu_missing = 0;
    session.state = SS_LOBBY;
}

/* Runs the step of the current state. Returns like session_tick. */
static int tick_state(const Snapshot* sample, const MenuView* menu, const LinkView* link, DWORD now)
{
    switch (session.state)
    {
    case SS_OFFLINE:
    case SS_FAILED:
        tick_form(menu);
        return 0;
    case SS_REQUESTED:
        session.entered_at = now;
        session.state = SS_CONNECTING;
        return 0;
    case SS_CONNECTING:
        tick_connecting(menu, link, now);
        return 0;
    case SS_LOBBY:
        tick_lobby(menu, link, now);
        return 0;
    case SS_RELEASE_MENU:
        session.state = SS_LOADING;
        return 0;
    case SS_LOADING:
        if (!session.engine.load_map(sample->game, session.start.map))
        {
            disconnect(SS_ABANDONED, 0);
            return -1;
        }
        session.entered_at = now;
        session.state = SS_WAIT_LEVEL;
        return 1;
    case SS_WAIT_LEVEL:
        return tick_wait_level(sample, link, now);
    case SS_WAIT_PLAYERS:
        return tick_wait_players(sample, link, now);
    case SS_IN_GAME:
        tick_in_game(sample, link);
        return 0;
    case SS_DISCONNECTING:
        return tick_disconnecting(sample->game);
    case SS_RETURNING_MENU:
        tick_returning_menu(menu);
        return 0;
    case SS_RETURNING_LOBBY:
        tick_returning_lobby(menu, link);
        return 0;
    default:
        return 0;
    }
}

/* One game-thread step of the state machine. Returns 1 when it loaded a map. After a
   native fault it makes no more native calls. */
int session_tick(const Snapshot* sample, enum ProbeResult result, DWORD now)
{
    if (!InterlockedCompareExchange(&session.enabled, 0, 0))
        return 0;
    session.game_thread = GetCurrentThreadId();

    /* Never make another native call after a native fault. Even if the game is
       unreadable, the worker must receive the disconnect request. */
    if (session.state == SS_DISCONNECTING && session.after_disconnect == SS_ABANDONED)
    {
        if (request(SESSION_DISCONNECT, NULL, 0, NULL, 0))
            session.state = SS_ABANDONED;
        return 0;
    }
    if (session.state == SS_ABANDONED || !sample->game || result == PROBE_READ_FAULT ||
        result == PROBE_GAME_TYPE)
        return 0;

    int loaded = 0;
    __try
    {
        MenuView menu;
        LinkView link;
        read_menu(sample->game, &menu);
        read_link(&link);
        remember_room(&link);
        loaded = tick_state(sample, &menu, &link, now);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        disconnect(SS_ABANDONED, 0);
        return -1;
    }
    flush_intent();
    return loaded;
}

/* Current state (SS_OFFLINE when disabled). */
enum SessionState session_state(void)
{
    return InterlockedCompareExchange(&session.enabled, 0, 0) ? session.state : SS_OFFLINE;
}

static const char* mode_name(uint32_t mode)
{
    return mode == MP_MODE_SURVIVAL ? "survival" : "campaign";
}

static const char* notice_text(uint32_t notice)
{
    if (notice == MP_LOBBY_NOTICE_LOAD_TIMEOUT)
        return "Start cancelled: a player did not finish loading";
    if (notice == MP_LOBBY_NOTICE_PLAYER_LEFT)
        return "Start cancelled: a player left";
    return NULL;
}

/* One roster line: marker for this player, name, and host or ready state. */
static void draw_member(const MpLobbyEntry* entry, float y)
{
    const MpLobby* lobby = &session.room.lobby;
    char line[PANEL_LINE_LENGTH];
    int is_local = entry->id == session.room.local_id;
    int is_host = entry->id == lobby->host_id && entry->session == lobby->host_session;
    int ready = (entry->flags & MP_LOBBY_ENTRY_READY) != 0;
    const char* role = is_host ? "host" : (ready ? "ready" : "not ready");

    sprintf_s(line, sizeof(line), "%s%s - %s", is_local ? "> " : "  ", entry->name, role);
    session.engine.draw_text(PANEL_X, y, line, ready || is_host ? PANEL_GREEN : PANEL_WHITE);
}

static void draw_room_panel(void)
{
    const MpLobby* lobby = &session.room.lobby;
    char line[PANEL_LINE_LENGTH];
    float y = PANEL_Y;

    sprintf_s(line, sizeof(line), "Room: %s (%s), %u/%u players", lobby->map,
              mode_name(lobby->mode), lobby->count, lobby->capacity);
    session.engine.draw_text(PANEL_X, y, line, PANEL_WHITE);
    for (unsigned int i = 0; i < lobby->count; ++i)
    {
        y += PANEL_LINE;
        draw_member(&lobby->entries[i], y);
    }

    const char* notice = notice_text(lobby->notice);
    if (notice)
        session.engine.draw_text(PANEL_X, y + PANEL_LINE * 2.0f, notice, PANEL_YELLOW);
}

void session_draw(void)
{
    if (!InterlockedCompareExchange(&session.enabled, 0, 0) || !session.engine.draw_text)
        return;
    if (GetCurrentThreadId() != session.game_thread)
        return;

    if (session.state == SS_LOBBY && session.room.known)
        draw_room_panel();
    else if (session.state == SS_WAIT_PLAYERS)
        session.engine.draw_text(PANEL_X, PANEL_Y, "Waiting for the other players to load...",
                                 PANEL_YELLOW);
}

/* Disables the session and drops pending commands. */
void session_stop(void)
{
    InterlockedExchange(&session.enabled, 0);
    AcquireSRWLockExclusive(&lock);
    session.pending = 0;
    session.link_ready = 0;
    ReleaseSRWLockExclusive(&lock);
}
