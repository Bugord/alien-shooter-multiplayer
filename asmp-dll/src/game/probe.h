#ifndef ASMP_PROBE_H
#define ASMP_PROBE_H
#include <stdint.h>
#include "steam_profile.h"
typedef struct Snapshot {
    uintptr_t game, army, player;
    uint32_t army_index, animation;
    int32_t health, max_health, weapon_slot, weapon_vid, current_ammo, current_ammo_raw;
    uint32_t stored_ammo[STEAM_STORED_AMMO_COUNT];
    unsigned int direction;
    uint32_t moving, torso_present, torso_direction;
    float x, y, z, velocity;
    uint32_t world_low, world_high;
    uint32_t in_level, map_started, world_load;
    char map[128];
} Snapshot;
enum ProbeResult { PROBE_OK, PROBE_NO_GAME, PROBE_GAME_TYPE, PROBE_NO_ARMY,
                   PROBE_NO_PLAYER, PROBE_PLAYER_TYPE, PROBE_BAD_COORDS, PROBE_READ_FAULT };
enum ProbeResult probe_read(uintptr_t image_base, Snapshot* output);
const char* probe_result_name(enum ProbeResult result);
#endif
