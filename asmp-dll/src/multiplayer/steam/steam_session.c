#if defined(ASMP_STEAM_BUILD)
#include <stdlib.h>
#include <string.h>
#include "steam_session.h"
#include "../../game/steam/steam_ui.h"
static uintptr_t base;
static volatile LONG enabled;
static int connecting, loaded, saw_gameplay, saw_menu, result_ready;
static unsigned int generation, result_generation;
static SteamConnectionRequest command;
static int pending, connection;
static char server_map[24];
static SRWLOCK lock = SRWLOCK_INIT;
int steam_session_enable(uintptr_t image) {
    if (!steam_ui_bind(image)) return 0;
    base = image; InterlockedExchange(&enabled, 1); return 1;
}
static int request(const char* host, unsigned short port, const char* name, int connect) {
    if (!TryAcquireSRWLockExclusive(&lock)) return 0;
    memset(&command, 0, sizeof(command));
    if (!++generation) ++generation;
    command.generation = generation; command.connect = connect; command.port = port;
    if (connect) { strcpy_s(command.host, sizeof(command.host), host); strcpy_s(command.name, sizeof(command.name), name); }
    pending = 1; result_ready = 0; connecting = connect; loaded = saw_gameplay = saw_menu = 0;
    ReleaseSRWLockExclusive(&lock); return 1;
}
int steam_session_direct(const char* host, unsigned short port, const char* name) {
    if (!host || strlen(host) >= 16 || !name || !*name || strlen(name) >= 16 || !port) return 0;
    return request(host, port, name, 1);
}
int steam_session_take_request(SteamConnectionRequest* out) {
    AcquireSRWLockExclusive(&lock); int ready = pending;
    if (ready) { *out = command; pending = 0; }
    ReleaseSRWLockExclusive(&lock); return ready;
}
void steam_session_result(unsigned int tag, int connected, const char* map) {
    AcquireSRWLockExclusive(&lock);
    if (tag == generation) {
        connection = connected; result_generation = tag; result_ready = 1;
        if (map) strcpy_s(server_map, sizeof(server_map), map); else server_map[0] = 0;
    }
    ReleaseSRWLockExclusive(&lock);
}
static int parse_address(char* address, unsigned short* port) {
    char* colon = strrchr(address, ':'); if (!colon) return 0;
    char* end; unsigned long value = strtoul(colon + 1, &end, 10);
    if (!value || value > 65535 || *end || colon == address) return 0;
    *colon = 0;
    /* Match the original IPv4-only menu. Transport validates the address. */
    if (strlen(address) >= 16) return 0;
    *port = (unsigned short)value; return 1;
}
int steam_session_tick(void) {
    LONG mode = InterlockedCompareExchange(&enabled, 0, 0);
    if (!mode) return 0;
    /* After a native fault, only queue disconnection; do not retry game writes. */
    if (mode == 2) {
        if (request(NULL, 0, NULL, 0)) InterlockedExchange(&enabled, 0);
        return 0;
    }
    __try {
        uintptr_t game = *(uintptr_t*)(base + STEAM_GAME_PTR_RVA);
        if (!game || *(uintptr_t*)game != base + STEAM_GAME_VTABLE_RVA) return 0;
        uintptr_t status = steam_ui_menu_item(game, 2, 0);
        uintptr_t nickname = steam_ui_menu_item(game, 4, 2);
        uintptr_t address = steam_ui_menu_item(game, 4, 3);
        uintptr_t button = steam_ui_menu_item(game, 705, 25);
        if (status && nickname && address && button) {
            saw_menu = 1;
            int state = *(int*)(status + STEAM_ENTITY_HEALTH_OFFSET);
            if (state == 1) {
                char name[16], host[32]; unsigned short port;
                if (!steam_ui_text(nickname, name, sizeof(name)) || !*name) state = 2;
                else if (!steam_ui_text(address, host, sizeof(host)) || !parse_address(host, &port)) state = 3;
                else if (request(host, port, name, 1)) { state = 4; steam_ui_button(button, 1); }
                steam_ui_menu_status(status, state);
            }
        }
        int ready = 0, connected = 0; char map[24] = {0};
        if (TryAcquireSRWLockShared(&lock)) {
            ready = result_ready && result_generation == generation;
            connected = connection; memcpy(map, server_map, sizeof(map));
            ReleaseSRWLockShared(&lock);
        }
        if (connecting && ready) {
            if (!connected) {
                connecting = 0;
                if (status) steam_ui_menu_status(status, 5);
                if (button) steam_ui_button(button, 0);
            } else if (!loaded) {
                /* Give asmp_play.lgc one update to release the menu, as in SCS_WAIT_MAP_LOAD. */
                if (status && *(int*)(status + STEAM_ENTITY_HEALTH_OFFSET) != 6) {
                    steam_ui_menu_status(status, 6); return 0;
                }
                loaded = 1;
                if (steam_ui_load_map(game, map)) return 1;
                InterlockedExchange(&enabled, 2); return -1;
            }
        }
        const char* map_name = *(const char**)(game + 0x20);
        if (loaded && map_name && (!_strnicmp(map_name, "maps\\Level_", 11) ||
            !_strnicmp(map_name, "maps\\survive_", 13))) saw_gameplay = 1;
        /* Main-menu assets retain the original multiplayer button (dir 204).
           Return from gameplay disconnects without touching campaign/shop menus. */
        if (connecting && (saw_gameplay || saw_menu) &&
            steam_ui_menu_item(game, 705, 204)) request(NULL, 0, NULL, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&enabled, 2); return -1; }
    return 0;
}
void steam_session_stop(void) { InterlockedExchange(&enabled, 0); }
#endif
