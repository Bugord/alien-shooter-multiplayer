#if defined(ASMP_STEAM_BUILD)
#include <windows.h>
#include <string.h>
#include "steam_display_hook.h"
typedef int (__fastcall* WindowProc)(void*, void*, HWND, unsigned int, uintptr_t, intptr_t);
typedef HRESULT (__stdcall* EndScene)(void*);
static WindowProc wnd_original;
static EndScene end_original;
static SteamDrawCallback paint;
static void* volatile* wnd_slot;
static void* volatile* end_slot;
static volatile LONG enabled, ready, frames;
static void* pointer_window;
static void* pointer_end;
static int replace(void* volatile* slot, void* before, void* after) {
    DWORD protection, ignored;
    if (!VirtualProtect((void*)slot, sizeof(void*), PAGE_READWRITE, &protection)) return 0;
    void* previous = InterlockedCompareExchangePointer(slot, after, before);
    int restored = VirtualProtect((void*)slot, sizeof(void*), protection, &ignored) != 0;
    return previous == before && restored;
}
static int __fastcall window_proc(void* game, void* unused, HWND window, unsigned int message, uintptr_t wparam, intptr_t lparam) {
    (void)unused;
    /* Original mod keeps simulation/network alive when the window loses focus. */
    if (message == WM_ACTIVATEAPP && InterlockedCompareExchange(&enabled, 0, 0)) wparam = 1;
    return wnd_original(game, NULL, window, message, wparam, lparam);
}
static HRESULT __stdcall end_scene(void* device) {
    if (InterlockedCompareExchange(&enabled, 0, 0)) { paint(); InterlockedIncrement(&frames); }
    return end_original(device);
}
int steam_display_hook_install(uintptr_t base, SteamDrawCallback draw) {
    if (wnd_slot || !draw) return 0;
    __try {
        uintptr_t render = *(uintptr_t*)(base + STEAM_RENDER_PTR_RVA);
        uintptr_t device = render ? *(uintptr_t*)(render + 0xE28) : 0;
        if (!device) return 0;
        static const unsigned char prefix[] = {0x55,0x8B,0xEC,0x6A,0xFF};
        wnd_slot = (void* volatile*)(base + STEAM_GAME_VTABLE_RVA + 4);
        if (*wnd_slot != (void*)(base + 0x40290) || memcmp((void*)(base + 0x40290), prefix, sizeof(prefix))) {
            wnd_slot = NULL; return 0;
        }
        /* Steam imports D3D9; IDirect3DDevice9::EndScene is vtable slot 42.
           +0xE28 is verified by the renderer's SetTexture/SetRenderState calls. */
        end_slot = (void* volatile*)(*(uintptr_t*)device + 42 * 4);
        union { WindowProc function; void* address; } w;
        union { EndScene function; void* address; } e;
        w.address = *wnd_slot; wnd_original = w.function; w.function = window_proc; pointer_window = w.address;
        e.address = *end_slot; end_original = e.function; e.function = end_scene; pointer_end = e.address;
        if (!end_original) { wnd_slot = end_slot = NULL; return 0; }
        paint = draw; InterlockedExchange(&enabled, 1);
        e.function = end_original;
        if (!replace(wnd_slot, (void*)(base + 0x40290), pointer_window) ||
            !replace(end_slot, e.address, pointer_end)) {
            steam_display_hook_stop(); return 0;
        }
        InterlockedExchange(&ready, 1); return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { steam_display_hook_stop(); return 0; }
}
int steam_display_hook_ready(void) { return InterlockedCompareExchange(&ready, 0, 0) != 0; }
long steam_display_hook_frames(void) { return InterlockedCompareExchange(&frames, 0, 0); }
int steam_display_hook_stop(void) {
    InterlockedExchange(&enabled, 0); InterlockedExchange(&ready, 0); int ok = 1;
    if (wnd_slot) {
        union { WindowProc function; void* address; } p; p.function = wnd_original;
        ok = *wnd_slot == p.address || (*wnd_slot == pointer_window && replace(wnd_slot, pointer_window, p.address));
    }
    if (end_slot) {
        union { EndScene function; void* address; } p; p.function = end_original;
        ok = (*end_slot == p.address || (*end_slot == pointer_end && replace(end_slot, pointer_end, p.address))) && ok;
    }
    return ok;
}
#endif
