#ifndef ASMP_DUMMY_ACTOR_H
#define ASMP_DUMMY_ACTOR_H
#include <windows.h>
#include "probe.h"

/* Steam methods use ECX and callee-cleaned stack arguments. An unused EDX
   argument makes __fastcall equivalent to their __thiscall ABI in MSVC C. */
typedef void* (__fastcall* ActorCreate)(void*, void*, void*, float, float, float, int, void*);
typedef void* (__fastcall* ActorDestroy)(void*, void*, int);
typedef void (__fastcall* ActorMove)(void*, void*, float, float, float);
typedef unsigned char (__fastcall* ActorRotate)(void*, void*, unsigned int);
typedef struct ActorEngine {
    ActorCreate create;
    ActorDestroy destroy;
    ActorMove move;
    ActorRotate rotate;
} ActorEngine;

enum ActorEvent { ACTOR_NONE, ACTOR_SPAWNED, ACTOR_REMOVED, ACTOR_LOST,
    ACTOR_REJECTED, ACTOR_FAULT, ACTOR_POSE };
enum ActorReason { ACTOR_REASON_NONE, ACTOR_REASON_VID_CLASS, ACTOR_REASON_NO_CHILD,
    ACTOR_REASON_CATEGORY, ACTOR_REASON_FACTORY, ACTOR_REASON_REGISTRATION,
    ACTOR_REASON_ENTITY_TYPE, ACTOR_REASON_LOCAL_PLAYER, ACTOR_REASON_EXCEPTION };
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
typedef struct DummyActor {
    ActorEngine engine;
    uintptr_t game, player, entity;
    char map[128];
    /* Private VID persists for the lifetime of its engine-owned entity.
       The engine never owns this descriptor through its VID registry. */
    unsigned char vid[0x490];
    unsigned int category, updates;
    DWORD ready_since, spawned_at, last_pose;
    int attempted, waiting;
} DummyActor;

/* Production binding validates Steam code and vtable before publishing calls.
   Initialize the actor before installing the tick hook. */
int actor_engine_bind(uintptr_t base, ActorEngine* output);
void dummy_actor_init(DummyActor* actor, const ActorEngine* engine);
/* All lifecycle and engine operations run on the game thread, after its tick.
   Stop removes only a live entity still registered with this private VID. */
ActorResult dummy_actor_tick(DummyActor* actor, uintptr_t base,
    const Snapshot* local, enum ProbeResult state, DWORD now, int stop);
#endif
