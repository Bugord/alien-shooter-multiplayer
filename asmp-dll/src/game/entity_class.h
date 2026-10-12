#ifndef ASMP_ENTITY_CLASS_H
#define ASMP_ENTITY_CLASS_H
#include "steam_profile.h"

/* Vtable RVA of the native entity classes the monster modules create or track
   (game layer): UNIT, CANNON and BUILDED_TERRAIN. Every other class they admit
   (the class-9 parts) is a SPRITE. Compare it with base + RVA. */
static inline uintptr_t entity_class_vtable_rva(unsigned int class_id)
{
    if (class_id == STEAM_UNIT_CLASS)
        return STEAM_UNIT_VTABLE_RVA;
    if (class_id == STEAM_CANNON_CLASS)
        return STEAM_CANNON_VTABLE_RVA;
    if (class_id == STEAM_TERRAIN_CLASS)
        return STEAM_TERRAIN_VTABLE_RVA;
    return STEAM_SPRITE_VTABLE_RVA;
}
#endif
