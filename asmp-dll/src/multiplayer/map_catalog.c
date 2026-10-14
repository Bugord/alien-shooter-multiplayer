/* Catalogue of the room's game modes and missions: parses a level path into a mode
   and mission, builds the map and briefing paths back, and steps through the choices
   the host's selectors offer. Layer: multiplayer/ (session); pure, no native calls. */

#include <stdio.h>
#include <string.h>
#include "map_catalog.h"

#define LEVEL_FILE_NAME_LENGTH 12u /* "level_NN.map" */
#define SURVIVAL_MISSIONS      1u

typedef struct ModeFiles
{
    const char* folder;     /* Normalized (lower case, backslashes) map folder. */
    const char* map_format; /* Map path with the mission number. */
    const char* text_format;
    unsigned int missions; /* Missions with a briefing text in Text\. */
} ModeFiles;

/* Campaign Level_11 and add-on 2 Level_06 are final maps without a briefing; the
   game reaches them only from the mission before, so the room does not offer them. */
static const ModeFiles mode_files[CATALOG_MODE_COUNT] = {
    {"maps\\", "maps\\Level_%02u.map", "Text\\level_%02u.txt", 10u},
    {"maps\\addon\\", "maps\\addon\\Level_%02u.map", "Text\\addon_level_%02u.txt", 6u},
    {"maps\\addon2\\", "maps\\addon2\\Level_%02u.map", "Text\\addon2level_%02u.txt", 5u},
    {"maps\\", "maps\\survive_01.map", NULL, SURVIVAL_MISSIONS},
};

#define SURVIVAL_FILE_NAME "survive_01.map"

/* Copies `map` in lower case with backslashes. Returns 0 when it does not fit. */
static int normalize(const char* map, char* out, size_t capacity)
{
    size_t i;
    for (i = 0; map[i]; ++i)
    {
        if (i + 1 >= capacity)
            return 0;
        char c = map[i];
        if (c == '/')
            c = '\\';
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        out[i] = c;
    }
    out[i] = 0;
    return 1;
}

/* Reads "level_NN.map" into a mission number, or returns 0. */
static unsigned int parse_level_file(const char* file)
{
    if (strlen(file) != LEVEL_FILE_NAME_LENGTH || strncmp(file, "level_", 6) ||
        strcmp(file + 8, ".map"))
        return 0;
    char tens = file[6];
    char ones = file[7];
    if (tens < '0' || tens > '9' || ones < '0' || ones > '9')
        return 0;
    return (unsigned int)(tens - '0') * 10u + (unsigned int)(ones - '0');
}

int map_catalog_parse(const char* map, MapChoice* out)
{
    char path[64];
    if (!map || !out || !normalize(map, path, sizeof(path)))
        return 0;

    if (!strcmp(path, "maps\\" SURVIVAL_FILE_NAME))
    {
        out->mode = CATALOG_SURVIVAL;
        out->mission = 1u;
        return 1;
    }

    /* Longest folder first: "maps\addon2\" also starts with "maps\". */
    static const enum CatalogMode order[] = {CATALOG_ADDON2, CATALOG_ADDON, CATALOG_CAMPAIGN};
    for (unsigned int i = 0; i < sizeof(order) / sizeof(order[0]); ++i)
    {
        const ModeFiles* files = &mode_files[order[i]];
        size_t folder_length = strlen(files->folder);
        if (strncmp(path, files->folder, folder_length))
            continue;

        unsigned int mission = parse_level_file(path + folder_length);
        if (!mission || mission > files->missions)
            return 0;
        out->mode = order[i];
        out->mission = mission;
        return 1;
    }
    return 0;
}

static int valid_choice(const MapChoice* choice)
{
    if (!choice || (unsigned int)choice->mode >= CATALOG_MODE_COUNT)
        return 0;
    return choice->mission >= 1u && choice->mission <= mode_files[choice->mode].missions;
}

/* Prints one mission number into `format`; returns 0 when it does not fit. */
static int format_path(const char* format, unsigned int mission, char* out, size_t capacity)
{
    int length = _snprintf_s(out, capacity, _TRUNCATE, format, mission);
    return length > 0;
}

int map_catalog_map_path(const MapChoice* choice, char* out, size_t capacity)
{
    if (!valid_choice(choice) || !out)
        return 0;
    return format_path(mode_files[choice->mode].map_format, choice->mission, out, capacity);
}

int map_catalog_text_path(const MapChoice* choice, char* out, size_t capacity)
{
    if (!valid_choice(choice) || !out || !mode_files[choice->mode].text_format)
        return 0;
    return format_path(mode_files[choice->mode].text_format, choice->mission, out, capacity);
}

unsigned int map_catalog_missions(enum CatalogMode mode)
{
    if ((unsigned int)mode >= CATALOG_MODE_COUNT)
        return 0;
    return mode_files[mode].missions;
}

MapChoice map_catalog_step_mode(MapChoice current, int step, unsigned int available)
{
    int direction = step < 0 ? -1 : 1;
    int mode = (int)current.mode;

    for (int tried = 1; tried < CATALOG_MODE_COUNT; ++tried)
    {
        mode = (mode + direction + CATALOG_MODE_COUNT) % CATALOG_MODE_COUNT;
        if (available & CATALOG_MODE_BIT(mode))
        {
            MapChoice next = {(enum CatalogMode)mode, 1u};
            return next;
        }
    }
    return current;
}

MapChoice map_catalog_step_mission(MapChoice current, int step)
{
    unsigned int count = map_catalog_missions(current.mode);
    if (count <= 1u || current.mission < 1u || current.mission > count)
        return current;

    if (step < 0)
        current.mission = current.mission == 1u ? count : current.mission - 1u;
    else
        current.mission = current.mission == count ? 1u : current.mission + 1u;
    return current;
}
