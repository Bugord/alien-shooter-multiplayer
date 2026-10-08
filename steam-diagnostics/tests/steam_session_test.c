#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../asmp-dll/src/multiplayer/steam/steam_session.h"
#include "../../common/src/steam_state_protocol.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned char status_entity[0x90], root_entity[0x90];
static int menu, root_menu, disabled, loads, fault;
static char nickname[32] = "Tester", address[40] = "127.0.0.1:27020", loaded_map[128];
static uintptr_t item(uintptr_t game, unsigned int vid, unsigned int direction) {
    CHECK(game == 123);
    if (fault) RaiseException(0xE0000001u, 0, 0, NULL);
    if (root_menu && vid == 705 && direction == 204) return (uintptr_t)root_entity;
    if (!menu) return 0;
    if (vid == 2) return (uintptr_t)status_entity;
    if (vid == 4 && direction == 2) return 1;
    if (vid == 4 && direction == 3) return 2;
    if (vid == 705 && direction == 25) return 3;
    return 0;
}
static int text(uintptr_t entity, char* out, unsigned int capacity) {
    const char* source = entity == 1 ? nickname : address;
    if (strlen(source) >= capacity) return 0;
    strcpy_s(out, capacity, source); return 1;
}
static void status(uintptr_t entity, int value) { *(int*)(entity + STEAM_ENTITY_HEALTH_OFFSET) = value; }
static void button(uintptr_t entity, int value) { CHECK(entity == 3); disabled = value; }
static int load(uintptr_t game, const char* map) { CHECK(game == 123); ++loads; strcpy_s(loaded_map, sizeof(loaded_map), map); return 1; }
static Snapshot sample;
static int tick(DWORD now) { return steam_session_tick(&sample, PROBE_OK, now); }
static void reset(void) {
    steam_session_stop(); SteamSessionEngine api = {item, text, status, button, load};
    CHECK(steam_session_initialize(&api));
    CHECK(!steam_session_initialize(&api));
    memset(&sample, 0, sizeof(sample)); sample.game = 123;
    status((uintptr_t)status_entity, SM_IDLE); menu = 1; root_menu = disabled = loads = fault = 0;
}
static void press(void) { status((uintptr_t)status_entity, SM_CONNECT_REQUESTED); }
static void enter_level(void) {
    sample.in_level = 1; mp_steam_world_key("maps\\Level_01.map", &sample.world_low, &sample.world_high); menu = 0;
}
int main(void) {
    SteamConnectionRequest command;
    reset(); nickname[0] = 0; press(); tick(1);
    CHECK(*(int*)(status_entity + 0x58) == SM_BAD_NICKNAME && !steam_session_take_request(&command));
    strcpy_s(nickname, sizeof(nickname), "Tester"); strcpy_s(address, sizeof(address), "127.0.0.1:"); press(); tick(2);
    CHECK(*(int*)(status_entity + 0x58) == SM_BAD_ADDRESS);
    strcpy_s(address, sizeof(address), "127.0.0.1:27020"); press(); tick(3);
    CHECK(steam_session_state() == SS_CONNECTING && disabled && steam_session_take_request(&command) && command.connect);
    CHECK(!strcmp(command.host, "127.0.0.1") && command.port == 27020);
    steam_session_result(command.generation + 1, 1, "maps\\Level_01.map"); tick(4); CHECK(steam_session_state() == SS_CONNECTING);
    steam_session_result(command.generation, 0, NULL); tick(5); tick(6);
    CHECK(steam_session_state() == SS_FAILED && !disabled && steam_session_take_request(&command) && !command.connect);
    press(); tick(7); CHECK(steam_session_take_request(&command));
    steam_session_result(command.generation, 1, "maps\\Level_01.map"); tick(8);
    CHECK(steam_session_state() == SS_RELEASE_MENU && *(int*)(status_entity + 0x58) == SM_RELEASE_MENU && !loads);
    menu = 0; tick(9); CHECK(steam_session_state() == SS_LOADING && !loads);
    CHECK(tick(10) == 1 && loads == 1 && steam_session_state() == SS_WAIT_LEVEL);
    tick(11); CHECK(loads == 1 && steam_session_state() == SS_WAIT_LEVEL);
    enter_level(); tick(12); CHECK(steam_session_state() == SS_IN_GAME);
    sample.health = 0; tick(13); CHECK(steam_session_state() == SS_IN_GAME);
    sample.in_level = 0; tick(14); tick(15);
    CHECK(steam_session_state() == SS_OFFLINE && steam_session_take_request(&command) && !command.connect);
    reset(); CHECK(steam_session_direct("127.0.0.1", 27020, "Direct")); menu = 0;
    tick(20); CHECK(steam_session_take_request(&command)); steam_session_result(command.generation, 1, "maps/LEVEL_01.MAP");
    tick(21); CHECK(steam_session_state() == SS_LOADING); tick(22); enter_level(); tick(23);
    CHECK(steam_session_state() == SS_IN_GAME && loads == 1);
    steam_session_result(command.generation, 0, NULL); tick(24); CHECK(steam_session_state() == SS_DISCONNECTING);
    CHECK(tick(25) == 1 && loads == 2 && !strcmp(loaded_map, "maps\\mainmenu.map"));
    CHECK(steam_session_take_request(&command) && !command.connect);
    sample.in_level = 0; root_menu = 1; tick(26); CHECK(*(int*)(root_entity + 0x58) == SM_CONNECTION_LOST);
    root_menu = 0; menu = 1; tick(27); CHECK(steam_session_state() == SS_FAILED && *(int*)(status_entity + 0x58) == SM_CONNECTION_LOST);
    press(); tick(28); CHECK(steam_session_take_request(&command) && command.connect); /* Retry after lost connection. */
    tick(10028); tick(10029); CHECK(steam_session_state() == SS_FAILED && !disabled);
    reset(); CHECK(steam_session_direct("127.0.0.1", 27020, "Fault")); tick(30); CHECK(steam_session_take_request(&command));
    fault = 1; CHECK(tick(31) == -1); tick(32); CHECK(steam_session_state() == SS_ABANDONED);
    CHECK(steam_session_take_request(&command) && !command.connect); tick(33); /* No native retry while fault flag remains. */
    reset(); CHECK(steam_session_direct("127.0.0.1", 27020, "LoadTimeout")); menu = 0;
    tick(40); CHECK(steam_session_take_request(&command)); steam_session_result(command.generation, 1, "maps\\Level_01.map");
    tick(41); tick(42); tick(10042); CHECK(steam_session_state() == SS_DISCONNECTING);
    steam_session_stop(); CHECK(!steam_session_take_request(&command));
    puts("Steam session checks passed: menu validation, failure/retry, direct connection, load confirmation/timeout, exit, loss, stale results and native fault.");
    return 0;
}
