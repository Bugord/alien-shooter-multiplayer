#ifndef ASMP_DUMMY_ACTOR_H
#define ASMP_DUMMY_ACTOR_H
#include "../../asmp-dll/src/game/steam/steam_actor.h"
enum DummyState { DUMMY_IDLE, DUMMY_WAITING, DUMMY_LIVE, DUMMY_DONE, DUMMY_ABANDONED };
typedef struct DummyActor {
    SteamActor native;
    enum DummyState state;
    uint32_t world_low, world_high;
    DWORD entered_at, last_pose;
} DummyActor;
void dummy_actor_init(DummyActor*, const ActorEngine*);
ActorResult dummy_actor_tick(DummyActor*, uintptr_t, const Snapshot*, enum ProbeResult, DWORD, int);
#endif
