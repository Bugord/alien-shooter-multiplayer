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
#define STEAM_RENDER_PTR_RVA 0x102AD4u
#define STEAM_RENDER_WIDTH_OFFSET 0x244u
#define STEAM_RENDER_WIDTH_CONTEXT_RVA 0x2DC36u
#define STEAM_RENDER_WIDTH_OPERAND_RVA 0x2DC3Cu
#define STEAM_GAME_VTABLE_RVA 0xDB034u
#define STEAM_MAN_VTABLE_RVA 0xDA438u
#define STEAM_PLAYER_ACCESSOR_RVA 0x3B5F0u
#define STEAM_GAME_TICK_RVA 0x40CA0u
#define STEAM_GAME_TICK_SLOT 3u
#define STEAM_ARMY_INDEX_OFFSET 0x240u
#define STEAM_ARMY_ARRAY_OFFSET 0x244u
#define STEAM_ARMY_PLAYER_OFFSET 0x10u
#define STEAM_ENTITY_X_OFFSET 0x30u
#define STEAM_ENTITY_Y_OFFSET 0x34u
#define STEAM_ENTITY_Z_OFFSET 0x38u
#define STEAM_ENTITY_ANIM_OFFSET 0x4Cu
#define STEAM_ENTITY_DIRECTION_OFFSET 0x50u
#define STEAM_ENTITY_HEALTH_OFFSET 0x58u
/* MAN::set_armed_weapon at 0x434BA0 follows player VID -> linked weapon VID.
   The linked VID index is slot + 10; slot 10 passed to the setter aliases 0. */
#define STEAM_ENTITY_VID_OFFSET 0x1Cu
#define STEAM_VID_LINKED_OFFSET 0x5Cu
#define STEAM_VID_INDEX_OFFSET 0x04u
#define STEAM_WEAPON_VID_FIRST 10
#define STEAM_WEAPON_SLOT_COUNT 10u
/* UNIT::action(0x5C), 0x47292E: signed fixed-point ammo / 64, toward zero.
   MAN::action(0x5C), 0x4346EE: inactive slot N uses player+0x94+N*4.
   The selected slot's stored count is stale until switching weapons. */
#define STEAM_CURRENT_AMMO_OFFSET 0x84u
#define STEAM_AMMO_SCALE 64
#define STEAM_STORED_AMMO_BASE_OFFSET 0x94u
#define STEAM_STORED_AMMO_FIRST_SLOT 1u
#define STEAM_STORED_AMMO_COUNT 9u

enum DiagStatus { DIAG_STARTING, DIAG_WAITING, DIAG_SAMPLING, DIAG_REJECTED, DIAG_ERROR, DIAG_STOPPED };
int hash_file_sha256(const wchar_t* path, char output[65]);
#endif
