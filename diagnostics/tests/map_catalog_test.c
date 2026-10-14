/* Checks for asmp-dll/src/multiplayer/map_catalog.c: level paths parse into a mode and
   mission and back, briefing paths, and the host's mode and mission steps. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../asmp-dll/src/multiplayer/map_catalog.h"

#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void check_parse(void)
{
    MapChoice choice;
    CHECK(map_catalog_parse("maps\\Level_03.map", &choice));
    CHECK(choice.mode == CATALOG_CAMPAIGN && choice.mission == 3u);
    CHECK(map_catalog_parse("MAPS/ADDON/level_06.MAP", &choice));
    CHECK(choice.mode == CATALOG_ADDON && choice.mission == 6u);
    CHECK(map_catalog_parse("maps\\addon2\\Level_05.map", &choice));
    CHECK(choice.mode == CATALOG_ADDON2 && choice.mission == 5u);
    CHECK(map_catalog_parse("maps\\survive_01.map", &choice));
    CHECK(choice.mode == CATALOG_SURVIVAL && choice.mission == 1u);

    /* Final maps without a briefing, other files and broken names are not offered. */
    CHECK(!map_catalog_parse("maps\\Level_11.map", &choice));
    CHECK(!map_catalog_parse("maps\\addon2\\Level_06.map", &choice));
    CHECK(!map_catalog_parse("maps\\Level_00.map", &choice));
    CHECK(!map_catalog_parse("maps\\Level_3.map", &choice));
    CHECK(!map_catalog_parse("maps\\mainmenu.map", &choice));
    CHECK(!map_catalog_parse("maps\\other\\Level_01.map", &choice));
    CHECK(!map_catalog_parse(NULL, &choice));
}

static void check_paths(void)
{
    char path[64];
    MapChoice addon = {CATALOG_ADDON2, 4u};
    CHECK(map_catalog_map_path(&addon, path, sizeof(path)));
    CHECK(!strcmp(path, "maps\\addon2\\Level_04.map"));
    CHECK(map_catalog_text_path(&addon, path, sizeof(path)));
    CHECK(!strcmp(path, "Text\\addon2level_04.txt"));

    MapChoice campaign = {CATALOG_CAMPAIGN, 10u};
    CHECK(map_catalog_text_path(&campaign, path, sizeof(path)));
    CHECK(!strcmp(path, "Text\\level_10.txt"));
    MapChoice extra = {CATALOG_ADDON, 6u};
    CHECK(map_catalog_text_path(&extra, path, sizeof(path)));
    CHECK(!strcmp(path, "Text\\addon_level_06.txt"));

    MapChoice survival = {CATALOG_SURVIVAL, 1u};
    CHECK(map_catalog_map_path(&survival, path, sizeof(path)));
    CHECK(!strcmp(path, "maps\\survive_01.map"));
    CHECK(!map_catalog_text_path(&survival, path, sizeof(path)));

    MapChoice outside = {CATALOG_CAMPAIGN, 11u};
    CHECK(!map_catalog_map_path(&outside, path, sizeof(path)));
    char tiny[8];
    CHECK(!map_catalog_map_path(&campaign, tiny, sizeof(tiny)));

    /* Every catalogue map round-trips. */
    for (int mode = 0; mode < CATALOG_MODE_COUNT; ++mode)
    {
        for (unsigned int mission = 1; mission <= map_catalog_missions(mode); ++mission)
        {
            MapChoice choice = {(enum CatalogMode)mode, mission};
            MapChoice parsed;
            CHECK(map_catalog_map_path(&choice, path, sizeof(path)));
            CHECK(map_catalog_parse(path, &parsed));
            CHECK(parsed.mode == choice.mode && parsed.mission == choice.mission);
        }
    }
}

static void check_steps(void)
{
    MapChoice choice = {CATALOG_CAMPAIGN, 4u};

    /* Without add-ons the mode selector moves between campaign and survival. */
    MapChoice next = map_catalog_step_mode(choice, 1, CATALOG_ALWAYS_AVAILABLE);
    CHECK(next.mode == CATALOG_SURVIVAL && next.mission == 1u);
    next = map_catalog_step_mode(next, 1, CATALOG_ALWAYS_AVAILABLE);
    CHECK(next.mode == CATALOG_CAMPAIGN && next.mission == 1u);
    next = map_catalog_step_mode(choice, -1, CATALOG_ALWAYS_AVAILABLE);
    CHECK(next.mode == CATALOG_SURVIVAL);

    next = map_catalog_step_mode(choice, 1, CATALOG_ALL_MODES);
    CHECK(next.mode == CATALOG_ADDON && next.mission == 1u);
    next = map_catalog_step_mode(choice, 1, CATALOG_MODE_BIT(CATALOG_CAMPAIGN));
    CHECK(next.mode == CATALOG_CAMPAIGN && next.mission == 4u); /* Nothing else to pick. */

    /* Missions wrap around; survival has one map. */
    next = map_catalog_step_mission(choice, 1);
    CHECK(next.mission == 5u);
    MapChoice last = {CATALOG_CAMPAIGN, 10u};
    CHECK(map_catalog_step_mission(last, 1).mission == 1u);
    MapChoice first = {CATALOG_ADDON, 1u};
    CHECK(map_catalog_step_mission(first, -1).mission == 6u);
    MapChoice survival = {CATALOG_SURVIVAL, 1u};
    CHECK(map_catalog_step_mission(survival, 1).mission == 1u);
}

int main(void)
{
    check_parse();
    check_paths();
    check_steps();
    puts("Map catalogue checks passed: parse, paths, briefings, mode and mission steps.");
    return 0;
}
