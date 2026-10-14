/* Session state machine for the multiplayer menu and room: reads the menu entities,
   turns "create" and "connect" into worker commands, follows the room snapshot, loads
   the map only when the host's start reaches this player, reports the load, and
   allows gameplay only after every participant loaded. It also returns to the menu
   or the room when a connection ends or a start is cancelled, and turns the room
   menu's clicks (mode, mission, character) into the room intent; menu_view.c builds
   what the menus show. Layer: multiplayer/ (session/map state machine). The game
   thread owns the state; the worker sees commands, intents and links only through
   the lock. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "session.h"
#include "map_catalog.h"
#include "menu_layout.h"
#include "menu_texts.h"
#include "menu_view.h"
#include "../game/ui.h"

#define SESSION_CONNECT_TIMEOUT_MS 15000u /* Server start (5 s) plus the connection. */
#define SESSION_LOAD_TIMEOUT_MS    10000u
#define SESSION_BARRIER_TIMEOUT_MS (MP_LOBBY_LOAD_TIMEOUT_MS + 10000u)
#define ROOM_MENU_GRACE_MS         3000u
#define MAIN_MENU_MAP              "maps\\mainmenu.map"
#define LOCAL_HOST_ADDRESS         "127.0.0.1"

/* Menu entities: VID index and the raw direction byte of the menu record. Both menus
   keep their status text at byte 0, where the script's MenuFind(2, 0) finds it (live
   check: a text at byte 25 was not found). The room menu is told apart by its action
   label, which the script never looks up (menu_layout.h). */
#define STATUS_VID         MENU_VID_STATUS
#define STATUS_DIR         MENU_DIR_STATUS
#define ROOM_MARKER_VID    MENU_VID_LABEL
#define ROOM_MARKER_DIR    MENU_DIR_ACTION_LABEL
#define NICKNAME_VID       4u
#define NICKNAME_DIR       2u
#define ADDRESS_VID        4u
#define ADDRESS_DIR        3u
#define CONNECT_BUTTON_VID 705u
#define CONNECT_BUTTON_DIR 25u
#define ROOT_BRIDGE_VID    705u
#define ROOT_BRIDGE_DIR    204u

/* The in-level hint while the other players load, in screen pixels. */
#define HINT_X      24.0f
#define HINT_Y      40.0f /* DrawText draws above y; 24 clipped the first line live. */
#define HINT_YELLOW 0xFFFFE060u

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

    /* Game-thread copy of the room for the menu. */
    struct
    {
        int known;
        MpLobby lobby;
        uint32_t local_id;
        char local_address[16];
    } room;

    /* What the room menu needs besides the snapshot. */
    struct
    {
        unsigned int available_modes; /* CATALOG_MODE_BIT per mode this copy has */
        int was_ready;                /* Guest: seen ready at ready_revision. */
        uint32_t ready_revision;
        MenuTextCache texts;
    } room_menu;

    /* The server this connection uses, for the room header. */
    struct
    {
        char address[16];
        unsigned short port;
    } server;

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
    SessionEngine api = {ui_menu_item, ui_text,        ui_menu_status, ui_button,
                         ui_load_map,  ui_screen_text, ui_set_text,    ui_set_text_file};
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
        memset(&session.room_menu, 0, sizeof(session.room_menu));
        session.room_menu.available_modes = CATALOG_ALWAYS_AVAILABLE;
        strcpy_s(session.server.address, sizeof(session.server.address), host);
        session.server.port = port;
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
    uintptr_t status = session.engine.menu_item(game, STATUS_VID, STATUS_DIR);
    if (session.engine.menu_item(game, ROOM_MARKER_VID, ROOM_MARKER_DIR))
        view->room_status = status;
    else
        view->form_status = status;

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
        strcpy_s(session.room.local_address, sizeof(session.room.local_address),
                 link->link.local_address);
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
/* Player names travel in capitals: the menu fonts draw lower case Latin in another
   style, and a name saved by an older build may still hold lower case. */
static void to_capitals(char* name)
{
    for (; *name; ++name)
    {
        if (*name >= 'a' && *name <= 'z')
            *name = (char)(*name - 'a' + 'A');
    }
}

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
    to_capitals(name);
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

/* The settings the room menu shows: the host's pending choice, else the room's map.
   Returns 0 when that map is not in the catalogue. */
