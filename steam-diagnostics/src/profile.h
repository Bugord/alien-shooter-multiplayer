#ifndef ASMP_DIAG_PROFILE_H
#define ASMP_DIAG_PROFILE_H
#include <stdint.h>
#include <stddef.h>
#if defined(_WIN64)
#error Build this diagnostic for x86.
#endif

/* Only the Steam EXE identified by this hash has been analyzed. */
#define STEAM_EXE_SHA256 "4dd960458d6fffcc9d00e9e7ba492739fb6d530d4c0b302f1c6baa8b55d9b142"
#define STEAM_GAME_PTR_RVA 0x122C20u
#define STEAM_GAME_VTABLE_RVA 0xDB034u
#define STEAM_MAN_VTABLE_RVA 0xDA438u
#define STEAM_PLAYER_ACCESSOR_RVA 0x3B5F0u
#define STEAM_ARMY_INDEX_OFFSET 0x240u
#define STEAM_ARMY_ARRAY_OFFSET 0x244u
#define STEAM_ARMY_PLAYER_OFFSET 0x10u
#define STEAM_ENTITY_X_OFFSET 0x30u
#define STEAM_ENTITY_Y_OFFSET 0x34u
#define STEAM_ENTITY_Z_OFFSET 0x38u
#define STEAM_ENTITY_ANIM_OFFSET 0x4Cu
#define STEAM_ENTITY_DIRECTION_OFFSET 0x50u
#define STEAM_ENTITY_HEALTH_OFFSET 0x58u

enum DiagStatus { DIAG_STARTING, DIAG_WAITING, DIAG_SAMPLING, DIAG_REJECTED, DIAG_ERROR };
int hash_file_sha256(const wchar_t* path, char output[65]);
#endif
