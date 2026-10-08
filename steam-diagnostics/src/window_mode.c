#include <string.h>
#include <math.h>
#include "window_mode.h"
#include "profile.h"

int window_mode_limit_width(HANDLE process, uintptr_t image, unsigned int width) {
    /* RENDER constructor supplies a fixed 1280 cap to its adapter-mode chooser.
       Keep the chooser, device setup, projection and input paths together. */
    static const unsigned char expected[] = {
        0xF3,0x0F,0x11,0x0C,0x24,0x68,0x00,0x05,0x00,0x00,0xFF,0xB7,0x24,0x0E,0x00,0x00,0x6A,0x00
    };
    unsigned char actual[sizeof(expected)];
    SIZE_T count = 0;
    if (width < 640 || width > 1920 || !ReadProcessMemory(process,
        (void*)(image + STEAM_RENDER_WIDTH_CONTEXT_RVA), actual, sizeof(actual), &count) ||
        count != sizeof(actual) || memcmp(actual, expected, sizeof(actual))) return 0;
    void* operand = (void*)(image + STEAM_RENDER_WIDTH_OPERAND_RVA);
    DWORD protection, ignored;
    if (!VirtualProtectEx(process, operand, sizeof(width), PAGE_EXECUTE_READWRITE, &protection)) return 0;
    int ok = WriteProcessMemory(process, operand, &width, sizeof(width), &count) && count == sizeof(width);
    unsigned int readback = 0;
    ok = ok && ReadProcessMemory(process, operand, &readback, sizeof(readback), &count) &&
        count == sizeof(readback) && readback == width;
    if (!ok) {
        unsigned int original = 1280;
        WriteProcessMemory(process, operand, &original, sizeof(original), NULL);
    }
    int flushed = FlushInstructionCache(process, operand, sizeof(width));
    int restored = VirtualProtectEx(process, operand, sizeof(width), protection, &ignored);
    return ok && flushed && restored;
}
int window_mode_render_size(HANDLE process, uintptr_t image, int* width, int* height) {
    uintptr_t render = 0;
    SIZE_T count;
    float dimensions[2];
    if (!ReadProcessMemory(process, (void*)(image + STEAM_RENDER_PTR_RVA), &render, sizeof(render), &count) ||
        count != sizeof(render) || !render ||
        !ReadProcessMemory(process, (void*)(render + STEAM_RENDER_WIDTH_OFFSET), dimensions, sizeof(dimensions), &count) ||
        count != sizeof(dimensions) || !isfinite(dimensions[0]) || !isfinite(dimensions[1]) ||
        dimensions[0] < 320 || dimensions[0] > 8192 || dimensions[1] < 240 || dimensions[1] > 8192) return 0;
    *width = (int)dimensions[0]; *height = (int)dimensions[1];
    return dimensions[0] == (float)*width && dimensions[1] == (float)*height;
}
