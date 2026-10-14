#ifndef ASMP_MAP_CATALOG_H
#define ASMP_MAP_CATALOG_H

/* The game modes and missions the room offers, and how they map to the game's files:
   level maps and briefing texts. Layer: multiplayer/ (session); pure, no native calls.
   Return convention: int functions return 1 on success and 0 for an unknown map or a
   buffer too small. */

#include <stddef.h>

/* In the order of the game's own Play menu. */
enum CatalogMode
{
    CATALOG_CAMPAIGN = 0, /* maps\Level_NN.map */
    CATALOG_ADDON = 1,    /* maps\addon\Level_NN.map */
    CATALOG_ADDON2 = 2,   /* maps\addon2\Level_NN.map */
    CATALOG_SURVIVAL = 3, /* maps\survive_01.map */
    CATALOG_MODE_COUNT = 4
};

/* One bit per mode. The add-on maps are optional; campaign and survival ship with
   every copy of the game. */
#define CATALOG_MODE_BIT(mode) (1u << (unsigned int)(mode))
#define CATALOG_ALWAYS_AVAILABLE                                                                   \
    (CATALOG_MODE_BIT(CATALOG_CAMPAIGN) | CATALOG_MODE_BIT(CATALOG_SURVIVAL))
#define CATALOG_ALL_MODES ((1u << CATALOG_MODE_COUNT) - 1u)

typedef struct MapChoice
{
    enum CatalogMode mode;
    unsigned int mission; /* 1-based */
} MapChoice;

/* Reads a level path such as "maps\addon\Level_03.map" (any case, either slash). */
int map_catalog_parse(const char* map, MapChoice* out);

int map_catalog_map_path(const MapChoice* choice, char* out, size_t capacity);

/* The briefing text the game shows for this mission ("Text\level_03.txt").
   Returns 0 when the mission has none (survival). */
int map_catalog_text_path(const MapChoice* choice, char* out, size_t capacity);

unsigned int map_catalog_missions(enum CatalogMode mode);

/* The next (step +1) or previous (step -1) mode among `available` ones, at its first
   mission. Returns `current` when no other mode is available. */
MapChoice map_catalog_step_mode(MapChoice current, int step, unsigned int available);

/* The next or previous mission of the same mode, wrapping around. */
MapChoice map_catalog_step_mission(MapChoice current, int step);

#endif
