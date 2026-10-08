#ifndef ASMP_SLOT_HOOK_H
#define ASMP_SLOT_HOOK_H
#include <windows.h>
#include <stdint.h>
enum SlotResult { SLOT_OK, SLOT_USED, SLOT_INVALID,
    SLOT_PROTECT_FAILED, SLOT_CHANGED, SLOT_RESTORE_FAILED, SLOT_BUSY };
typedef struct SlotHook { void* volatile* slot; void* original; void* callback; LONG installed; } SlotHook;
enum SlotResult slot_install(SlotHook*, void* volatile* slot, void* original, void* callback);
enum SlotResult slot_stop(SlotHook*);
/* Hook targets/private VIDs can outlive shutdown or already be fetched. */
int module_pin(void);
#endif
