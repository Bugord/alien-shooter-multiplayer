#ifndef ASMP_STEAM_SESSION_H
#define ASMP_STEAM_SESSION_H
#include <windows.h>
#include <stdint.h>
typedef struct SteamConnectionRequest { unsigned int generation; int connect; char host[16], name[16]; unsigned short port; } SteamConnectionRequest;
int steam_session_enable(uintptr_t base);
/* Before tick installation only: optional launcher's direct connection. */
int steam_session_direct(const char* host, unsigned short port, const char* name);
/* Worker consumes commands and publishes the connection result. */
int steam_session_take_request(SteamConnectionRequest* request);
void steam_session_result(unsigned int generation, int connected, const char* map);
/* Post-native update: reads legacy menu and loads the server-selected map. */
int steam_session_tick(void);
void steam_session_stop(void);
#endif
