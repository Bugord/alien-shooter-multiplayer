#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/window_mode.h"
#include "../src/profile.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
int main(void) {
    static const unsigned char signature[] = {
        0xF3,0x0F,0x11,0x0C,0x24,0x68,0x00,0x05,0x00,0x00,0xFF,0xB7,0x24,0x0E,0x00,0x00,0x6A,0x00
    };
    unsigned char* image = VirtualAlloc(NULL, 0x110000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(image);
    memcpy(image + STEAM_RENDER_WIDTH_CONTEXT_RVA, signature, sizeof(signature));
    DWORD old;
    CHECK(VirtualProtect(image, 0x110000, PAGE_EXECUTE_READ, &old));
    HANDLE process = GetCurrentProcess();
    CHECK(!window_mode_limit_width(process, (uintptr_t)image, 100));
    CHECK(!memcmp(image + STEAM_RENDER_WIDTH_CONTEXT_RVA, signature, sizeof(signature)));
    CHECK(window_mode_limit_width(process, (uintptr_t)image, 800));
    CHECK(*(unsigned int*)(image + STEAM_RENDER_WIDTH_OPERAND_RVA) == 800);
    CHECK(!memcmp(image + STEAM_RENDER_WIDTH_CONTEXT_RVA, signature, 6));
    CHECK(!memcmp(image + STEAM_RENDER_WIDTH_CONTEXT_RVA + 10, signature + 10, sizeof(signature) - 10));
    MEMORY_BASIC_INFORMATION info;
    CHECK(VirtualQuery(image + STEAM_RENDER_WIDTH_OPERAND_RVA, &info, sizeof(info)));
    CHECK(info.Protect == PAGE_EXECUTE_READ);
    CHECK(!window_mode_limit_width(process, (uintptr_t)image, 640)); /* Reject a changed site. */
    CHECK(VirtualProtect(image, 0x110000, PAGE_READWRITE, &old));
    float render[0x250 / sizeof(float)] = {0};
    render[STEAM_RENDER_WIDTH_OFFSET / sizeof(float)] = 800;
    render[STEAM_RENDER_WIDTH_OFFSET / sizeof(float) + 1] = 600;
    *(uintptr_t*)(image + STEAM_RENDER_PTR_RVA) = (uintptr_t)render;
    int w, h;
    CHECK(window_mode_render_size(process, (uintptr_t)image, &w, &h) && w == 800 && h == 600);
    render[STEAM_RENDER_WIDTH_OFFSET / sizeof(float)] = 0;
    CHECK(!window_mode_render_size(process, (uintptr_t)image, &w, &h));
    CHECK(VirtualFree(image, 0, MEM_RELEASE));
    puts("Window mode checks passed: signature guards, operand-only update, protection restoration, actual render dimensions.");
    return 0;
}
