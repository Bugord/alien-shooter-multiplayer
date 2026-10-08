#ifndef ASMP_DIAG_PROBE_H
#define ASMP_DIAG_PROBE_H
#include <stdint.h>
#include "profile.h"
typedef struct Snapshot {
    uintptr_t game, army, player;
    uint32_t army_index, animation;
    int32_t health, weapon_slot, weapon_vid, current_ammo, current_ammo_raw;
    uint32_t stored_ammo[STEAM_STORED_AMMO_COUNT];
    unsigned int direction;
    float x, y, z;
} Snapshot;
enum ProbeResult { PROBE_OK, PROBE_NO_GAME, PROBE_GAME_TYPE, PROBE_NO_ARMY,
                   PROBE_NO_PLAYER, PROBE_PLAYER_TYPE, PROBE_BAD_COORDS, PROBE_READ_FAULT };
enum ProbeResult probe_read(uintptr_t image_base, Snapshot* output);
const char* probe_result_name(enum ProbeResult result);
#endif
