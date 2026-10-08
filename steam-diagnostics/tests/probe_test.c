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
    unsigned char* player = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0xBC);
    unsigned char* vid = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x60);
    unsigned char* weapon = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x08);
    Snapshot s;
    if (!image || !game || !army || !player || !vid || !weapon) return 2;
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
    *(uintptr_t*)(player + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)vid;
    *(uintptr_t*)(vid + STEAM_VID_LINKED_OFFSET) = (uintptr_t)weapon;
    *(int32_t*)(weapon + STEAM_VID_INDEX_OFFSET) = 12;
    *(int32_t*)(player + STEAM_CURRENT_AMMO_OFFSET) = 7 * 64 + 63;
    for (unsigned int i = 0; i < STEAM_STORED_AMMO_COUNT; ++i)
        *(uint32_t*)(player + STEAM_STORED_AMMO_BASE_OFFSET +
            (i + STEAM_STORED_AMMO_FIRST_SLOT) * sizeof(uint32_t)) = 100 + i;
    expect(probe_read(base, &s), PROBE_OK, "read complete player snapshot");
    if (s.army_index != 3 || s.x != 123.25f || s.y != -75.5f || s.z != 12.0f ||
        s.health != 87 || s.animation != 2 || s.direction != 128 ||
        s.weapon_slot != 2 || s.weapon_vid != 12 || s.current_ammo != 7 ||
        s.current_ammo_raw != 511 || s.stored_ammo[1] != 101) ++failures;
    for (unsigned int i = 0; i < STEAM_STORED_AMMO_COUNT; ++i)
        if (s.stored_ammo[i] != 100 + i) ++failures;
    /* The armed weapon uses live fixed-point ammo, not its stale stored slot. */
    *(int32_t*)(player + STEAM_CURRENT_AMMO_OFFSET) = 6 * 64;
    *(int32_t*)(player + STEAM_ENTITY_HEALTH_OFFSET) = 42;
    expect(probe_read(base, &s), PROBE_OK, "shot and damage");
    if (s.current_ammo != 6 || s.stored_ammo[1] != 101 || s.health != 42) ++failures;
    *(int32_t*)(weapon + STEAM_VID_INDEX_OFFSET) = 19;
    *(int32_t*)(player + STEAM_CURRENT_AMMO_OFFSET) = 0;
    expect(probe_read(base, &s), PROBE_OK, "last weapon and empty ammo");
    if (s.weapon_slot != 9 || s.current_ammo != 0 || s.stored_ammo[8] != 108) ++failures;
    *(int32_t*)(weapon + STEAM_VID_INDEX_OFFSET) = 10;
    *(int32_t*)(player + STEAM_CURRENT_AMMO_OFFSET) = -127;
    *(int32_t*)(player + STEAM_ENTITY_HEALTH_OFFSET) = -1;
    expect(probe_read(base, &s), PROBE_OK, "signed values and pistol slot");
    if (s.weapon_slot != 0 || s.current_ammo != -1 || s.health != -1) ++failures;
    *(int32_t*)(weapon + STEAM_VID_INDEX_OFFSET) = 260;
    expect(probe_read(base, &s), PROBE_OK, "unrecognized weapon VID");
    if (s.weapon_slot != -1 || s.weapon_vid != 260) ++failures;
    *(uintptr_t*)(vid + STEAM_VID_LINKED_OFFSET) = 0;
    expect(probe_read(base, &s), PROBE_OK, "missing weapon during transition");
    if (s.weapon_slot != -1 || s.weapon_vid != -1) ++failures;
    *(uintptr_t*)(player + STEAM_ENTITY_VID_OFFSET) = 0;
    expect(probe_read(base, &s), PROBE_OK, "missing player VID");
    if (s.weapon_slot != -1) ++failures;
    *(uintptr_t*)(player + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)vid;
    *(uintptr_t*)(vid + STEAM_VID_LINKED_OFFSET) = 1;
    expect(probe_read(base, &s), PROBE_READ_FAULT, "contain a stale weapon pointer");
    *(uintptr_t*)(vid + STEAM_VID_LINKED_OFFSET) = (uintptr_t)weapon;
    uint32_t nan_bits = 0x7FC00000;
    memcpy(player + STEAM_ENTITY_X_OFFSET, &nan_bits, sizeof(nan_bits));
    expect(probe_read(base, &s), PROBE_BAD_COORDS, "reject nonfinite coordinates");
    *(uintptr_t*)(army + STEAM_ARMY_PLAYER_OFFSET) = 1;
    expect(probe_read(base, &s), PROBE_READ_FAULT, "contain a stale player pointer");
    VirtualFree(image, 0, MEM_RELEASE);
    HeapFree(GetProcessHeap(), 0, game);
    HeapFree(GetProcessHeap(), 0, army);
    HeapFree(GetProcessHeap(), 0, player);
    HeapFree(GetProcessHeap(), 0, vid);
    HeapFree(GetProcessHeap(), 0, weapon);
    printf("Probe checks: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
