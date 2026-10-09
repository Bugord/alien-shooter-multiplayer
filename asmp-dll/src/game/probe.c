#include <windows.h>
#include <float.h>
#include <string.h>
#include "steam_profile.h"
#include "probe.h"
#include "../../../common/src/protocol.h"

enum ProbeResult probe_read(uintptr_t image_base, Snapshot* output)
{
    Snapshot s = {0};
    s.weapon_slot = -1;
    s.weapon_vid = -1;
    enum ProbeResult result = PROBE_OK;
    /* No game function calls or writes. SEH contains invalid/transient pointers. */
    __try {
        s.game = *(uintptr_t*)(image_base + STEAM_GAME_PTR_RVA);
        if (!s.game) result = PROBE_NO_GAME;
        else if (*(uintptr_t*)s.game != image_base + STEAM_GAME_VTABLE_RVA) result = PROBE_GAME_TYPE;
        else {
            const char* map = *(const char**)(s.game + STEAM_GAME_MAP_PATH_OFFSET);
            s.map_started = *(uint32_t*)(s.game + STEAM_GAME_MAP_STARTED_OFFSET);
            if (map) {
                unsigned int i;
                for (i = 0; i < sizeof(s.map) - 1 && map[i]; ++i) s.map[i] = map[i];
                s.map[i] = 0;
                s.in_level = !(i == sizeof(s.map) - 1 && map[i]) && mp_is_level_path(s.map);
                if (s.in_level)
                    mp_world_key(s.map, &s.world_low, &s.world_high);
            }
            s.army_index = *(uint32_t*)(s.game + STEAM_ARMY_INDEX_OFFSET) & 3u;
            s.army = *(uintptr_t*)(s.game + STEAM_ARMY_ARRAY_OFFSET + s.army_index * sizeof(uintptr_t));
            if (!s.army) result = PROBE_NO_ARMY;
            else {
                s.player = *(uintptr_t*)(s.army + STEAM_ARMY_PLAYER_OFFSET);
                if (!s.player) result = PROBE_NO_PLAYER;
                else if (*(uintptr_t*)s.player != image_base + STEAM_MAN_VTABLE_RVA) result = PROBE_PLAYER_TYPE;
                else {
                    s.x = *(float*)(s.player + STEAM_ENTITY_X_OFFSET);
                    s.y = *(float*)(s.player + STEAM_ENTITY_Y_OFFSET);
                    s.z = *(float*)(s.player + STEAM_ENTITY_Z_OFFSET);
                    s.health = *(int32_t*)(s.player + STEAM_ENTITY_HEALTH_OFFSET);
                    s.current_ammo_raw = *(int32_t*)(s.player + STEAM_CURRENT_AMMO_OFFSET);
                    s.current_ammo = s.current_ammo_raw / STEAM_AMMO_SCALE;
                    for (unsigned int i = 0; i < STEAM_STORED_AMMO_COUNT; ++i) {
                        s.stored_ammo[i] = *(uint32_t*)(s.player + STEAM_STORED_AMMO_BASE_OFFSET +
                            (i + STEAM_STORED_AMMO_FIRST_SLOT) * sizeof(uint32_t));
                    }
                    uintptr_t vid = *(uintptr_t*)(s.player + STEAM_ENTITY_VID_OFFSET);
                    if (vid) {
                        s.max_health = *(int32_t*)(vid + STEAM_VID_MAX_HEALTH_OFFSET + s.army_index * sizeof(int32_t));
                        uintptr_t weapon = *(uintptr_t*)(vid + STEAM_VID_LINKED_OFFSET);
                        if (weapon) {
                            s.weapon_vid = *(int32_t*)(weapon + STEAM_VID_INDEX_OFFSET);
                            if (s.weapon_vid >= STEAM_WEAPON_VID_FIRST &&
                                s.weapon_vid < STEAM_WEAPON_VID_FIRST + (int32_t)STEAM_WEAPON_SLOT_COUNT)
                                s.weapon_slot = s.weapon_vid - STEAM_WEAPON_VID_FIRST;
                            uintptr_t child = *(uintptr_t*)(s.player + STEAM_ENTITY_CHILD_OFFSET);
                            if (child && *(uintptr_t*)(child + STEAM_ENTITY_VID_OFFSET) == weapon) {
                                s.torso_present = 1;
                                s.torso_direction = *(unsigned char*)(child + STEAM_ENTITY_DIRECTION_OFFSET);
                            }
                        }
                    }
                    s.animation = *(uint32_t*)(s.player + STEAM_ENTITY_ANIM_OFFSET);
                    s.direction = *(unsigned char*)(s.player + STEAM_ENTITY_DIRECTION_OFFSET);
                    s.velocity = *(float*)(s.player + STEAM_ENTITY_VELOCITY_OFFSET);
                    s.moving = !!(*(uint32_t*)(s.player + STEAM_ENTITY_FLAGS_OFFSET) & STEAM_ENTITY_MOVING_FLAG);
                    if (!_finite(s.x) || !_finite(s.y) || !_finite(s.z) || !_finite(s.velocity))
                        result = PROBE_BAD_COORDS;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = PROBE_READ_FAULT;
    }
    memcpy(output, &s, sizeof(s));
    return result;
}

const char* probe_result_name(enum ProbeResult result)
{
    static const char* names[] = {"player", "waiting-game", "unexpected-game-type", "no-army",
                                  "no-player", "unexpected-player-type", "invalid-coordinates", "read-fault"};
    return names[result];
}
