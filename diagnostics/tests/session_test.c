/* Checks for asmp-dll/src/multiplayer/session.c with a mock menu: form validation,
   host and join commands, the room (ready, start, load barrier), cancelled starts,
   the host leaving, late joiners, leaving the room, the diagnostic direct join, load
   timeout, lost connections and native faults. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../asmp-dll/src/multiplayer/session.h"
#include "../../common/src/protocol.h"

#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define GAME          123u
#define NICKNAME_ITEM 1u
#define ADDRESS_ITEM  2u
#define BUTTON_ITEM   3u
#define LEVEL_MAP     "maps\\Level_01.map"
#define HOST_ID       0u
#define GUEST_ID      1u

static unsigned char form_entity[0x90], room_entity[0x90], root_entity[0x90];
static int form_menu, room_menu, root_menu, disabled, loads, fault, drawn_lines;
static char nickname[32] = "Tester", address[40] = "127.0.0.1:27020", loaded_map[128];
static Snapshot sample;
static unsigned int generation;
static MpLobby room;

static uintptr_t item(uintptr_t game, unsigned int vid, unsigned int direction)
{
    CHECK(game == GAME);
    if (fault)
        RaiseException(0xE0000001u, 0, 0, NULL);
    if (root_menu && vid == 705 && direction == 204)
        return (uintptr_t)root_entity;
    if (room_menu && vid == 2 && direction == 25)
        return (uintptr_t)room_entity;
    if (!form_menu)
        return 0;
    if (vid == 2 && direction == 0)
        return (uintptr_t)form_entity;
    if (vid == 4 && direction == 2)
        return NICKNAME_ITEM;
    if (vid == 4 && direction == 3)
        return ADDRESS_ITEM;
    if (vid == 705 && direction == 25)
        return BUTTON_ITEM;
    return 0;
}

static int text(uintptr_t entity, char* out, unsigned int capacity)
{
    const char* source = entity == NICKNAME_ITEM ? nickname : address;
    if (strlen(source) >= capacity)
        return 0;
    strcpy_s(out, capacity, source);
    return 1;
}

static void status(uintptr_t entity, int value)
{
    *(int*)(entity + STEAM_ENTITY_HEALTH_OFFSET) = value;
}

static int read_code(unsigned char* entity)
{
    return *(int*)(entity + STEAM_ENTITY_HEALTH_OFFSET);
}

static void button(uintptr_t entity, int value)
{
    CHECK(entity == BUTTON_ITEM);
    disabled = value;
}

static int load(uintptr_t game, const char* map)
{
    CHECK(game == GAME);
    ++loads;
    strcpy_s(loaded_map, sizeof(loaded_map), map);
    return 1;
}

static void draw_text(float x, float y, const char* line, uint32_t color)
{
    (void)x;
    (void)y;
    (void)color;
    CHECK(line && line[0]);
    ++drawn_lines;
}

static int tick(DWORD now)
{
    return session_tick(&sample, PROBE_OK, now);
}

static void reset(void)
{
    session_stop();
    SessionEngine api = {item, text, status, button, load, draw_text};
    CHECK(session_initialize(&api));
    CHECK(!session_initialize(&api));
    memset(&sample, 0, sizeof(sample));
    sample.game = GAME;
    status((uintptr_t)form_entity, SM_IDLE);
    status((uintptr_t)room_entity, SM_IDLE);
    status((uintptr_t)root_entity, SM_IDLE);
    form_menu = 1;
    room_menu = root_menu = disabled = loads = fault = drawn_lines = 0;
    strcpy_s(nickname, sizeof(nickname), "Tester");
    strcpy_s(address, sizeof(address), "127.0.0.1:27020");
}

static void enter_level(void)
{
    sample.in_level = 1;
    mp_world_key(LEVEL_MAP, &sample.world_low, &sample.world_high);
    form_menu = room_menu = 0;
}

/* A two-player room; `local` is this client's id. */
static void make_room(uint32_t phase, uint32_t run_epoch)
{
    memset(&room, 0, sizeof(room));
    room.room = 77u;
    room.revision = 5u;
    room.settings_revision = 1u;
    room.phase = phase;
    room.host_id = HOST_ID;
    room.host_session = 10u;
    room.run_epoch = run_epoch;
    room.mode = MP_MODE_CAMPAIGN;
    room.compat = MP_COMPAT_ID;
    room.capacity = 4u;
    strcpy_s(room.map, sizeof(room.map), LEVEL_MAP);
    room.count = 2u;
    room.entries[0].id = HOST_ID;
    room.entries[0].session = 10u;
    strcpy_s(room.entries[0].name, sizeof(room.entries[0].name), "Host");
    room.entries[1].id = GUEST_ID;
    room.entries[1].session = 20u;
    strcpy_s(room.entries[1].name, sizeof(room.entries[1].name), "Guest");
    if (phase == MP_LOBBY_STARTING || phase == MP_LOBBY_IN_GAME)
    {
        room.entries[0].flags = MP_LOBBY_ENTRY_PARTICIPANT;
        room.entries[1].flags = MP_LOBBY_ENTRY_PARTICIPANT;
    }
    CHECK(mp_lobby_valid(&room));
}

