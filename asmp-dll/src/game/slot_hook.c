#include "slot_hook.h"
#include <string.h>
int module_pin(void) {
    static const unsigned char anchor = 0;
    HMODULE module;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        (LPCWSTR)&anchor, &module) != 0;
}
static enum SlotResult exchange(SlotHook* h, void* before, void* after) {
    MEMORY_BASIC_INFORMATION info; DWORD protection, ignored;
    if (!h->slot || (uintptr_t)h->slot % sizeof(void*) || !VirtualQuery((void*)h->slot, &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return SLOT_INVALID;
    DWORD writable = info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    if (!VirtualProtect((void*)h->slot, sizeof(void*), writable, &protection)) return SLOT_PROTECT_FAILED;
    void* previous = InterlockedCompareExchangePointer(h->slot, after, before);
    if (previous == before) InterlockedExchange(&h->installed, after == h->callback);
    if (!VirtualProtect((void*)h->slot, sizeof(void*), protection, &ignored)) return SLOT_RESTORE_FAILED;
    return previous == before ? SLOT_OK : SLOT_CHANGED;
}
enum SlotResult slot_install(SlotHook* h, void* volatile* slot, void* original, void* callback) {
    if (h->slot) return SLOT_USED;
    if (!original || !callback || !module_pin()) return SLOT_INVALID;
    h->slot = slot; h->original = original; h->callback = callback;
    enum SlotResult result = exchange(h, original, callback);
    if (result != SLOT_OK) {
        if (InterlockedCompareExchange(&h->installed, 0, 0)) slot_stop(h);
        else memset(h, 0, sizeof(*h));
    }
    return result;
}
enum SlotResult slot_stop(SlotHook* h) {
    if (!h->slot) return SLOT_OK;
    enum SlotResult result = exchange(h, h->callback, h->original);
    /* A foreign pointer is reported and preserved; it can still chain to us. */
    if (result == SLOT_OK) memset(h, 0, sizeof(*h));
    return result;
}
