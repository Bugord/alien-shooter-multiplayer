#ifndef ASMP_STEAM_SLOT_HOOK_H
#define ASMP_STEAM_SLOT_HOOK_H
#include <windows.h>
#include <stdint.h>
enum SteamSlotResult { STEAM_SLOT_OK, STEAM_SLOT_USED, STEAM_SLOT_INVALID,
    STEAM_SLOT_PROTECT_FAILED, STEAM_SLOT_CHANGED, STEAM_SLOT_RESTORE_FAILED, STEAM_SLOT_BUSY };
typedef struct SteamSlotHook { void* volatile* slot; void* original; void* callback; LONG installed; } SteamSlotHook;
enum SteamSlotResult steam_slot_install(SteamSlotHook*, void* volatile* slot, void* original, void* callback);
enum SteamSlotResult steam_slot_stop(SteamSlotHook*);
/* Hook targets/private VIDs can outlive shutdown or already be fetched. */
int steam_module_pin(void);
#endif
