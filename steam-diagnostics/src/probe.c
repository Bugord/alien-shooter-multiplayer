#include <windows.h>
#include <float.h>
#include <string.h>
#include "profile.h"
#include "probe.h"

enum ProbeResult probe_read(uintptr_t image_base, Snapshot* output)
{
    Snapshot s = {0};
    enum ProbeResult result = PROBE_OK;
    /* No game function calls or writes. SEH contains invalid/transient pointers. */
    __try {
        s.game = *(uintptr_t*)(image_base + STEAM_GAME_PTR_RVA);
        if (!s.game) result = PROBE_NO_GAME;
        else if (*(uintptr_t*)s.game != image_base + STEAM_GAME_VTABLE_RVA) result = PROBE_GAME_TYPE;
        else {
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
                    s.health = *(uint32_t*)(s.player + STEAM_ENTITY_HEALTH_OFFSET);
                    s.animation = *(uint32_t*)(s.player + STEAM_ENTITY_ANIM_OFFSET);
                    s.direction = *(unsigned char*)(s.player + STEAM_ENTITY_DIRECTION_OFFSET);
                    if (!_finite(s.x) || !_finite(s.y) || !_finite(s.z)) result = PROBE_BAD_COORDS;
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
