#ifndef ASMP_DUMMY_ACTOR_H
#define ASMP_DUMMY_ACTOR_H
#include "../../asmp-dll/src/game/steam/steam_actor.h"
typedef SteamActor DummyActor;
void dummy_actor_init(DummyActor*, const ActorEngine*);
ActorResult dummy_actor_tick(DummyActor*, uintptr_t, const Snapshot*, enum ProbeResult, DWORD, int);
#endif
