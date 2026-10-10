#ifndef ASMP_STEAM_PROFILE_H
#define ASMP_STEAM_PROFILE_H
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
/* REA/Ghidra, supported EXE: UNIT puppet experiment. Live inventory found
   the flying drone VID290 is also class 2; no loaded class-4 VID was found. */
#define STEAM_UNIT_VTABLE_RVA 0xDDB08u
#define STEAM_UNIT_CLASS 2u
#define STEAM_UNIT_DESTROY_RVA 0x72800u
#define STEAM_UNIT_ACTION_RVA 0x72870u
#define STEAM_UNIT_UPDATE_RVA 0x6FDA0u
#define STEAM_UNIT_MOVEMENT_RVA 0x73290u
/* Ground UNIT's linked weapon part, observed live as VID108/class9 and
   verified against the supported EXE's SPRITE table (common action/update). */
#define STEAM_PUPPET_PART_CLASS 9u
#define STEAM_SPRITE_VTABLE_RVA 0xDD204u
#define STEAM_SPRITE_DESTROY_RVA 0x68940u
#define STEAM_SPRITE_ACTION_RVA 0x68970u
#define STEAM_ENTITY_PARENT_OFFSET 0x44u
#define STEAM_ENTITY_GOAL_OFFSET 0x3Cu
#define STEAM_ENTITY_COMMAND_MASK 0x7Cu
#define STEAM_ENTITY_COMMAND_TIMER_OFFSET 0x54u
#define STEAM_ENTITY_STACK_COUNT_OFFSET 0x60u
#define STEAM_UNIT_AVOIDANCE_OFFSET 0x8Cu
#define STEAM_UNIT_BEHAVIOR_OFFSET 0x90u
#define STEAM_ENTITY_Z_VELOCITY_OFFSET 0x24u
#define STEAM_ACT_NEXT_COMMAND 130u
#define STEAM_ACT_CLEAR_COMMAND 42u
#define STEAM_ACT_CLEAR_STACK 72u
#define STEAM_ACT_STOP 39u
#define STEAM_ACT_SET_BEHAVIOR 95u
#define STEAM_ANIM_IDLE 0u
#define STEAM_ANIM_RUN 2u
/* REA saved MAP loader and SPRITE update/action/teardown; private combat VIDs.
   0x440 is also animation script 14. Never edit the shared loaded descriptor. */
#define STEAM_GAME_VID_TABLE_OFFSET 0x2BCu
#define STEAM_VID_SIZE 0x490u
#define STEAM_ANIMATION_COUNT 17u
#define STEAM_VID_ANIMATION_SCRIPTS_OFFSET 0x408u
#define STEAM_VID_CREATIONS_OFFSET 0x250u
#define STEAM_VID_DAMAGE_SCRIPT_OFFSET 0x450u
#define STEAM_VID_COLLISION_SCRIPT_OFFSET 0x454u
#define STEAM_VID_DAMAGE_OFFSET 0x48u
#define STEAM_VID_SERVICE_OFFSET 0x468u
#define STEAM_VID_RESOURCE_FLAGS_OFFSET 0x47Cu
#define STEAM_VID_SPECIAL_FACTORY_FLAG 0x10u
#define STEAM_ACT_DAMAGE 85u
#define STEAM_ACT_SET_ANIMATION 15u
#define STEAM_ACT_SET_HP 88u
#define STEAM_ACT_GET_HP 87u
#define STEAM_ANIM_ATTACK 8u
#define STEAM_ACT_USE_AMMO 0x5Du
/* Native SPRITE CHANGE_VID: a=table index, b=animation (-1 keeps current). */
#define STEAM_ACT_CHANGE_VID 0x3Eu
#define STEAM_ANIM_DEATH 15u
#define STEAM_ANIM_GIB_DEATH 16u
/* REA death graph: CANNON (class5) and BUILDED_TERRAIN (class8), both
   use SPRITE teardown. CANNON collision returns immediately for mask0. */
