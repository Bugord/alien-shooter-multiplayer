#ifndef ASMP_WORLD_LIST_H
#define ASMP_WORLD_LIST_H
#include "steam_profile.h"

/* Read-only access to native lists (game layer). Each list is a 16-byte header
   inside the GAME object: count, capacity and an items pointer. The MAP entity
   lists start at STEAM_WORLD_LIST_OFFSET; the menu list has its own offset.
   Callers must run inside SEH and must already have checked the GAME vtable. */

/* Reads the list header at `list`. Returns 1 and fills `items` and `count` when
   the header is sane: count <= capacity, count <= limit, and items is set when
   count > 0. Returns 0 for a damaged header; the outputs are then undefined. */
static inline int native_list_read(uintptr_t list, unsigned int limit, uintptr_t** items,
                                   unsigned int* count)
{
    unsigned int length = *(unsigned int*)(list + STEAM_LIST_COUNT_OFFSET);
    unsigned int capacity = *(unsigned int*)(list + STEAM_LIST_CAPACITY_OFFSET);
    uintptr_t* entries = *(uintptr_t**)(list + STEAM_LIST_ITEMS_OFFSET);
    if (length > capacity || length > limit || (length && !entries))
        return 0;
    *items = entries;
    *count = length;
    return 1;
}

/* Reads the header of one MAP entity list (see native_list_read). */
static inline int world_list_read(uintptr_t game, unsigned int category, uintptr_t** items,
                                  unsigned int* count)
{
    uintptr_t list = game + STEAM_WORLD_LIST_OFFSET + category * STEAM_LIST_SIZE;
    return native_list_read(list, STEAM_LIST_MAX_ENTRIES, items, count);
}

/* Returns 1 if `entity` is in the given category list. A damaged list header
   counts as not found. The pointer is only compared, never dereferenced. */
static inline int world_list_find(uintptr_t game, unsigned int category, uintptr_t entity)
{
    uintptr_t* items;
    unsigned int count;
    if (!world_list_read(game, category, &items, &count))
        return 0;
    for (unsigned int i = 0; i < count; ++i)
    {
        if (items[i] == entity)
            return 1;
    }
    return 0;
}
#endif
