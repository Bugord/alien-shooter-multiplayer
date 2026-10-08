#ifndef ASMP_STEAM_SESSION_H
#define ASMP_STEAM_SESSION_H
#include <windows.h>
#include <stdint.h>
#include "../../game/steam/steam_probe.h"
enum SteamSessionState { SS_OFFLINE, SS_REQUESTED, SS_CONNECTING, SS_RELEASE_MENU,
    SS_LOADING, SS_WAIT_LEVEL, SS_IN_GAME, SS_DISCONNECTING, SS_RETURNING_MENU, SS_FAILED, SS_ABANDONED };
enum SteamMenuStatus { SM_IDLE, SM_CONNECT_REQUESTED, SM_BAD_NICKNAME, SM_BAD_ADDRESS,
    SM_CONNECTING, SM_FAILED, SM_RELEASE_MENU, SM_CONNECTION_LOST };
typedef struct SteamSessionEngine {
    uintptr_t (*menu_item)(uintptr_t, unsigned int, unsigned int);
    int (*text)(uintptr_t, char*, unsigned int);
    void (*status)(uintptr_t, int);
    void (*button)(uintptr_t, int);
    int (*load_map)(uintptr_t, const char*);
} SteamSessionEngine;
typedef struct SteamConnectionRequest { unsigned int generation; int connect; char host[16], name[16]; unsigned short port; } SteamConnectionRequest;
int steam_session_enable(uintptr_t base);
int steam_session_initialize(const SteamSessionEngine*);
enum SteamSessionState steam_session_state(void);
/* Before tick installation only: optional launcher's direct connection. */
int steam_session_direct(const char* host, unsigned short port, const char* name);
/* Worker consumes commands and publishes the connection result. */
int steam_session_take_request(SteamConnectionRequest* request);
void steam_session_result(unsigned int generation, int connected, const char* map);
/* Post-native update: reads legacy menu and loads the server-selected map. */
int steam_session_tick(const Snapshot*, enum ProbeResult, DWORD now);
void steam_session_stop(void);
#endif
