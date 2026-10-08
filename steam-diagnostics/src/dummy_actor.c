#include "dummy_actor.h"
void dummy_actor_init(DummyActor* actor, const ActorEngine* engine) { steam_actor_init(actor, engine); }
ActorResult dummy_actor_tick(DummyActor* actor, uintptr_t base, const Snapshot* local, enum ProbeResult state, DWORD now, int stop) {
    return steam_actor_tick(actor, base, local, state, local, now, stop, 0, 80.0f, 60000u);
}
