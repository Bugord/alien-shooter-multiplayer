#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../src/profile.h"
#include "../../asmp-dll/src/game/ui.h"

/* Fake engine image. A machine-code stub at STEAM_DRAW_TEXT_RVA records the
   thiscall arguments of Render::DrawText: ECX, x, y, text pointer, color. */
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static void install_recorder(unsigned char* image, unsigned int rva, const unsigned char* prefix, unsigned char fifth, unsigned int args, uintptr_t* out)
{
    unsigned char stub[96]; unsigned int n = 0;
    memcpy(stub, prefix, 5); n = 5;
    stub[n++] = fifth;                                           /* finishes the 6-byte prologue (mov esi,ecx / sub esp,imm8 operand) */
    stub[n++] = 0x89; stub[n++] = 0x0D; *(uintptr_t*)(stub + n) = (uintptr_t)&out[0]; n += 4;
    for (unsigned int i = 0; i < args; ++i) {
        stub[n++] = 0x8B; stub[n++] = 0x45; stub[n++] = (unsigned char)(8 + 4 * i);  /* mov eax, [ebp+8+4i] */
        stub[n++] = 0xA3; *(uintptr_t*)(stub + n) = (uintptr_t)&out[1 + i]; n += 4;
    }
    stub[n++] = 0x8B; stub[n++] = 0xE5; stub[n++] = 0x5D; stub[n++] = 0xC2; stub[n++] = (unsigned char)(4 * args); stub[n++] = 0x00;
    memcpy(image + rva, stub, n);
}
static void put(unsigned char* image, unsigned int rva, const unsigned char* bytes, unsigned int n) { memcpy(image + rva, bytes, n); }

int main(void)
{
    unsigned char* image = VirtualAlloc(NULL, 0x276000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    unsigned char* game = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x300);
    unsigned char* render = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x300);
    unsigned char* entity = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x100);
    uintptr_t* record = (uintptr_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 5 * sizeof(uintptr_t));
    uintptr_t* rect = (uintptr_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 6 * sizeof(uintptr_t));
    if (!image || !game || !render || !entity || !record || !rect) return 2;
    uintptr_t base = (uintptr_t)image;
    static const unsigned char prefix[5] = {0x55, 0x8B, 0xEC, 0x56, 0x8B}, rect_prefix[5] = {0x55, 0x8B, 0xEC, 0x83, 0xEC};
    install_recorder(image, STEAM_DRAW_TEXT_RVA, prefix, 0xF1, 4, record);
    install_recorder(image, STEAM_DRAW_RECT_RVA, rect_prefix, 0x10, 5, rect);
    /* Other bindings checked by ui_bind. */
    static const unsigned char sig_load[5] = {0x55,0x8B,0xEC,0x6A,0xFF}, sig_create[5] = {0x55,0x8B,0xEC,0x53,0x57},
        sig_assign[5] = {0x55,0x8B,0xEC,0x53,0x8B}, sig_anim[5] = {0x55,0x8B,0xEC,0x51,0x53};
    put(image, STEAM_LOAD_MAP_RVA, sig_load, 5); put(image, STEAM_STRING_CREATE_RVA, sig_create, 5);
    put(image, STEAM_STRING_ASSIGN_RVA, sig_assign, 5);
    put(image, STEAM_SET_ANIMATION_RVA, sig_anim, 5);
    *(uintptr_t*)(image + STEAM_GAME_VTABLE_RVA + STEAM_LOAD_MAP_SLOT * sizeof(uintptr_t)) = base + STEAM_LOAD_MAP_RVA;
    *(uintptr_t*)(image + STEAM_STEXT_VTABLE_RVA) = base + STEAM_STEXT_DESTRUCTOR_RVA;
    *(uintptr_t*)(image + STEAM_STEXT_VTABLE_RVA + sizeof(uintptr_t)) = base + STEAM_STEXT_ACTION_RVA;

    /* Not bound yet: nothing is drawn (and nothing crashes). */
    *(uintptr_t*)(image + STEAM_RENDER_PTR_RVA) = (uintptr_t)render;
    *(float*)(game + STEAM_GAME_CAMERA_X_OFFSET) = 100.0f; *(float*)(game + STEAM_GAME_CAMERA_Y_OFFSET) = 50.0f;
    *(float*)(entity + STEAM_ENTITY_X_OFFSET) = 300.0f; *(float*)(entity + STEAM_ENTITY_Y_OFFSET) = 250.0f;
    *(float*)(entity + STEAM_ENTITY_Z_OFFSET) = 99.0f;
    ui_name_label((uintptr_t)game, (uintptr_t)entity, "Mirror");
    CHECK(!record[3]);

    CHECK(ui_bind(base) == 1);
    ui_name_label((uintptr_t)game, (uintptr_t)entity, "");
    CHECK(!record[3]);
    ui_name_label((uintptr_t)game, 0, "Mirror");
    CHECK(!record[3]);
    ui_name_label((uintptr_t)game, (uintptr_t)entity, "Mirror");
    CHECK(record[0] == (uintptr_t)render);
    CHECK(*(float*)&record[1] == 300.0f - 100.0f - 40.0f);          /* x - cameraX - 40 */
    CHECK(*(float*)&record[2] == 250.0f - 50.0f - 92.0f);           /* y - cameraY - 92; z is not subtracted */
    CHECK(record[3] && !strcmp((const char*)record[3], "Mirror"));
    CHECK(record[4] == 0xFFFFFFFFu);

    /* Health bar: width follows the owner's maximum, with a 110 fallback. */
    ui_health_bar((uintptr_t)game, (uintptr_t)entity, 100, 200);
    CHECK(*(float*)&rect[3] == 160.0f + 1.0f + 100.0f * 78.0f / 200.0f);  /* last rect: x+1+width */
    ui_health_bar((uintptr_t)game, (uintptr_t)entity, 500, 200);          /* overhealth fills the bar */
    CHECK(*(float*)&rect[3] == 160.0f + 1.0f + 78.0f);
    ui_health_bar((uintptr_t)game, (uintptr_t)entity, 55, 0);             /* unknown maximum */
    CHECK(*(float*)&rect[3] == 160.0f + 1.0f + 55.0f * 78.0f / 110.0f);

    /* A missing render pointer is ignored. */
    memset(record, 0, 5 * sizeof(uintptr_t));
    *(uintptr_t*)(image + STEAM_RENDER_PTR_RVA) = 0;
    ui_name_label((uintptr_t)game, (uintptr_t)entity, "Mirror");
    CHECK(!record[3]);
    ui_stop();

    /* A changed function prologue is rejected at bind time. */
    image[STEAM_DRAW_TEXT_RVA + 3] = 0x90;
    CHECK(ui_bind(base) == 0);
    if (failures) return 1;
    puts("UI label checks passed");
    return 0;
}
