#ifndef ASMP_DISPLAY_HOOK_H
#define ASMP_DISPLAY_HOOK_H
#include "steam_profile.h"
typedef void (*DrawCallback)(void);
enum DisplayResult { DISPLAY_PENDING, DISPLAY_OK, DISPLAY_FAILED };
enum DisplayResult display_hook_install(uintptr_t base, DrawCallback draw);
int display_hook_ready(void);
long display_hook_frames(void);
int display_hook_stop(void);
#endif