static int shown_settings(MapChoice* choice)
{
    const char* map = session.room.lobby.map;
    if (session.intent.settings_wanted && local_is_host())
        map = session.intent.settings_map;
    return map_catalog_parse(map, choice);
}

/* Host: steps the mode (mode_step) or the mission (mission_step) and asks the room for
   the new map. Ignored outside an open room. */
static void step_settings(int mode_step, int mission_step)
{
    if (!local_is_host() || session.room.lobby.phase != MP_LOBBY_OPEN)
        return;

    MapChoice choice = {CATALOG_CAMPAIGN, 1u};
    shown_settings(&choice);
    if (mode_step)
        choice = map_catalog_step_mode(choice, mode_step, session.room_menu.available_modes);
    else
        choice = map_catalog_step_mission(choice, mission_step);

    char map[MP_MAX_MAP_NAME_LEN];
    if (!map_catalog_map_path(&choice, map, sizeof(map)))
        return;
    session.intent.settings_wanted = 1;
    strcpy_s(session.intent.settings_map, sizeof(session.intent.settings_map), map);
}

/* A guest that is ready keeps its character; nobody changes it during a start. */
static int character_unlocked(void)
{
    const MpLobbyEntry* entry = local_entry();
    if (!entry || session.room.lobby.phase != MP_LOBBY_OPEN)
        return 0;
    return local_is_host() || !(entry->flags & MP_LOBBY_ENTRY_READY);
}

static void want_character(uint32_t character)
{
    session.intent.character_wanted = 1;
    session.intent.character_value = character;
}

/* A click on the hero selector; ignored while the character is locked. */
static void choose_character(uint32_t character)
{
    if (character_unlocked())
        want_character(character);
}

/* The script's first code in a new room menu: add-on maps and the saved character. */
static void apply_setup(int flags)
{
    unsigned int modes = CATALOG_ALWAYS_AVAILABLE;
    if (flags & SM_SETUP_ADDON)
        modes |= CATALOG_MODE_BIT(CATALOG_ADDON);
    if (flags & SM_SETUP_ADDON2)
        modes |= CATALOG_MODE_BIT(CATALOG_ADDON2);
    session.room_menu.available_modes = modes;
    want_character((flags & SM_SETUP_FEMALE) ? MP_CHARACTER_FEMALE : MP_CHARACTER_MALE);
}

/* The action button: the host starts, a guest toggles ready. `enabled` is the view's
   rule (everyone ready, the maps present). */
static void press_action(int enabled)
{
    const MpLobbyEntry* entry = local_entry();
    if (!enabled || !entry || session.room.lobby.phase != MP_LOBBY_OPEN)
        return;

    if (local_is_host())
    {
        session.intent.start_wanted = 1;
        session.intent.start_room = session.room.lobby.room;
        session.intent.start_revision = session.room.lobby.revision;
        return;
    }
    session.intent.ready_wanted = 1;
    session.intent.ready_value = (entry->flags & MP_LOBBY_ENTRY_READY) ? 0u : 1u;
    session.intent.ready_settings_revision = session.room.lobby.settings_revision;
}

/* Handles one request code of the room menu. Returns 1 when the player left. */
static int consume_room_request(const MenuView* menu, const LobbyView* view)
{
    int code = read_status(menu->room_status);
    if (code == SM_LOBBY_LEAVE)
    {
        session.engine.status(menu->room_status, SM_IDLE);
        disconnect(SS_OFFLINE, 0);
        return 1;
    }

    if (code == SM_LOBBY_ACTION)
        press_action(view->action_enabled);
    else if (code == SM_LOBBY_MODE_PREV || code == SM_LOBBY_MODE_NEXT)
        step_settings(code == SM_LOBBY_MODE_NEXT ? 1 : -1, 0);
    else if (code == SM_LOBBY_MISSION_PREV || code == SM_LOBBY_MISSION_NEXT)
        step_settings(0, code == SM_LOBBY_MISSION_NEXT ? 1 : -1);
    else if (code == SM_LOBBY_HERO_MALE || code == SM_LOBBY_HERO_FEMALE)
        choose_character(code == SM_LOBBY_HERO_FEMALE ? MP_CHARACTER_FEMALE : MP_CHARACTER_MALE);
    else if (code >= SM_LOBBY_SETUP && code <= SM_SETUP_LAST)
        apply_setup(code - SM_LOBBY_SETUP);
    else
        return 0; /* A display code: nothing to do. */

    session.engine.status(menu->room_status, SM_IDLE);
    return 0;
}

