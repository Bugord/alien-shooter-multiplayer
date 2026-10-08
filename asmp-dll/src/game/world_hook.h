#ifndef ASMP_WORLD_HOOK_H
#define ASMP_WORLD_HOOK_H
#include "slot_hook.h"
/* Observe map loads even when GAME, map name and level start time are reused. */
int world_hook_install(uintptr_t base);
int world_hook_stop(void);
uint32_t world_hook_generation(void);
#endif
