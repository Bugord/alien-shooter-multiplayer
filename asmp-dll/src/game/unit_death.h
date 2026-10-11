#ifndef ASMP_UNIT_DEATH_H
#define ASMP_UNIT_DEATH_H
#include "unit_puppet.h"

#define UNIT_DEATH_SLOT_COUNT 128u

typedef struct UnitDeathSample
{
    unsigned int entities, corpses, gibs;
    int fault;
} UnitDeathSample;
typedef struct UnitDeathEngine
{
    ActorCreate create;
    ActorDestroy cannon_destroy, terrain_destroy, part_destroy;
} UnitDeathEngine;
/* Bounded death presentations, selected on the game thread. Native allocations
   belong to the map; this module
   owns bounded pinned shallow VIDs and tracks ALL effects by private VID identity.
   Art/behavior resources are borrowed and never freed. Terrain stamps are native
   map visuals and remain until map reload. No scripts, damage or unit collision.
   All operations except init/stop execute on the game thread. Init/stop are global;
   stop refuses while any selected or unselected slot still owns effects. */
int unit_death_init(uintptr_t base, ActorCreate);
int unit_death_initialize(uintptr_t base, const UnitDeathEngine*);
int unit_death_select(unsigned int slot);
/* Strict VID50 compatibility fixture; play_type follows the selected UNIT CRE. */
int unit_death_play_type(uintptr_t game, const UnitPuppetPose*, unsigned int animation,
                         unsigned int source_vid);
/* Armor break visuals only. A native class-2 successor is deliberately omitted;
   the authoritative live form is created/replicated separately. Large90 uses
   the medium70 shell fragments as an intentional custom effect on both peers. */
int unit_death_play_armor(uintptr_t game, const UnitPuppetPose*, unsigned int source_vid);
int unit_death_play(uintptr_t game, const UnitPuppetPose*, unsigned int animation);
UnitDeathSample unit_death_sample(uintptr_t game);
int unit_death_remove(uintptr_t game);
int unit_death_stop(void);
#endif
