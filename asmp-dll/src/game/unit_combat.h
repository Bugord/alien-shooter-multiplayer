#ifndef ASMP_UNIT_COMBAT_H
#define ASMP_UNIT_COMBAT_H
#include "unit_puppet.h"

typedef struct UnitCombatSample {
    uintptr_t root, child;
    unsigned int private_entities, other_entities, unexpected_entities;
    int sources_unchanged, fault;
} UnitCombatSample;
/* Scoped offline fixture / network copy, one VID50 + VID108. Factory owns native allocations;
   this module owns private shallow VID copies, never the borrowed resources.
   Buffers stay in the pinned DLL through the complete deleting destructors.
   Every function below runs on the game thread. Init binds only, on worker. */
int unit_combat_bind(uintptr_t base, ActorCreate*);
int unit_combat_init(uintptr_t base, ActorCreate, ActorDestroy, UnitAnimation);
int unit_combat_spawn(const Snapshot*);
UnitCombatSample unit_combat_sample(uintptr_t game, int compare_sources);
/* One guarded removal attempt; a fault retains ownership and makes stop false.
   A later world with no private entities permits release of this fixture. */
int unit_combat_remove(uintptr_t game);
int unit_combat_stop(void);
#endif
