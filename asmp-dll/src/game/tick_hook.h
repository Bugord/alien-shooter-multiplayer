#ifndef ASMP_TICK_HOOK_H
#define ASMP_TICK_HOOK_H
#include "slot_hook.h"
typedef void (*TickCallback)(void* game, LONG tick, DWORD now);
enum SlotResult tick_hook_install(void* volatile* slot, void* original, TickCallback callback);
enum SlotResult tick_hook_stop(void);
LONG tick_hook_calls(void);
int tick_hook_ready(void);
#endif
