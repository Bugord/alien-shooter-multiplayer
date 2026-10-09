#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../../asmp-dll/src/game/ui.h"

/* The native state bar hides weapon icons of items granted after the level
   started. A stub entity action records the "show" request. */
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main(void)
{
    unsigned char* game = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x300);
    unsigned char* code = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    uintptr_t* record = (uintptr_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 5 * sizeof(uintptr_t));
    uintptr_t* vtable = (uintptr_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 8 * sizeof(uintptr_t));
    unsigned char* icon = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x100);
    unsigned char* icon_vid = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x20);
    uintptr_t* items = (uintptr_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 4 * sizeof(uintptr_t));
    if (!game || !code || !record || !vtable || !icon || !icon_vid || !items) return 2;
    /* __thiscall stub taking (kind, a, b, c): records ECX and the four stack arguments. */
    unsigned char stub[64]; unsigned int n = 0;
    stub[n++] = 0x55; stub[n++] = 0x8B; stub[n++] = 0xEC;
    stub[n++] = 0x89; stub[n++] = 0x0D; *(uintptr_t*)(stub + n) = (uintptr_t)&record[0]; n += 4;
    for (unsigned int i = 0; i < 4; ++i) {
        stub[n++] = 0x8B; stub[n++] = 0x45; stub[n++] = (unsigned char)(8 + 4 * i);
        stub[n++] = 0xA3; *(uintptr_t*)(stub + n) = (uintptr_t)&record[1 + i]; n += 4;
    }
    stub[n++] = 0x5D; stub[n++] = 0xC2; stub[n++] = 0x10; stub[n++] = 0x00;
    memcpy(code, stub, n);
    vtable[1] = (uintptr_t)code; *(uintptr_t*)icon = (uintptr_t)vtable;
    *(uintptr_t*)(icon + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)icon_vid;
    *(unsigned int*)(icon_vid + STEAM_VID_INDEX_OFFSET) = STEAM_STATEBAR_WEAPON_VID;
    icon[STEAM_ENTITY_DIRECTION_OFFSET] = 6;
    items[0] = (uintptr_t)icon;
    *(unsigned int*)(game + STEAM_GAME_MENU_LIST_OFFSET + 4) = 1;
    *(unsigned int*)(game + STEAM_GAME_MENU_LIST_OFFSET + 8) = 4;
    *(uintptr_t**)(game + STEAM_GAME_MENU_LIST_OFFSET + 12) = items;
    CHECK(ui_show_weapon_icons((uintptr_t)game) == 1);
    CHECK(record[0] == (uintptr_t)icon && record[1] == STEAM_ACT_SET_INVISIBLE && record[2] == 0);
    *(unsigned int*)(icon_vid + STEAM_VID_INDEX_OFFSET) = 123; /* Not a state bar icon. */
    memset(record, 0, 5 * sizeof(uintptr_t));
    CHECK(ui_show_weapon_icons((uintptr_t)game) == 0 && !record[0]);
    *(unsigned int*)(game + STEAM_GAME_MENU_LIST_OFFSET + 4) = 0; /* Panel not built yet. */
    CHECK(ui_show_weapon_icons((uintptr_t)game) == 0);
    if (failures) return 1;
    puts("UI icon checks passed");
    return 0;
}
