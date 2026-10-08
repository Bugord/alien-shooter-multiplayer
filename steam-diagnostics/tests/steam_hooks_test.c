#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../asmp-dll/src/game/steam/steam_world_hook.h"
#include "../../asmp-dll/src/game/steam/steam_display_hook.h"
#include "../../asmp-dll/src/game/steam/steam_action_hook.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned int paints, scenes;
static HRESULT __stdcall end_scene(void* device) { CHECK(device); ++scenes; return S_OK; }
static HRESULT __stdcall foreign_scene(void* device) { (void)device; return S_FALSE; }
static void paint(void) { ++paints; }
static int replica(uintptr_t entity) { (void)entity; return 1; }
static void shot(int x, int y, unsigned int weapon) { (void)x; (void)y; (void)weapon; CHECK(0); }
static DWORD protection(void* pointer) { MEMORY_BASIC_INFORMATION info; CHECK(VirtualQuery(pointer, &info, sizeof(info))); return info.Protect; }
int main(void) {
    unsigned char* image = VirtualAlloc(NULL, 0x276000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); CHECK(image);
    uintptr_t base = (uintptr_t)image;
    unsigned char render[0xF00] = {0}; uintptr_t device;
    void* volatile* device_table = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); CHECK(device_table);
    typedef void (__fastcall* Load)(void*, void*, char*);
    union { Load function; uintptr_t pointer; } load;
    union { HRESULT (__stdcall* function)(void*); void* pointer; } end, foreign;
    /* Valid x86 stubs with the native prefixes and callee-cleaned argument sizes. */
    static const unsigned char map_stub[] = {0x55,0x8B,0xEC,0x6A,0xFF,0x83,0xC4,0x04,0x5D,0xC2,0x04,0x00};
    static const unsigned char wnd_stub[] = {0x55,0x8B,0xEC,0x6A,0xFF,0x83,0xC4,0x04,0x31,0xC0,0x5D,0xC2,0x10,0x00};
    memcpy(image + STEAM_LOAD_MAP_RVA, map_stub, sizeof(map_stub)); memcpy(image + STEAM_WINDOW_PROC_RVA, wnd_stub, sizeof(wnd_stub));
    uintptr_t* map_slot = (uintptr_t*)(base + STEAM_GAME_VTABLE_RVA + STEAM_LOAD_MAP_SLOT * 4u);
    *map_slot = base + STEAM_LOAD_MAP_RVA + 1;
    CHECK(!steam_world_hook_install(base) && steam_world_hook_stop());
    *map_slot = base + STEAM_LOAD_MAP_RVA;
    CHECK(steam_world_hook_install(base)); load.pointer = *map_slot;
    load.function(NULL, NULL, NULL); load.function(NULL, NULL, NULL);
    CHECK(steam_world_hook_generation() == 2);
    CHECK(steam_world_hook_stop() && *map_slot == base + STEAM_LOAD_MAP_RVA && steam_world_hook_stop());
    CHECK(steam_world_hook_install(base) && steam_world_hook_generation() == 0 && steam_world_hook_stop());
    CHECK(steam_display_hook_install(base, paint) == STEAM_DISPLAY_PENDING && steam_display_hook_stop());
    *(uintptr_t*)(base + STEAM_GAME_VTABLE_RVA + 4) = base + STEAM_WINDOW_PROC_RVA;
    *(uintptr_t*)(base + STEAM_RENDER_PTR_RVA) = (uintptr_t)render;
    *(uintptr_t*)(render + STEAM_RENDER_DEVICE_OFFSET) = (uintptr_t)&device; device = (uintptr_t)device_table;
    end.function = end_scene; foreign.function = foreign_scene; device_table[42] = end.pointer;
    DWORD ignored; CHECK(VirtualProtect((void*)device_table, 4096, PAGE_READONLY, &ignored));
    CHECK(steam_display_hook_install(base, paint) == STEAM_DISPLAY_OK && steam_display_hook_ready());
    CHECK(protection((void*)device_table) == PAGE_READONLY);
    end.pointer = device_table[42]; CHECK(end.function(&device) == S_OK && paints == 1 && scenes == 1);
    CHECK(steam_display_hook_stop() && steam_display_hook_stop() && !steam_display_hook_ready());
    CHECK(end.function(&device) == S_OK && paints == 1 && scenes == 2); /* Fetched before stop. */
    CHECK(steam_display_hook_install(base, paint) == STEAM_DISPLAY_OK);
    void* ours = device_table[42]; CHECK(VirtualProtect((void*)device_table, 4096, PAGE_READWRITE, &ignored));
    device_table[42] = foreign.pointer; CHECK(VirtualProtect((void*)device_table, 4096, PAGE_READONLY, &ignored));
    CHECK(!steam_display_hook_stop() && device_table[42] == foreign.pointer);
    CHECK(VirtualProtect((void*)device_table, 4096, PAGE_READWRITE, &ignored)); device_table[42] = ours;
    CHECK(VirtualProtect((void*)device_table, 4096, PAGE_READONLY, &ignored)); CHECK(steam_display_hook_stop());
    /* Validated actor binding; only action is executed in this test. */
    static const struct { unsigned int rva, length; unsigned char code[12]; } funcs[] = {
        {0x40680,5,{0x55,0x8B,0xEC,0x6A,0xFF}}, {0x34440,8,{0x55,0x8B,0xEC,0x56,0x8B,0xF1,0xC7,0x06}},
        {0x6BC20,8,{0x55,0x8B,0xEC,0xF3,0x0F,0x10,0x4D,0x08}}, {0x6BD50,9,{0x55,0x8B,0xEC,0x83,0xEC,0x10,0x8A,0x45,0x08}},
        {0x34470,11,{0x55,0x8B,0xEC,0x53,0x8B,0x5D,0x08,0x56,0x57,0x8B,0xF9}},
        {0x34BA0,10,{0x55,0x8B,0xEC,0x51,0x56,0x8B,0xF1,0x57,0x8B,0x46}},
        {0x6BFB0,10,{0x55,0x8B,0xEC,0x56,0x57,0x8B,0x7D,0x08,0x8B,0xF1}}
    };
    for (unsigned int i = 0; i < sizeof(funcs)/sizeof(funcs[0]); ++i) memcpy(image + funcs[i].rva, funcs[i].code, funcs[i].length);
    image[0x34BA0 + 10] = 0x1C;
    static const unsigned char action_return[] = {0x5F,0x5E,0x5B,0x31,0xC0,0x5D,0xC2,0x10,0x00};
    memcpy(image + STEAM_ACTOR_ACTION_RVA + 11, action_return, sizeof(action_return));
    *(uintptr_t*)(base + STEAM_MAN_VTABLE_RVA) = base + 0x34440;
    *(uintptr_t*)(base + STEAM_MAN_VTABLE_RVA + 4) = base + STEAM_ACTOR_ACTION_RVA;
    *(uintptr_t*)(base + STEAM_GAME_VTABLE_RVA + 8 * 4) = base + 0x40680;
    image[0x394B4 + 5] = 4; *(uintptr_t*)(base + 0x39484 + 4 * 4) = base + 0x3934C;
    CHECK(!steam_action_hook_install(0, replica, shot) && steam_action_hook_stop());
    CHECK(steam_action_hook_install(base, replica, shot));
    union { ActorAction function; uintptr_t pointer; } action;
    action.pointer = *(uintptr_t*)(base + STEAM_MAN_VTABLE_RVA + 4);
    CHECK(action.function(NULL, NULL, 0x55, 0, 0, 0) == 0);
    CHECK(steam_action_hook_stop() && steam_action_hook_stop());
    CHECK(action.function(NULL, NULL, 0x82, 0, 0, 0) == 0); /* Original still callable. */
    CHECK(steam_action_hook_install(base, replica, shot) && steam_action_hook_stop());
    VirtualFree((void*)device_table, 0, MEM_RELEASE); VirtualFree(image, 0, MEM_RELEASE);
    puts("Steam hook checks passed: map-load ABI/generation, pending device, EndScene ABI, foreign preservation, protection, failed install, idempotent stop and reinstall.");
    return 0;
}