static void publish(uint32_t local, enum SessionFailure failure)
{
    SessionLink link;
    memset(&link, 0, sizeof(link));
    link.connected = failure == SF_NONE;
    link.failure = failure;
    link.lobby_known = 1;
    link.lobby = room;
    link.local_id = local;
    session_publish_link(generation, &link);
}

static LobbyIntent intent(void)
{
    LobbyIntent value;
    CHECK(session_read_intent(&value));
    return value;
}

/* Presses a form button and returns the queued command. */
static ConnectionRequest submit(enum MenuStatus code, DWORD now)
{
    ConnectionRequest command;
    status((uintptr_t)form_entity, code);
    tick(now);
    CHECK(session_take_request(&command));
    generation = command.generation;
    return command;
}

/* Connects through the form and enters the room menu as `local`. */
static void join_room(uint32_t local, DWORD now)
{
    ConnectionRequest command = submit(SM_CONNECT_REQUESTED, now);
    CHECK(command.command == SESSION_CONNECT && command.lobby);
    tick(now + 1);
    CHECK(session_state() == SS_CONNECTING);
    make_room(MP_LOBBY_OPEN, 0u);
    publish(local, SF_NONE);
    tick(now + 2);
    CHECK(session_state() == SS_LOBBY && read_code(form_entity) == SM_ENTER_LOBBY);
    form_menu = 0;
    room_menu = 1;
    tick(now + 3);
}

static void check_form(void)
{
    ConnectionRequest command;
    reset();
    nickname[0] = 0;
    status((uintptr_t)form_entity, SM_CONNECT_REQUESTED);
    tick(1);
    CHECK(read_code(form_entity) == SM_BAD_NICKNAME && !session_take_request(&command));
    strcpy_s(nickname, sizeof(nickname), "Tester");
    strcpy_s(address, sizeof(address), "127.0.0.1:");
    status((uintptr_t)form_entity, SM_CONNECT_REQUESTED);
    tick(2);
    CHECK(read_code(form_entity) == SM_BAD_ADDRESS);
    strcpy_s(address, sizeof(address), ":27020"); /* Joining needs a host. */
    status((uintptr_t)form_entity, SM_CONNECT_REQUESTED);
    tick(3);
    CHECK(read_code(form_entity) == SM_BAD_ADDRESS);

    strcpy_s(address, sizeof(address), "10.0.0.5");
    command = submit(SM_CONNECT_REQUESTED, 4);
    CHECK(!strcmp(command.host, "10.0.0.5") && command.port == MP_DEFAULT_PORT);
    CHECK(read_code(form_entity) == SM_CONNECTING && disabled);
    tick(5);
    publish(0u, SF_NO_ANSWER);
    tick(6);
    tick(7);
    CHECK(session_state() == SS_FAILED && read_code(form_entity) == SM_NO_ANSWER && !disabled);
    CHECK(session_take_request(&command) && command.command == SESSION_DISCONNECT);

    /* Hosting ignores the host part and only takes an optional port. */
    strcpy_s(address, sizeof(address), ":27031");
    command = submit(SM_HOST_REQUESTED, 8);
    CHECK(command.command == SESSION_HOST && command.port == 27031 && command.lobby);
    CHECK(!strcmp(command.host, "127.0.0.1"));
    tick(9);
    publish(0u, SF_PORT_IN_USE);
    tick(10);
    CHECK(read_code(form_entity) == SM_PORT_IN_USE);
    tick(11);
    address[0] = 0;
    command = submit(SM_HOST_REQUESTED, 12);
    CHECK(command.port == MP_DEFAULT_PORT);
}

