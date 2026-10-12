#ifndef ASMP_UNIT_PUPPET_H
#define ASMP_UNIT_PUPPET_H
#include "actor.h"
#include "unit_result.h"
#define UNIT_PUPPET_CAPACITY 64u

/* One puppet slot: a borrowed native UNIT and its optional part. The struct is
   private to unit_puppet.c. Every per-slot function below takes the slot as its
   first argument; there is no "currently selected" slot. The hooks find the
   owning slot of each native callback by entity identity. */
/* Every function below that returns enum UnitResult follows unit_result.h:
   UNIT_OK done,
 * UNIT_REFUSED not done, UNIT_FAULT native fault. */
typedef struct UnitPuppet UnitPuppet;

/* Returns slot `index`, or NULL when index >= UNIT_PUPPET_CAPACITY. Slots are
   static and live as long as the DLL; every function rejects a NULL slot. */
UnitPuppet* unit_puppet_slot(unsigned int index);

typedef void(__fastcall* UnitMovement)(void*, void*);
typedef void(__fastcall* UnitAnimation)(void*, void*, unsigned int);
typedef struct UnitPuppetEngine
{
    ActorAction action;
    ActorDestroy destroy;
    UnitMovement movement;
    UnitAnimation animation;
    ActorMove move;
    ActorRotate rotate;
    ActorAction part_action;
    ActorDestroy part_destroy;
} UnitPuppetEngine;
enum UnitPuppetState
{
    UNIT_PUPPET_OFF,
    UNIT_PUPPET_READY,
    UNIT_PUPPET_ACTIVE,
    UNIT_PUPPET_LOST,
    UNIT_PUPPET_FAULT
};
typedef struct UnitPuppetPose
{
    float x, y, z, velocity;
    unsigned int direction, moving;
} UnitPuppetPose;
typedef struct UnitPuppetSample
{
    enum UnitPuppetState state;
    uintptr_t entity;
    unsigned int vid, class_id, animation, frame, behavior, command, stack, moving, direction;
    unsigned int frame_first, frame_last, child_frame_first, child_frame, child_frame_last,
        child_frame_changes;
    uintptr_t goal;
    uintptr_t child, child_goal;
    unsigned int child_command, child_stack, child_animation;
    float x, y, z, velocity;
    unsigned int decisions, movements, blocked, frame_changes;
    unsigned int replayed, duplicates;
    unsigned int attack_frame_changes;
    int health;
    unsigned int captured_attacks, captured_deaths;
    unsigned int death_animation;
    UnitPuppetPose death_pose;
    unsigned int form_revision, captured_forms;
    int transitioning;
    unsigned int predicted_hits;
    int predictive;
} UnitPuppetSample;
/* Game-thread callback. requested_vid is nonzero immediately before native
   CHANGE_VID; return a prepared private descriptor for that table entry. A zero
   request observes settled identity. No native allocation is performed here. */
typedef enum UnitResult (*UnitFormObserver)(uintptr_t game, uintptr_t entity,
                                            uintptr_t* private_vid, unsigned int* current_vid,
                                            unsigned int* revision, int* transitioning,
                                            unsigned int requested_vid);
void unit_puppet_set_form_observer(UnitFormObserver);

/* Each slot borrows one class-2 UNIT, optionally one class-9 part.
   Longer/different child chains are rejected. No allocation, VID editing or entity
   deletion. Game thread owns attach/apply/sample/release and every callback.
   Install before ticking; stop ONLY after game-thread release is acknowledged.
   The borrowed entity's old command/stack are consumed, not restored; release
   restores its behavior and idle pose so native AI can choose a fresh command.
   The DLL stays pinned while a fetched hook could still reference it. */
enum UnitResult unit_puppet_bind(uintptr_t base, UnitPuppetEngine*);
enum UnitResult unit_puppet_install(uintptr_t base, const UnitPuppetEngine*);
/* Scans the native tick's 16 entity lists; excludes player, unsupported child
   chains, other classes and death animations. */
enum UnitResult unit_puppet_attach_first(UnitPuppet* puppet, uintptr_t game, uintptr_t player);
/* Host observation leaves all original actions, goals and movement untouched.
   Only the scoped stock VID50 is accepted. Captures native USE_AMMO attack
   initiation (including a zero-cost melee attack), never inferred snapshots. */
enum UnitResult unit_puppet_watch_first(UnitPuppet* puppet, uintptr_t game, uintptr_t player);
enum UnitResult unit_puppet_watch_entity(UnitPuppet* puppet, uintptr_t game, uintptr_t player,
                                         uintptr_t entity);
/* Freeze AI/motion while preserving native incoming damage and death. */
enum UnitResult unit_puppet_watch_stationary(UnitPuppet* puppet, uintptr_t game, uintptr_t player,
                                             uintptr_t entity);
enum UnitResult unit_puppet_attach_entity(UnitPuppet* puppet, uintptr_t game, uintptr_t player,
                                          uintptr_t entity);
enum UnitResult unit_puppet_health(UnitPuppet* puppet, uintptr_t game, int health);
/* Opt in only after attaching fully sanitized private root/part descriptors.
   Incoming native damage may change cosmetic health/forms/death; authority is
   still external. The sample counter counts root DAMAGE calls, not SET_HP. */
enum UnitResult unit_puppet_prediction(UnitPuppet* puppet, uintptr_t game);
/* Diagnostic native-hit helper for an enabled predictive copy. freeze follows
   the verified lethal-DAMAGE/CHANGE_VID action sequence, not weapon collision. */
enum UnitResult unit_puppet_prediction_test_hit(UnitPuppet* puppet, uintptr_t game, int damage,
                                                int freeze);
/* Diagnostic host death trigger through the ordinary native SET_HP action. */
enum UnitResult unit_puppet_host_kill(UnitPuppet* puppet, uintptr_t game);
enum UnitResult unit_puppet_host_kill_variant(UnitPuppet* puppet, uintptr_t game,
                                              unsigned int animation);
/* Diagnostic form-stage trigger through native CHANGE_VID and idle/stop.
   Tests form ownership/presentation; does not simulate projectile collision. */
enum UnitResult unit_puppet_host_freeze(UnitPuppet* puppet, uintptr_t game);
enum UnitResult unit_puppet_apply(UnitPuppet* puppet, uintptr_t game, const UnitPuppetPose*);
/* Only sanitized private root/part descriptors are accepted. Sequence is local
   to this attached entity. Attack is visual only; death uses native SET_HP. */
enum UnitResult unit_puppet_replay(UnitPuppet* puppet, uintptr_t game, uint32_t sequence,
                                   unsigned int animation);
enum UnitResult unit_puppet_test_damage(UnitPuppet* puppet, uintptr_t game);
UnitPuppetSample unit_puppet_sample(UnitPuppet* puppet, uintptr_t game);
enum UnitResult unit_puppet_release(UnitPuppet* puppet, uintptr_t game);
enum UnitResult unit_puppet_stop(void);
#endif
