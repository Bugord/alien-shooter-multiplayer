#ifndef ASMP_WORLD_LIST_H
#define ASMP_WORLD_LIST_H
#include "steam_profile.h"

/* Read-only access to the native MAP entity lists (game layer). Each list is a
   16-byte header inside the GAME object: count, capacity and an items pointer.
   Callers must run inside SEH and must already have checked the GAME vtable. */

/* Returns 1 if `entity` is in the given category list. A damaged list header
   (null items, count above capacity or above the sanity limit) counts as not
   found. The pointer is only compared, never dereferenced. */
static inline int world_list_find(uintptr_t game, unsigned int category, uintptr_t entity)
{
    uintptr_t list = game + STEAM_WORLD_LIST_OFFSET + category * STEAM_LIST_SIZE;
    unsigned int count = *(unsigned int*)(list + STEAM_LIST_COUNT_OFFSET);
    unsigned int capacity = *(unsigned int*)(list + STEAM_LIST_CAPACITY_OFFSET);
    uintptr_t* items = *(uintptr_t**)(list + STEAM_LIST_ITEMS_OFFSET);
    if (!items || count > capacity || count > STEAM_LIST_MAX_ENTRIES)
        return 0;
    for (unsigned int i = 0; i < count; ++i)
    {
        if (items[i] == entity)
            return 1;
    }
    return 0;
}
#endif
