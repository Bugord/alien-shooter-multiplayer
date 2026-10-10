#ifndef ASMP_UNIT_COMBAT_H
#define ASMP_UNIT_COMBAT_H
#include "unit_puppet.h"
#define UNIT_COMBAT_CAPACITY UNIT_PUPPET_CAPACITY
int unit_combat_select(unsigned int slot);

typedef struct UnitCombatSample {
    uintptr_t root, child;
    unsigned int private_entities, other_entities, unexpected_entities;
    int sources_unchanged, fault;
} UnitCombatSample;
/* Bounded selected offline fixture / network copy. Default is VID50 + VID108.
   Factory owns native allocations;
   this module owns private shallow VID copies, never the borrowed resources.
   Buffers stay in the pinned DLL through the complete deleting destructors.
   Every function below runs on the game thread. Init binds only, on worker. */
int unit_combat_bind(uintptr_t base, ActorCreate*);
int unit_combat_init(uintptr_t base, ActorCreate, ActorDestroy, UnitAnimation);
int unit_combat_spawn(const Snapshot*);
/* A supported class-2 stock descriptor, with zero or one class-9 part.
   Host mode preserves only native death creation links; AI is frozen separately
   with unit_puppet_watch_stationary. Native death successors belong to the map. */
int unit_combat_spawn_type(const Snapshot*, unsigned int vid, const UnitPuppetPose*, int host);
UnitCombatSample unit_combat_sample(uintptr_t game, int compare_sources);
/* One guarded removal attempt; a fault retains ownership and makes stop false.
   A later world with no private entities permits release of this fixture. */
int unit_combat_remove(uintptr_t game);
int unit_combat_stop(void);
#endif
