#ifndef ASMP_STEAM_WORLD_HOOK_H
#define ASMP_STEAM_WORLD_HOOK_H
#include "steam_slot_hook.h"
/* Observe map loads even when GAME, map name and level start time are reused. */
int steam_world_hook_install(uintptr_t base);
int steam_world_hook_stop(void);
uint32_t steam_world_hook_generation(void);
#endif
