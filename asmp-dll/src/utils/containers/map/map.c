/**
 * @file map.c
 * @brief Implementation of a generic map data structure and its associated
 *        functions.
 */
#include "map.h"
#include <string.h>
#include "utils/mem/mem.h"

typedef struct MapItem
{
    int key;
    void* data;
} MapItem;

typedef struct Map
{
    MapItem* data;
    int items_number;
    int capacity;
} Map;

/**
 * @brief Finds the index where a key is or should be inserted (binary search).
 *
 * @param[in] map Pointer to the map.
 * @param[in] key The key to search for.
 * @param[out] found Set to true if the key exists, false otherwise.
 *
 * @return The index of the item if found, or the insertion point if not found.
 */
static int find_index_(Map* map, int key, bool* found)
{
    int lo = 0;
    int hi = map->items_number;
    while (lo < hi)
    {
        int mid = lo + (hi - lo) / 2;
        if (map->data[mid].key < key)
        {
            lo = mid + 1;
        }
        else if (map->data[mid].key > key)
        {
            hi = mid;
        }
        else
        {
            *found = true;
            return mid;
        }
    }
    *found = false;
    return lo;
}

/**
 * @brief Searches for a MapItem instance with the specified key.
 *
 * @param[in] map Pointer to the map in which the MapItem instance is searched.
 * @param[in] key The key to be searched.
 *
 * @return MapItem* Pointer to the MapItem instance if found.
 * @return NULL If not found.
 */
static MapItem* find_item_(Map* map, int key)
{
    if (!map || !map->data || !map->items_number)
    {
        return 0;
    }
    bool found = false;
    int idx = find_index_(map, key, &found);
    return found ? &map->data[idx] : 0;
}

Map* map_create(void)
{
    Map* map = mem_alloc(sizeof(Map));
    if (!map)
    {
        return 0;
    }
    mem_set(map, 0, sizeof(Map));
    return map;
}

void map_destroy(Map* map)
{
    if (!map)
    {
        return;
    }
    map_clear(map);
    mem_free(map);
}

void map_clear(Map* map)
{
    if (!map)
    {
        return;
    }
    map->capacity = 0;
    map->items_number = 0;
    if (!map->data)
    {
        return;
    }
    mem_free(map->data);
    map->data = 0;
}

bool map_insert(Map* map, int key, void* data)
{
    if (!map)
    {
        return false;
    }
    bool found = false;
    int idx = 0;
    if (map->items_number > 0)
    {
        idx = find_index_(map, key, &found);
        if (found)
        {
            return false;
        }
    }
    if (map->items_number >= map->capacity)
    {
        int new_capacity = map->capacity == 0 ? 1 : map->capacity * 2;
        void* new_data = mem_realloc(map->data, new_capacity * sizeof(MapItem));
        if (new_data)
        {
            map->capacity = new_capacity;
            map->data = new_data;
        }
        else
        {
            return false;
        }
    }
    /* Shift items right to make room at the insertion point. */
    if (idx < map->items_number)
    {
        memmove(&map->data[idx + 1], &map->data[idx],
                (map->items_number - idx) * sizeof(MapItem));
    }
    map->data[idx].key = key;
    map->data[idx].data = data;
    ++map->items_number;
    return true;
}

bool map_erase(Map* map, int key)
{
    if (!map || !map->data)
    {
        return false;
    }
    bool found = false;
    int idx = find_index_(map, key, &found);
    if (!found)
    {
        return false;
    }
    /* Shift items left to fill the gap. */
    if (idx < map->items_number - 1)
    {
        memmove(&map->data[idx], &map->data[idx + 1],
                (map->items_number - idx - 1) * sizeof(MapItem));
    }
    --map->items_number;
    return true;
}

void* map_find(Map* map, int key)
{
    if (!map)
    {
        return 0;
    }
    MapItem* item = find_item_(map, key);
    return item ? item->data : 0;
}

void map_foreach(Map* map, Map_foreach_callback callback)
{
    if (!map || !map->items_number || !callback)
    {
        return;
    }
    for (int i = 0; i < map->items_number; i++)
    {
        callback(map->data[i].key, map->data[i].data);
    }
}
