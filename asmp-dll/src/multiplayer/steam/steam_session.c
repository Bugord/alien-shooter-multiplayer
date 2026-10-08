#if defined(ASMP_STEAM_BUILD)
#include <stdlib.h>
#include <string.h>
#include "steam_session.h"
#include "../../game/steam/steam_ui.h"
#include "../../../../common/src/steam_state_protocol.h"
static struct {
    SteamSessionEngine engine;
    enum SteamSessionState state, after_disconnect;
    volatile LONG enabled;
    unsigned int generation, result_generation;
    SteamConnectionRequest command;
    int pending, result_ready, connection;
    uint32_t world_low, world_high;
    DWORD entered_at;
    char server_map[128];
} session;
static SRWLOCK lock = SRWLOCK_INIT;
int steam_session_initialize(const SteamSessionEngine* api) {
    if (session.enabled || !api || !api->menu_item || !api->text || !api->status || !api->button || !api->load_map) return 0;
    memset(&session, 0, sizeof(session)); session.engine = *api;
    InterlockedExchange(&session.enabled, 1); return 1;
}
int steam_session_enable(uintptr_t base) {
    if (!steam_ui_bind(base)) return 0;
    SteamSessionEngine api = {steam_ui_menu_item, steam_ui_text, steam_ui_menu_status, steam_ui_button, steam_ui_load_map};
    if (steam_session_initialize(&api)) return 1;
    steam_ui_stop(); return 0;
}
static int request(const char* host, unsigned short port, const char* name, int connect) {
    if (!TryAcquireSRWLockExclusive(&lock)) return 0;
    if (!InterlockedCompareExchange(&session.enabled, 0, 0)) { ReleaseSRWLockExclusive(&lock); return 0; }
    memset(&session.command, 0, sizeof(session.command));
    if (!++session.generation) ++session.generation;
    session.command.generation = session.generation; session.command.connect = connect; session.command.port = port;
    if (connect) { strcpy_s(session.command.host, sizeof(session.command.host), host); strcpy_s(session.command.name, sizeof(session.command.name), name); }
    session.pending = 1; session.result_ready = 0;
    ReleaseSRWLockExclusive(&lock); return 1;
}
int steam_session_direct(const char* host, unsigned short port, const char* name) {
    if (!session.enabled || session.state != SS_OFFLINE || !host || strlen(host) >= 16 || !name || !*name || strlen(name) >= 16 || !port) return 0;
    if (!request(host, port, name, 1)) return 0;
    session.state = SS_REQUESTED; return 1;
}
int steam_session_take_request(SteamConnectionRequest* out) {
    AcquireSRWLockExclusive(&lock); int ready = session.pending;
    if (ready) { *out = session.command; session.pending = 0; }
    ReleaseSRWLockExclusive(&lock); return ready;
}
void steam_session_result(unsigned int generation, int connected, const char* map) {
    AcquireSRWLockExclusive(&lock);
    if (session.enabled && generation == session.generation) {
        session.connection = connected; session.result_generation = generation; session.result_ready = 1;
        if (map && strlen(map) < sizeof(session.server_map) && mp_steam_is_level_path(map)) strcpy_s(session.server_map, sizeof(session.server_map), map);
        else { session.server_map[0] = 0; if (connected) session.connection = 0; }
    }
    ReleaseSRWLockExclusive(&lock);
}
static int parse_address(char* address, unsigned short* port) {
    char* colon = strrchr(address, ':'); if (!colon || colon == address) return 0;
    char* end; unsigned long value = strtoul(colon + 1, &end, 10);
    if (!value || value > 65535 || end == colon + 1 || *end) return 0;
    *colon = 0; if (strlen(address) >= 16) return 0;
    *port = (unsigned short)value; return 1;
}
static void disconnect(enum SteamSessionState next) { session.after_disconnect = next; session.state = SS_DISCONNECTING; }
int steam_session_tick(const Snapshot* sample, enum ProbeResult result, DWORD now) {
    if (!InterlockedCompareExchange(&session.enabled, 0, 0)) return 0;
    /* Never make another native call after a native fault. Even if the game is
       unreadable, the worker must receive the disconnect request. */
    if (session.state == SS_DISCONNECTING && session.after_disconnect == SS_ABANDONED) {
        if (request(NULL, 0, NULL, 0)) session.state = SS_ABANDONED;
        return 0;
    }
    if (session.state == SS_ABANDONED || !sample->game || result == PROBE_READ_FAULT || result == PROBE_GAME_TYPE) return 0;
    __try {
        uintptr_t game = sample->game;
        uintptr_t status = session.engine.menu_item(game, 2, 0);
        uintptr_t nickname = session.engine.menu_item(game, 4, 2);
        uintptr_t address = session.engine.menu_item(game, 4, 3);
        uintptr_t button = session.engine.menu_item(game, 705, 25);
        if ((session.state == SS_OFFLINE || session.state == SS_FAILED) && status && nickname && address && button &&
            *(int*)(status + STEAM_ENTITY_HEALTH_OFFSET) == SM_CONNECT_REQUESTED) {
            char name[16], host[32]; unsigned short port;
            if (!session.engine.text(nickname, name, sizeof(name)) || !*name) session.engine.status(status, SM_BAD_NICKNAME);
            else if (!session.engine.text(address, host, sizeof(host)) || !parse_address(host, &port)) session.engine.status(status, SM_BAD_ADDRESS);
            else if (request(host, port, name, 1)) {
                session.state = SS_REQUESTED; session.engine.status(status, SM_CONNECTING); session.engine.button(button, 1);
            }
        }
        int ready = 0, connected = 0; char map[128] = {0};
        if (TryAcquireSRWLockShared(&lock)) {
            ready = session.result_ready && session.result_generation == session.generation;
            connected = session.connection; memcpy(map, session.server_map, sizeof(map));
            ReleaseSRWLockShared(&lock);
        }
        switch (session.state) {
        case SS_REQUESTED: session.entered_at = now; session.state = SS_CONNECTING; break;
        case SS_CONNECTING:
            if ((ready && !connected) || now - session.entered_at >= 10000u) {
                if (status) session.engine.status(status, SM_FAILED);
                if (button) session.engine.button(button, 0);
                disconnect(SS_FAILED);
            } else if (ready && connected) {
                mp_steam_world_key(map, &session.world_low, &session.world_high);
                if (status) { session.engine.status(status, SM_RELEASE_MENU); session.state = SS_RELEASE_MENU; }
                else session.state = SS_LOADING;
            }
            break;
        case SS_RELEASE_MENU: session.state = SS_LOADING; break;
        case SS_LOADING:
            if (!ready || !connected) { disconnect(SS_FAILED); break; }
            if (!session.engine.load_map(game, map)) { disconnect(SS_ABANDONED); return -1; }
            session.entered_at = now; session.state = SS_WAIT_LEVEL; return 1;
        case SS_WAIT_LEVEL:
            if (ready && !connected) disconnect(SS_RETURNING_MENU);
            else if (sample->in_level && sample->world_low == session.world_low && sample->world_high == session.world_high) session.state = SS_IN_GAME;
            else if (now - session.entered_at >= 10000u) disconnect(SS_RETURNING_MENU);
            break;
        case SS_IN_GAME:
            if (ready && !connected) disconnect(SS_RETURNING_MENU);
            else if (!sample->in_level || sample->world_low != session.world_low || sample->world_high != session.world_high) disconnect(SS_OFFLINE);
            break;
        case SS_DISCONNECTING:
            if (request(NULL, 0, NULL, 0)) {
                session.state = session.after_disconnect;
                if (session.state == SS_RETURNING_MENU) {
                    if (!session.engine.load_map(game, "maps\\mainmenu.map")) { session.state = SS_ABANDONED; return -1; }
                    return 1;
                }
            }
            break;
        case SS_RETURNING_MENU: {
            /* Script bridge after map load: root menu routes to ASMP and carries
               the lost-connection message. It is not a world-exit heuristic. */
            uintptr_t root_button = session.engine.menu_item(game, 705, 204);
            if (root_button) session.engine.status(root_button, SM_CONNECTION_LOST);
            if (status) { session.engine.status(status, SM_CONNECTION_LOST); if (button) session.engine.button(button, 0); session.state = SS_FAILED; }
            break;
        }
        default: break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { disconnect(SS_ABANDONED); return -1; }
    return 0;
}
enum SteamSessionState steam_session_state(void) { return InterlockedCompareExchange(&session.enabled, 0, 0) ? session.state : SS_OFFLINE; }
void steam_session_stop(void) {
    InterlockedExchange(&session.enabled, 0);
    AcquireSRWLockExclusive(&lock); session.pending = session.result_ready = 0; ReleaseSRWLockExclusive(&lock);
}
#endif