#define STEAM_CANNON_VTABLE_RVA 0xD94C8u
#define STEAM_CANNON_DESTROY_RVA 0x18140u
#define STEAM_TERRAIN_VTABLE_RVA 0xDA7E0u
#define STEAM_TERRAIN_DESTROY_RVA 0x38380u
#define STEAM_VID_COLLISION_MASK_OFFSET 0x18u
#define STEAM_VID_HEALTH_OFFSET 0x28u
#define STEAM_VID_CREATION_COUNT_OFFSET 0x294u
#define STEAM_PLAYER_ACCESSOR_RVA 0x3B5F0u
/* Entity factory (GAME slot 8), MAN destructor (MAN slot 0), and MAN methods. */
#define STEAM_ACTOR_CREATE_RVA 0x40680u
#define STEAM_ACTOR_DESTROY_RVA 0x34440u
#define STEAM_ACTOR_ACTION_RVA 0x34470u
#define STEAM_ACTOR_MOVE_RVA 0x6BC20u
#define STEAM_ACTOR_ROTATE_RVA 0x6BD50u
#define STEAM_ACTOR_WEAPON_RVA 0x34BA0u
#define STEAM_ACTOR_HEALTH_RVA 0x6BFB0u
/* Factory switch: entity class 7 (MAN) -> index table (classes start at 2)
   -> jump table entry 4, whose arm constructs MAN. */
#define STEAM_MAN_CLASS 7u
#define STEAM_MAN_JUMP_INDEX 4u
#define STEAM_CLASS_INDEX_TABLE_RVA 0x394B4u
#define STEAM_CLASS_JUMP_TABLE_RVA 0x39484u
#define STEAM_MAN_CONSTRUCT_RVA 0x3934Cu
#define STEAM_RENDER_DEVICE_OFFSET 0xE28u
#define STEAM_WINDOW_PROC_RVA 0x40290u
#define STEAM_END_SCENE_SLOT 42u
#define STEAM_GAME_TICK_RVA 0x40CA0u
#define STEAM_GAME_TICK_SLOT 3u
#define STEAM_LOAD_MAP_RVA 0x3C7D0u
#define STEAM_LOAD_MAP_SLOT 6u
/* Native UI/text bindings used by ui.c. STEXT vtable slots 0 and 1
   are its destructor and action; its owned text string is at +0x74. */
#define STEAM_STRING_CREATE_RVA 0x25EA0u
#define STEAM_STRING_ASSIGN_RVA 0x44CB0u
#define STEAM_DRAW_RECT_RVA 0x271B0u
/* Render::DrawText(this=Render, x, y, const char*, argb) draws via the D3D9
   font at Render+0xE34; verified statically (Ghidra), live use pending. */
#define STEAM_DRAW_TEXT_RVA 0x306A0u
#define STEAM_SET_ANIMATION_RVA 0x6B970u
#define STEAM_STEXT_VTABLE_RVA 0xDA7BCu
#define STEAM_STEXT_DESTRUCTOR_RVA 0x38550u
#define STEAM_STEXT_ACTION_RVA 0x260E0u
#define STEAM_STEXT_TEXT_OFFSET 0x74u
/* GAME menu entity list uses the same count/capacity/entries layout as world lists. */
#define STEAM_GAME_MENU_LIST_OFFSET 0x274u
/* common_statebar.lgc builds the weapon panel once per level and hides icons of
   items not owned at that moment (ACT_HAVE_ITEM). Weapon icon VID 710 (direction
   byte slot*256/10) and ammo gauge VID 745 (direction byte = ammo fraction);
   ACT_SET_INVISIBLE (98) with 0 shows one. */
