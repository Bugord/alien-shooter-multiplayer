#ifndef ASMP_STEAM_TICK_HOOK_H
#define ASMP_STEAM_TICK_HOOK_H
#include "steam_slot_hook.h"
typedef void (*SteamTickCallback)(void* game, LONG tick, DWORD now);
enum SteamSlotResult steam_tick_hook_install(void* volatile* slot, void* original, SteamTickCallback callback);
enum SteamSlotResult steam_tick_hook_stop(void);
LONG steam_tick_hook_calls(void);
int steam_tick_hook_ready(void);
#endif
