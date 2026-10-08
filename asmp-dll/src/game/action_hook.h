#ifndef ASMP_ACTION_HOOK_H
#define ASMP_ACTION_HOOK_H
#include "actor.h"
typedef void (*ShotCallback)(int, int, unsigned int);
typedef int (*ReplicaFilter)(uintptr_t entity);
int action_hook_install(uintptr_t base, ReplicaFilter, ShotCallback);
int action_hook_stop(void);
#endif