#define STEAM_STATEBAR_WEAPON_VID 710u
#define STEAM_STATEBAR_AMMO_VID 745u
#define STEAM_ACT_SET_INVISIBLE 98u
#define STEAM_GAME_CAMERA_X_OFFSET 0x54u
#define STEAM_GAME_CAMERA_Y_OFFSET 0x58u
#define STEAM_ARMY_INDEX_OFFSET 0x240u
#define STEAM_ARMY_ARRAY_OFFSET 0x244u
#define STEAM_ARMY_PLAYER_OFFSET 0x10u
#define STEAM_WORLD_LIST_OFFSET 0x5Cu
#define STEAM_GAME_MAP_PATH_OFFSET 0x20u
/* load_map writes the level's start time here at 43CA51..43CA5C. */
#define STEAM_GAME_MAP_STARTED_OFFSET 0x34u
#define STEAM_WORLD_LIST_COUNT 19u
/* MAP::tick (0x440CA0) compares its category index with 0x10. Later MAP
   members include script lists, not entity lists. Use this bound when scanning. */
#define STEAM_TICK_ENTITY_LIST_COUNT 16u
#define STEAM_LIST_SIZE 0x10u
#define STEAM_VID_CLASS_OFFSET 0x10u
#define STEAM_VID_BEHAVIOR_OFFSET 0x45Cu
/* Loaded MAP behavior table records: loader 43DE61 uses stride 0x264.
   UNIT constructor reads the command/AI flags at behavior+0x30. */
#define STEAM_BEHAVIOR_SIZE 0x264u
#define STEAM_BEHAVIOR_UNIT_FLAGS_OFFSET 0x30u
#define STEAM_VID_CATEGORY_OFFSET 0x38Cu
#define STEAM_VID_COUNTERS_OFFSET 0x3A8u
#define STEAM_VID_COUNTERS_SIZE 0x30u
#define STEAM_VID_CREATE_SCRIPT_OFFSET 0x440u
#define STEAM_VID_DELETE_SCRIPT_OFFSET 0x44Cu
#define STEAM_ENTITY_X_OFFSET 0x30u
#define STEAM_ENTITY_Y_OFFSET 0x34u
#define STEAM_ENTITY_Z_OFFSET 0x38u
#define STEAM_ENTITY_ANIM_OFFSET 0x4Cu
#define STEAM_ENTITY_DIRECTION_OFFSET 0x50u
#define STEAM_ENTITY_HEALTH_OFFSET 0x58u
#define STEAM_ENTITY_CHILD_OFFSET 0x40u
#define STEAM_ENTITY_FRAME_FIRST_OFFSET 0x08u
#define STEAM_ENTITY_FRAME_CURRENT_OFFSET 0x0Cu
#define STEAM_ENTITY_FRAME_LAST_OFFSET 0x10u
/* Torso attack readiness, mirrored from MAN::action(0x25) at 0x434D10 and the
   torso attack at 0x46A94C: a new target is ignored while the cooldown is above
   the idle value (0x46AAC5 sets weapon delay + idle when it fires) or while the
   attack animation is still playing. */
#define STEAM_TORSO_COOLDOWN_OFFSET 0x54u
#define STEAM_TORSO_COOLDOWN_IDLE 5000u
#define STEAM_TORSO_ATTACK_ANIM 8u
#define STEAM_ENTITY_VELOCITY_OFFSET 0x20u
#define STEAM_ENTITY_FLAGS_OFFSET 0x28u
/* ENTITY::calculate_movement (0x46E7B0) accelerates with this flag set;
   otherwise it decelerates. MAN::action(0x82) chooses idle/run from velocity. */
#define STEAM_ENTITY_MOVING_FLAG 0x80u
/* MAN::set_armed_weapon at 0x434BA0 follows player VID -> linked weapon VID.
   The linked VID index is slot + 10; slot 10 passed to the setter aliases 0. */
#define STEAM_ENTITY_VID_OFFSET 0x1Cu
#define STEAM_VID_LINKED_OFFSET 0x5Cu
#define STEAM_VID_INDEX_OFFSET 0x04u
/* VID maximum health per army: int32 at +0x3D8 + army*4 (read by 0x46BFB0). */
#define STEAM_VID_MAX_HEALTH_OFFSET 0x3D8u
#define STEAM_DEFAULT_MAX_HEALTH 110
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
/* Stored counts are plain rounds (a selected slot 5 with 172 rounds stores 172).
   Granting weapons tops every stored slot up to at least this many. */
#define STEAM_GRANT_AMMO 500u

#endif
