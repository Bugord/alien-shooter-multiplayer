#ifndef ASMP_STEAM_DISPLAY_HOOK_H
#define ASMP_STEAM_DISPLAY_HOOK_H
#include "steam_profile.h"
typedef void (*SteamDrawCallback)(void);
int steam_display_hook_install(uintptr_t base, SteamDrawCallback draw);
int steam_display_hook_ready(void);
long steam_display_hook_frames(void);
int steam_display_hook_stop(void);
#endif
