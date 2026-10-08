#ifndef ASMP_ACTOR_H
#define ASMP_ACTOR_H
#include <windows.h>
#include "probe.h"

/* Steam methods use ECX and callee-cleaned stack arguments. An unused EDX
   argument makes __fastcall equivalent to their __thiscall ABI in MSVC C. */
typedef void* (__fastcall* ActorCreate)(void*, void*, void*, float, float, float, int, void*);
typedef void* (__fastcall* ActorDestroy)(void*, void*, int);
typedef void (__fastcall* ActorMove)(void*, void*, float, float, float);
typedef unsigned char (__fastcall* ActorRotate)(void*, void*, unsigned int);
typedef int (__fastcall* ActorAction)(void*, void*, unsigned int, intptr_t, intptr_t, intptr_t);
typedef int (__fastcall* ActorWeapon)(void*, void*, int);
typedef void (__fastcall* ActorHealth)(void*, void*, int);
typedef struct ActorEngine {
    ActorCreate create;
    ActorDestroy destroy;
    ActorMove move;
    ActorRotate rotate;
    ActorAction action;
    ActorWeapon weapon;
    ActorHealth health;
} ActorEngine;

enum ActorEvent { ACTOR_NONE, ACTOR_SPAWNED, ACTOR_REMOVED, ACTOR_LOST,
    ACTOR_REJECTED, ACTOR_FAULT, ACTOR_POSE };
enum ActorReason { ACTOR_REASON_NONE, ACTOR_REASON_VID_CLASS, ACTOR_REASON_NO_CHILD,
    ACTOR_REASON_CATEGORY, ACTOR_REASON_FACTORY, ACTOR_REASON_REGISTRATION,
    ACTOR_REASON_ENTITY_TYPE, ACTOR_REASON_LOCAL_PLAYER, ACTOR_REASON_EXCEPTION,
    ACTOR_REASON_WEAPON, ACTOR_REASON_PRECONDITION };
typedef struct ActorResult {
    enum ActorEvent event;
    uintptr_t entity;
    unsigned int category, updates;
    enum ActorReason reason;
    unsigned int source_class;
    /* Read-only native animation diagnostics; never copied from the source. */
    unsigned int native_animation, native_frame, applied_torso, torso_present;
    float applied_velocity;
    unsigned int applied_moving;
} ActorResult;
typedef struct Actor {
    ActorEngine engine;
    uintptr_t game, player, entity;
    /* Private VID persists for the lifetime of its engine-owned entity.
       The engine never owns this descriptor through its VID registry. */
    unsigned char vid[0x490];
    unsigned int category, updates;
    int armed_weapon;
} Actor;

/* Production binding validates Steam code and vtable before publishing calls.
   Initialize the actor before installing the tick hook. */
int actor_engine_bind(uintptr_t base, ActorEngine* output);
void actor_init(Actor* actor, const ActorEngine* engine);
/* All lifecycle and engine operations run on the game thread, after its tick.
   Stop removes only a live entity still registered with this private VID. */
ActorResult actor_spawn(Actor*, uintptr_t base, const Snapshot* local, const Snapshot* target);
int actor_torso_ready(const Actor*);
int actor_set_army(Actor*, unsigned int army);
/* 1 success, 0 rejected (keep previous weapon), -1 native exception. */
int actor_arm(Actor*, int slot);
ActorResult actor_apply(Actor*, const Snapshot* target);
/* The native destructor cascades through the child chain, including name STEXT.
   Map owns MAN, child entities and their strings. VID remains in the pinned DLL.
   Removal reports ACTOR_FAULT and retains the pointer for truthful stop reporting. */
ActorResult actor_remove(Actor*, uintptr_t game);
int actor_live(const Actor* actor, uintptr_t game);
int actor_shoot(Actor* actor, uintptr_t game, int x, int y, int weapon);
#endif