static void check_guest_start(void)
{
    reset();
    join_room(GUEST_ID, 100);
    CHECK(read_code(room_entity) == SM_LOBBY_NOT_READY);
    session_draw();
    CHECK(drawn_lines == 3); /* Title and two players. */

    status((uintptr_t)room_entity, SM_LOBBY_ACTION);
    tick(104);
    LobbyIntent wanted = intent();
    CHECK(wanted.ready_wanted && wanted.ready_value == 1u && !wanted.start_wanted);
    CHECK(wanted.ready_settings_revision == 1u);
    room.entries[1].flags = MP_LOBBY_ENTRY_READY;
    ++room.revision;
    publish(GUEST_ID, SF_NONE);
    tick(105);
    CHECK(read_code(room_entity) == SM_LOBBY_READY);

    make_room(MP_LOBBY_STARTING, 1u);
    publish(GUEST_ID, SF_NONE);
    tick(106);
    CHECK(session_state() == SS_RELEASE_MENU && read_code(room_entity) == SM_RELEASE_MENU);
    room_menu = 0;
    tick(107);
    CHECK(tick(108) == 1 && loads == 1 && !strcmp(loaded_map, LEVEL_MAP));
    CHECK(session_state() == SS_WAIT_LEVEL);
    enter_level();
    tick(109);
    CHECK(session_state() == SS_WAIT_PLAYERS);
    wanted = intent();
    CHECK(wanted.loaded_room == 77u && wanted.loaded_run_epoch == 1u && !wanted.ready_wanted);
    drawn_lines = 0;
    session_draw();
    CHECK(drawn_lines == 1);

    make_room(MP_LOBBY_IN_GAME, 1u);
    publish(GUEST_ID, SF_NONE);
    tick(110);
    CHECK(session_state() == SS_IN_GAME && loads == 1);
    tick(111); /* The same start never loads twice. */
    CHECK(loads == 1);

    /* The host leaving ends the game and returns to the menu with a reason. */
    make_room(MP_LOBBY_CLOSED, 1u);
    room.count = 1u;
    room.entries[0] = room.entries[1];
    room.entries[0].flags = 0;
    room.host_id = UINT32_MAX;
    room.host_session = 0;
    publish(GUEST_ID, SF_NONE);
    tick(112);
    CHECK(session_state() == SS_DISCONNECTING);
    CHECK(tick(113) == 1 && loads == 2 && !strcmp(loaded_map, "maps\\mainmenu.map"));
    sample.in_level = 0;
    root_menu = 1;
    tick(114);
    CHECK(read_code(root_entity) == SM_CONNECTION_LOST);
    form_menu = 1;
    tick(115);
    CHECK(session_state() == SS_FAILED && read_code(form_entity) == SM_HOST_LEFT);
}

static void check_host_and_cancel(void)
{
    reset();
    join_room(HOST_ID, 200);
    CHECK(read_code(room_entity) == SM_LOBBY_HOST_WAITING);
    room.entries[1].flags = MP_LOBBY_ENTRY_READY;
    ++room.revision;
    publish(HOST_ID, SF_NONE);
    tick(204);
    CHECK(read_code(room_entity) == SM_LOBBY_HOST_CAN_START);
    status((uintptr_t)room_entity, SM_LOBBY_ACTION);
    tick(205);
    LobbyIntent wanted = intent();
    CHECK(wanted.start_wanted && wanted.start_room == 77u && wanted.start_revision == 6u);

    make_room(MP_LOBBY_STARTING, 1u);
    publish(HOST_ID, SF_NONE);
    tick(206);
    room_menu = 0;
    tick(207);
    CHECK(tick(208) == 1 && loads == 1);
    enter_level();
    tick(209);
    CHECK(session_state() == SS_WAIT_PLAYERS);

    /* The server cancels: back to the main menu, then into the room. */
    make_room(MP_LOBBY_OPEN, 1u);
    room.notice = MP_LOBBY_NOTICE_LOAD_TIMEOUT;
    publish(HOST_ID, SF_NONE);
    CHECK(tick(210) == 1 && loads == 2 && session_state() == SS_RETURNING_LOBBY);
    CHECK(intent().loaded_run_epoch == 0u);
    sample.in_level = 0;
    root_menu = 1;
    tick(211);
    CHECK(read_code(root_entity) == SM_RETURN_LOBBY && session_state() == SS_RETURNING_LOBBY);
    root_menu = 0;
    form_menu = 1;
    tick(212);
    CHECK(session_state() == SS_LOBBY && read_code(form_entity) == SM_ENTER_LOBBY);
    form_menu = 0;
    room_menu = 1;
    tick(213);
    CHECK(read_code(room_entity) == SM_LOBBY_HOST_WAITING);
    drawn_lines = 0;
    session_draw();
    CHECK(drawn_lines == 4); /* Title, two players and the notice. */

    /* A new start with a new epoch loads again. */
    make_room(MP_LOBBY_STARTING, 2u);
    publish(HOST_ID, SF_NONE);
    tick(214);
    CHECK(session_state() == SS_RELEASE_MENU);
}