/* Guest: remembers when it was ready, to explain a reset caused by new settings. */
static int settings_changed_since_ready(void)
{
    const MpLobbyEntry* entry = local_entry();
    if (!entry || local_is_host())
        return 0;
    if (entry->flags & MP_LOBBY_ENTRY_READY)
    {
        session.room_menu.was_ready = 1;
        session.room_menu.ready_revision = session.room.lobby.settings_revision;
        return 0;
    }
    return session.room_menu.was_ready &&
           session.room_menu.ready_revision != session.room.lobby.settings_revision;
}

/* The character the room menu shows: the wanted one until the room shows it. */
static uint32_t shown_character(void)
{
    if (session.intent.character_wanted)
        return session.intent.character_value;
    const MpLobbyEntry* entry = local_entry();
    return entry ? entry->character : MP_CHARACTER_MALE;
}

/* Builds the room view from the snapshot and the menu's text bank. */
static void build_room_view(const MenuView* menu, LobbyView* view)
{
    MenuBankContext context = {&session.engine, menu->game};
    TextBank bank = menu_texts_bank(&context);
    int host = local_is_host();

    LobbyViewInput input;
    memset(&input, 0, sizeof(input));
    input.lobby = &session.room.lobby;
    input.local_id = session.room.local_id;
    input.settings_known = shown_settings(&input.settings);
    input.character = shown_character();
    input.available_modes = session.room_menu.available_modes;
    input.settings_changed = settings_changed_since_ready();
    input.address = host ? session.room.local_address : session.server.address;
    input.port = session.server.port;
    menu_view_lobby(&input, &bank, view);
}

/* Shows the text of the form's status code. */
static void present_form(const MenuView* menu, DWORD now)
{
    if (!menu->form_status)
        return;
    MenuBankContext context = {&session.engine, menu->game};
    TextBank bank = menu_texts_bank(&context);
    char text[MENU_VIEW_TEXT_MAX];
    enum MenuStatus code = (enum MenuStatus)read_status(menu->form_status);
    menu_view_form_status(code, session.server.port, now, &bank, text, sizeof(text));
    menu_texts_set(&session.engine, menu->game, MENU_VID_CAPTION, MENU_DIR_STATUS_TEXT, text);
}

/* The script code that releases the menu for this map: a catalogue map names its mode
   and mission, so the script can save the GameType and LevelNumber the level reads. */
static enum MenuStatus release_code(const char* map)
{
    MapChoice choice;
    if (!map_catalog_parse(map, &choice))
        return SM_RELEASE_MENU;
    int code =
        SM_RELEASE_MISSION_BASE + (int)choice.mode * SM_RELEASE_MODE_STRIDE + (int)choice.mission;
    return (enum MenuStatus)code;
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

    show(menu, release_code(session.start.map));
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
    if (!session.room.known)
        return;

    LobbyView view;
    build_room_view(menu, &view);
    if (consume_room_request(menu, &view))
        return;

    const MpLobbyEntry* entry = local_entry();
    int participant = entry && (entry->flags & MP_LOBBY_ENTRY_PARTICIPANT);
    if (session.room.lobby.phase == MP_LOBBY_STARTING && participant &&
        session.room.lobby.run_epoch != session.start.run_epoch)
    {
        begin_room_load(menu);
        return;
    }
    if (read_status(menu->room_status) != (int)view.code)
        session.engine.status(menu->room_status, view.code);
    menu_texts_show_lobby(&session.engine, menu->game, menu->button, &view,
                          &session.room_menu.texts);
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
        if (!loaded)
            present_form(&menu, now);
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

void session_draw(void)
{
    if (!InterlockedCompareExchange(&session.enabled, 0, 0))
        return;
    if (GetCurrentThreadId() != session.game_thread)
        return;

    if (session.state == SS_WAIT_PLAYERS && session.engine.draw_text)
        session.engine.draw_text(HINT_X, HINT_Y, "Waiting for the other players to load...",
                                 HINT_YELLOW);
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
