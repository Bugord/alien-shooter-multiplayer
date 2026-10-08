#include <windows.h>
#include <string.h>
#include "display_hook.h"
#include "slot_hook.h"
typedef int (__fastcall* WindowProc)(void*, void*, HWND, unsigned int, uintptr_t, intptr_t);
typedef HRESULT (__stdcall* EndScene)(void*);
static struct {
    WindowProc wnd_original;
    EndScene end_original;
    DrawCallback paint;
    SlotHook wnd_hook, end_hook;
    volatile LONG enabled, ready, frames;
    void* pointer_window;
    void* pointer_end;
} display;
static int __fastcall window_proc(void* game, void* unused, HWND window, unsigned int message, uintptr_t wparam, intptr_t lparam) {
    (void)unused;
    /* Keep simulation/network alive when the window loses focus. */
    if (message == WM_ACTIVATEAPP && InterlockedCompareExchange(&display.enabled, 0, 0)) wparam = 1;
    return display.wnd_original(game, NULL, window, message, wparam, lparam);
}
static HRESULT __stdcall end_scene(void* device) {
    if (InterlockedCompareExchange(&display.enabled, 0, 0)) { display.paint(); InterlockedIncrement(&display.frames); }
    return display.end_original(device);
}
enum DisplayResult display_hook_install(uintptr_t base, DrawCallback draw) {
    if (display_hook_ready()) return DISPLAY_OK;
    if (display.wnd_hook.slot || display.end_hook.slot || !draw) return DISPLAY_FAILED;
    __try {
        uintptr_t render = *(uintptr_t*)(base + STEAM_RENDER_PTR_RVA);
        uintptr_t device = render ? *(uintptr_t*)(render + STEAM_RENDER_DEVICE_OFFSET) : 0;
        if (!device) return DISPLAY_PENDING;
        static const unsigned char prefix[] = {0x55,0x8B,0xEC,0x6A,0xFF};
        void* volatile* wnd_slot = (void* volatile*)(base + STEAM_GAME_VTABLE_RVA + 4);
        if (*wnd_slot != (void*)(base + STEAM_WINDOW_PROC_RVA) || memcmp((void*)(base + STEAM_WINDOW_PROC_RVA), prefix, sizeof(prefix)))
            return DISPLAY_FAILED;
        void* volatile* end_slot = (void* volatile*)(*(uintptr_t*)device + STEAM_END_SCENE_SLOT * sizeof(void*));
        union { WindowProc function; void* address; } w;
        union { EndScene function; void* address; } e;
        w.address = *wnd_slot; display.wnd_original = w.function; w.function = window_proc; display.pointer_window = w.address;
        e.address = *end_slot; display.end_original = e.function; e.function = end_scene; display.pointer_end = e.address;
        if (!display.end_original) return DISPLAY_FAILED;
        display.paint = draw; InterlockedExchange(&display.enabled, 1);
        e.function = display.end_original;
        if (slot_install(&display.wnd_hook, wnd_slot, (void*)(base + STEAM_WINDOW_PROC_RVA), display.pointer_window) != SLOT_OK ||
            slot_install(&display.end_hook, end_slot, e.address, display.pointer_end) != SLOT_OK) {
            display_hook_stop(); return DISPLAY_FAILED;
        }
        InterlockedExchange(&display.ready, 1); return DISPLAY_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) { display_hook_stop(); return DISPLAY_FAILED; }
}
int display_hook_ready(void) { return InterlockedCompareExchange(&display.ready, 0, 0) != 0; }
long display_hook_frames(void) { return InterlockedCompareExchange(&display.frames, 0, 0); }
int display_hook_stop(void) {
    InterlockedExchange(&display.enabled, 0);
    int end_ok = slot_stop(&display.end_hook) == SLOT_OK;
    int wnd_ok = slot_stop(&display.wnd_hook) == SLOT_OK;
    if (end_ok && wnd_ok) InterlockedExchange(&display.ready, 0);
    return end_ok && wnd_ok;
}