static void check_late_join_and_leave(void)
{
    ConnectionRequest command;
    reset();
    join_room(GUEST_ID, 300);
    make_room(MP_LOBBY_IN_GAME, 3u);
    room.entries[1].flags = 0; /* Joined after the start. */
    publish(GUEST_ID, SF_NONE);
    tick(304);
    CHECK(session_state() == SS_LOBBY && read_code(room_entity) == SM_LOBBY_IN_PROGRESS);
    CHECK(loads == 0);

    status((uintptr_t)room_entity, SM_LOBBY_LEAVE);
    tick(305);
    tick(306);
    CHECK(session_state() == SS_OFFLINE && session_take_request(&command));
    CHECK(command.command == SESSION_DISCONNECT);

    /* Leaving the menus without the leave button also leaves after a grace period. */
    reset();
    join_room(GUEST_ID, 400);
    room_menu = 0;
    tick(404);
    tick(404 + 2999);
    CHECK(session_state() == SS_LOBBY);
    tick(404 + 3000);
    CHECK(session_state() == SS_DISCONNECTING);

    /* A closed room in the menu shows why. */
    reset();
    join_room(GUEST_ID, 500);
    room.phase = MP_LOBBY_CLOSED;
    publish(GUEST_ID, SF_NONE);
    tick(504);
    CHECK(read_code(room_entity) == SM_HOST_LEFT);
    tick(505);
    CHECK(session_state() == SS_FAILED);
}

static void check_direct_and_faults(void)
{
    ConnectionRequest command;
    SessionLink link;
    memset(&link, 0, sizeof(link));
    link.connected = 1;
    strcpy_s(link.map, sizeof(link.map), "maps/LEVEL_01.MAP");

    reset();
    CHECK(session_direct("127.0.0.1", 27020, "Direct"));
    form_menu = 0;
    tick(20);
    CHECK(session_take_request(&command) && !command.lobby);
    session_publish_link(command.generation, &link);
    tick(21);
    CHECK(session_state() == SS_LOADING);
    tick(22);
    enter_level();
    tick(23);
    CHECK(session_state() == SS_IN_GAME && loads == 1);
    link.connected = 0;
    link.failure = SF_LOST;
    session_publish_link(command.generation, &link);
    tick(24);
    CHECK(session_state() == SS_DISCONNECTING);
    CHECK(tick(25) == 1 && loads == 2 && !strcmp(loaded_map, "maps\\mainmenu.map"));
    CHECK(session_take_request(&command) && command.command == SESSION_DISCONNECT);

    reset();
    CHECK(session_direct("127.0.0.1", 27020, "Fault"));
    tick(30);
    CHECK(session_take_request(&command));
    fault = 1;
    CHECK(tick(31) == -1);
    tick(32);
    CHECK(session_state() == SS_ABANDONED);
    CHECK(session_take_request(&command) && command.command == SESSION_DISCONNECT);
    tick(33); /* No native retry while the fault flag remains. */

    reset();
    CHECK(session_direct("127.0.0.1", 27020, "LoadTimeout"));
    form_menu = 0;
    tick(40);
    CHECK(session_take_request(&command));
    link.connected = 1;
    link.failure = SF_NONE;
    session_publish_link(command.generation, &link);
    tick(41);
    tick(42);
    tick(10042);
    CHECK(session_state() == SS_DISCONNECTING);
    session_stop();
    CHECK(!session_take_request(&command));
}

int main(void)
{
    check_form();
    check_guest_start();
    check_host_and_cancel();
    check_late_join_and_leave();
    check_direct_and_faults();
    puts("Session checks passed: form validation, host and join commands, failure reasons, "
         "room ready/start, load barrier, cancelled start back to the room, host left, late "
         "joiner, leave, direct join, load timeout, lost connection and native fault.");
    return 0;
}
