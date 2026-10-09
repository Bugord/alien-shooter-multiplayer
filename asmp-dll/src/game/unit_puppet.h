#ifndef ASMP_UNIT_PUPPET_H
#define ASMP_UNIT_PUPPET_H
#include "actor.h"

typedef void (__fastcall* UnitMovement)(void*, void*);
typedef void (__fastcall* UnitAnimation)(void*, void*, unsigned int);
typedef struct UnitPuppetEngine {
    ActorAction action;
    ActorDestroy destroy;
    UnitMovement movement;
    UnitAnimation animation;
    ActorMove move;
    ActorRotate rotate;
    ActorAction part_action;
    ActorDestroy part_destroy;
} UnitPuppetEngine;
enum UnitPuppetState { UNIT_PUPPET_OFF, UNIT_PUPPET_READY, UNIT_PUPPET_ACTIVE,
    UNIT_PUPPET_LOST, UNIT_PUPPET_FAULT };
typedef struct UnitPuppetPose { float x, y, z, velocity; unsigned int direction, moving; } UnitPuppetPose;
typedef struct UnitPuppetSample {
    enum UnitPuppetState state;
    uintptr_t entity;
    unsigned int vid, class_id, animation, frame, behavior, command, stack, moving, direction;
    unsigned int frame_first, frame_last, child_frame_first, child_frame, child_frame_last, child_frame_changes;
    uintptr_t goal;
    uintptr_t child, child_goal;
    unsigned int child_command, child_stack, child_animation;
    float x, y, z, velocity;
    unsigned int decisions, movements, blocked, frame_changes;
    unsigned int replayed, duplicates;
    unsigned int attack_frame_changes;
    int health;
    unsigned int captured_attacks, captured_deaths;
} UnitPuppetSample;

/* Single borrowed class-2 UNIT, optionally one linked class-9 part.
   Longer/different child chains are rejected. No allocation, VID editing or entity
   deletion. Game thread owns attach/apply/sample/release and every callback.
   Install before ticking; stop ONLY after game-thread release is acknowledged.
   The borrowed entity's old command/stack are consumed, not restored; release
   restores its behavior and idle pose so native AI can choose a fresh command.
   The DLL stays pinned while a fetched hook could still reference it. */
int unit_puppet_bind(uintptr_t base, UnitPuppetEngine*);
int unit_puppet_install(uintptr_t base, const UnitPuppetEngine*);
/* Scans the native tick's 16 entity lists; excludes player, unsupported child
   chains, other classes and death animations. */
int unit_puppet_attach_first(uintptr_t game, uintptr_t player);
/* Host observation leaves all original actions, goals and movement untouched.
   Only the scoped stock VID50 is accepted. Captures native USE_AMMO attack
   initiation (including a zero-cost melee attack), never inferred snapshots. */
int unit_puppet_watch_first(uintptr_t game, uintptr_t player);
int unit_puppet_attach_entity(uintptr_t game, uintptr_t player, uintptr_t entity);
int unit_puppet_health(uintptr_t game, int health);
/* Diagnostic host death trigger through the ordinary native SET_HP action. */
int unit_puppet_host_kill(uintptr_t game);
int unit_puppet_apply(uintptr_t game, const UnitPuppetPose*);
/* Only sanitized private root/part descriptors are accepted. Sequence is local
   to this attached entity. Attack is visual only; death uses native SET_HP. */
int unit_puppet_replay(uintptr_t game, uint32_t sequence, unsigned int animation);
int unit_puppet_test_damage(uintptr_t game);
UnitPuppetSample unit_puppet_sample(uintptr_t game);
int unit_puppet_release(uintptr_t game);
int unit_puppet_stop(void);
#endif
