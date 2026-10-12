#ifndef ASMP_UNIT_COMBAT_H
#define ASMP_UNIT_COMBAT_H
#include "unit_puppet.h"
#define UNIT_COMBAT_CAPACITY UNIT_PUPPET_CAPACITY

/* One combat fixture slot: private VID copies plus the native entities the
   factory created from them. The struct is private to unit_combat.c. Every
   per-fixture function below takes the slot as its first argument. */
/* Functions returning enum UnitResult follow unit_result.h. */
typedef struct UnitCombat UnitCombat;

/* Returns slot `index`, or NULL when index >= UNIT_COMBAT_CAPACITY. Slots are
   static and live as long as the DLL; every function rejects a NULL slot. */
UnitCombat* unit_combat_slot(unsigned int index);

typedef struct UnitCombatSample
{
    uintptr_t root, child;
    unsigned int private_entities, other_entities, unexpected_entities;
    int sources_unchanged, fault;
    unsigned int current_vid, form_revision, transitions;
    int transitioning;
} UnitCombatSample;
enum UnitResult unit_combat_observe_entity(uintptr_t game, uintptr_t entity, uintptr_t* private_vid,
                                           unsigned int* current_vid, unsigned int* revision,
                                           int* transitioning, unsigned int requested_vid);
/* Bounded offline fixture / network copy. Default is VID50 + VID108.
   Factory owns native allocations;
   this module owns private shallow VID copies, never the borrowed resources.
   Buffers stay in the pinned DLL through the complete deleting destructors.
   Every function below runs on the game thread. Init binds only, on worker. */
enum UnitResult unit_combat_bind(uintptr_t base, ActorCreate*);
enum UnitResult unit_combat_init(uintptr_t base, ActorCreate, ActorDestroy, UnitAnimation);
enum UnitResult unit_combat_spawn(UnitCombat* fixture, const Snapshot*);
/* A supported class-2 stock descriptor, with zero or one class-9 part.
   Host mode preserves only native death creation links; AI is frozen separately
   with unit_puppet_watch_stationary. Native death successors belong to the map. */
enum UnitResult unit_combat_spawn_type(UnitCombat* fixture, const Snapshot*, unsigned int vid,
                                       const UnitPuppetPose*, int host);
/* Cosmetic local-hit prediction: native incoming damage/resistance survives,
   while every script and creation link (including armor successors) is inert. */
enum UnitResult unit_combat_spawn_predictive(UnitCombat* fixture, const Snapshot*, unsigned int vid,
                                             const UnitPuppetPose*);
UnitCombatSample unit_combat_sample(UnitCombat* fixture, uintptr_t game, int compare_sources);
/* One guarded removal attempt; a fault retains ownership and makes stop false.
   A later world with no private entities permits release of this fixture. */
enum UnitResult unit_combat_remove(UnitCombat* fixture, uintptr_t game);
enum UnitResult unit_combat_stop(void);
#endif
