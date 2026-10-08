#include "dummy_actor.h"
#include <string.h>
void dummy_actor_init(DummyActor* a, const ActorEngine* engine) {
    memset(a, 0, sizeof(*a)); steam_actor_init(&a->native, engine);
}
ActorResult dummy_actor_tick(DummyActor* a, uintptr_t base, const Snapshot* local, enum ProbeResult state, DWORD now, int stop) {
    ActorResult result = {0};
    int gameplay = state == PROBE_OK && local->health > 0 && (local->world_low || local->world_high);
    int changed = a->world_low != local->world_low || a->world_high != local->world_high;
    if (a->state == DUMMY_ABANDONED) return result;
    if (a->native.entity && (!gameplay || stop || changed || now - a->entered_at >= 60000u)) {
        result = steam_actor_remove(&a->native, local->game);
        a->state = result.event == ACTOR_FAULT ? DUMMY_ABANDONED : DUMMY_DONE; return result;
    }
    if (a->native.entity && !steam_actor_live(&a->native, local->game)) {
        result.event = ACTOR_LOST; result.entity = a->native.entity; a->native.entity = 0; a->state = DUMMY_DONE; return result;
    }
    if (!gameplay || stop) { if (!gameplay) a->state = DUMMY_IDLE; return result; }
    if (changed) a->state = DUMMY_IDLE;
    if (a->state == DUMMY_IDLE) {
        a->world_low = local->world_low; a->world_high = local->world_high;
        a->entered_at = now; a->state = DUMMY_WAITING;
    }
    Snapshot target = *local; target.x += 80.0f;
    if (a->state == DUMMY_WAITING && now - a->entered_at >= 2000u) {
        result = steam_actor_spawn(&a->native, base, local, &target);
        a->state = result.event == ACTOR_SPAWNED ? DUMMY_LIVE : DUMMY_DONE;
        a->entered_at = a->last_pose = now;
    } else if (a->state == DUMMY_LIVE) {
        result = steam_actor_apply(&a->native, &target);
        if (result.event == ACTOR_POSE && now - a->last_pose < 1000u) result.event = ACTOR_NONE;
        else a->last_pose = now;
    }
    if (result.event == ACTOR_FAULT) {
        steam_actor_remove(&a->native, local->game); a->state = DUMMY_ABANDONED;
    }
    return result;
}
