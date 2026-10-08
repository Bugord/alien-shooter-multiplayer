#ifndef ASMP_STEAM_ACTION_HOOK_H
#define ASMP_STEAM_ACTION_HOOK_H
#include "steam_actor.h"
typedef void (*SteamShotCallback)(int, int, unsigned int);
typedef int (*SteamReplicaFilter)(uintptr_t entity);
int steam_action_hook_install(uintptr_t base, SteamReplicaFilter, SteamShotCallback);
int steam_action_hook_stop(void);
#endif
