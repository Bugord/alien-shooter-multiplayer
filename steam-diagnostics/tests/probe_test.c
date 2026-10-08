#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../src/profile.h"
#include "../src/probe.h"

static int failures;
static void expect(enum ProbeResult actual, enum ProbeResult wanted, const char* label)
{
    if (actual != wanted) { fprintf(stderr, "FAIL %s: %s\n", label, probe_result_name(actual)); ++failures; }
}

int main(void)
{
    unsigned char* image = VirtualAlloc(NULL, 0x276000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    unsigned char* game = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x22C8);
    unsigned char* army = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x100);
    unsigned char* player = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0xB8);
    Snapshot s;
    if (!image || !game || !army || !player) return 2;
    uintptr_t base = (uintptr_t)image;
    expect(probe_read(base, &s), PROBE_NO_GAME, "no initialized game");
    *(uintptr_t*)(image + STEAM_GAME_PTR_RVA) = (uintptr_t)game;
    expect(probe_read(base, &s), PROBE_GAME_TYPE, "reject other game layouts");
    *(uintptr_t*)game = base + STEAM_GAME_VTABLE_RVA;
    expect(probe_read(base, &s), PROBE_NO_ARMY, "menu before army creation");
    *(uint32_t*)(game + STEAM_ARMY_INDEX_OFFSET) = 3;
    *(uintptr_t*)(game + STEAM_ARMY_ARRAY_OFFSET + 3 * sizeof(uintptr_t)) = (uintptr_t)army;
    expect(probe_read(base, &s), PROBE_NO_PLAYER, "map before player spawn");
    *(uintptr_t*)(army + STEAM_ARMY_PLAYER_OFFSET) = (uintptr_t)player;
    expect(probe_read(base, &s), PROBE_PLAYER_TYPE, "reject other entity classes");
    *(uintptr_t*)player = base + STEAM_MAN_VTABLE_RVA;
    *(float*)(player + STEAM_ENTITY_X_OFFSET) = 123.25f;
    *(float*)(player + STEAM_ENTITY_Y_OFFSET) = -75.5f;
    *(float*)(player + STEAM_ENTITY_Z_OFFSET) = 12.0f;
    *(uint32_t*)(player + STEAM_ENTITY_HEALTH_OFFSET) = 87;
    *(uint32_t*)(player + STEAM_ENTITY_ANIM_OFFSET) = 2;
    *(unsigned char*)(player + STEAM_ENTITY_DIRECTION_OFFSET) = 128;
    expect(probe_read(base, &s), PROBE_OK, "read complete player snapshot");
    if (s.army_index != 3 || s.x != 123.25f || s.y != -75.5f || s.z != 12.0f ||
        s.health != 87 || s.animation != 2 || s.direction != 128) ++failures;
    uint32_t nan_bits = 0x7FC00000;
    memcpy(player + STEAM_ENTITY_X_OFFSET, &nan_bits, sizeof(nan_bits));
    expect(probe_read(base, &s), PROBE_BAD_COORDS, "reject nonfinite coordinates");
    *(uintptr_t*)(army + STEAM_ARMY_PLAYER_OFFSET) = 1;
    expect(probe_read(base, &s), PROBE_READ_FAULT, "contain a stale player pointer");
    VirtualFree(image, 0, MEM_RELEASE);
    HeapFree(GetProcessHeap(), 0, game);
    HeapFree(GetProcessHeap(), 0, army);
    HeapFree(GetProcessHeap(), 0, player);
    printf("Probe checks: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
