#include <string.h>
#include <math.h>
#include "monster_probe.h"
#include "../../asmp-dll/src/game/ui.h"
#include "../../asmp-dll/src/game/world_hook.h"

/* Explicit, offline experiment. No sockets, no replica lifecycle and no
   game-thread logging. This controller feeds a scripted stand-in host pose. */
static struct {
    enum MonsterProbeState state;
    DWORD entered, last_log;
    uintptr_t game;
    uint32_t low, high, epoch;
    UnitPuppetPose anchor, previous;
    unsigned int phase, violations;
    int ui, world, hooks, applied, animated;
    enum MonsterProbeKind kind;
} probe;
int monster_probe_enable_kind(uintptr_t base, enum MonsterProbeKind kind) {
    if (probe.ui || probe.world || probe.hooks) return 0;
    memset(&probe, 0, sizeof(probe));
    UnitPuppetEngine api;
    if (kind != MONSTER_PROBE_GROUND && kind != MONSTER_PROBE_FLYING && kind != MONSTER_PROBE_FLYING_RELOAD) return 0;
    if (!unit_puppet_bind(base, &api)) return 0;
    probe.kind = kind;
    probe.ui = ui_bind(base);
    if (!probe.ui) return 0;
    probe.world = world_hook_install(base);
    if (!probe.world) { monster_probe_stop(); return 0; }
    /* Even a failed install can retain a foreign/protection-failed hook. */
    probe.hooks = 1;
    if (!unit_puppet_install(base, &api)) { monster_probe_stop(); return 0; }
    probe.state = MONSTER_PROBE_MENU; return 1;
}
int monster_probe_enable(uintptr_t base) { return monster_probe_enable_kind(base, MONSTER_PROBE_GROUND); }
static void finish(MonsterProbeFrame* f, uintptr_t game, int failed) {
    f->cleanup = unit_puppet_release(game);
    probe.state = failed || !f->cleanup ? MONSTER_PROBE_FAILED : MONSTER_PROBE_DONE;
    f->state = probe.state; f->event = 1;
}
MonsterProbeFrame monster_probe_tick(const Snapshot* local, enum ProbeResult result, DWORD now, int stop) {
    MonsterProbeFrame f = {0}; f.state = probe.state;
    if (probe.state == MONSTER_PROBE_OFF || probe.state == MONSTER_PROBE_DONE || probe.state == MONSTER_PROBE_FAILED) return f;
    if (stop) { f.unit = unit_puppet_sample(local->game); finish(&f, local->game, 0); return f; }
    if (probe.state == MONSTER_PROBE_MENU) {
        if (!local->game) return f;
        if (!probe.entered) probe.entered = now;
        if (now - probe.entered < 2000u) return f;
        if (!ui_load_map(local->game, "maps\\survive_01.map")) { finish(&f, local->game, 1); return f; }
        probe.state = MONSTER_PROBE_SEARCH; probe.entered = now; f.state = probe.state; f.event = 1; return f;
    }
    if (probe.state == MONSTER_PROBE_SEARCH) {
        if (now - probe.entered > 45000u) { finish(&f, local->game, 1); return f; }
        if (result != PROBE_OK || !local->in_level || local->health <= 0) return f;
        int attached = unit_puppet_attach_first(local->game, local->player);
        if (attached < 0) { finish(&f, local->game, 1); return f; }
        if (!attached) return f;
        probe.game = local->game; probe.low = local->world_low; probe.high = local->world_high;
        probe.epoch = world_hook_generation();
        probe.anchor.x = local->x + 80; probe.anchor.y = local->y; probe.anchor.z = local->z;
        if (probe.kind != MONSTER_PROBE_GROUND) {
            UnitPuppetSample captured = unit_puppet_sample(local->game);
            if (captured.vid != 290u) { finish(&f, local->game, 1); return f; }
            probe.anchor.z = captured.z;
        }
        probe.anchor.direction = local->direction;
        probe.entered = now; probe.state = MONSTER_PROBE_RUNNING; f.event = 1;
    }
    f.unit = unit_puppet_sample(local->game);
    if (probe.state != MONSTER_PROBE_RUNNING) return f;
    if (f.unit.frame_last > f.unit.frame_first || f.unit.child_frame_last > f.unit.child_frame_first) probe.animated = 1;
    if (local->game != probe.game || !local->in_level || local->world_low != probe.low ||
        local->world_high != probe.high || world_hook_generation() != probe.epoch || result != PROBE_OK || local->health <= 0 ||
        f.unit.state != UNIT_PUPPET_ACTIVE) { finish(&f, local->game, 1); return f; }
    /* Observe BEFORE applying the next host pose. Overwriting first would hide
       native drift, random steering or StopMove between snapshots. */
    if (probe.applied && (f.unit.behavior || f.unit.goal || f.unit.command || f.unit.stack ||
        f.unit.child_goal || f.unit.child_command || f.unit.child_stack ||
        f.unit.animation != (probe.previous.moving && probe.previous.velocity != 0 ? STEAM_ANIM_RUN : STEAM_ANIM_IDLE) ||
        fabsf(f.unit.x - probe.previous.x) > 0.01f || fabsf(f.unit.y - probe.previous.y) > 0.01f ||
        fabsf(f.unit.z - probe.previous.z) > 0.01f || f.unit.direction != probe.previous.direction ||
        f.unit.moving != probe.previous.moving || fabsf(f.unit.velocity - probe.previous.velocity) > 0.00001f)) ++probe.violations;
    DWORD elapsed = now - probe.entered;
    unsigned int phase = elapsed / 5000u;
    if (probe.kind == MONSTER_PROBE_FLYING_RELOAD && elapsed >= 10000u) {
        /* A separate live cleanup case. Reload once, then finish immediately:
           never apply the old pose after load_map invalidates the entity. */
        f.reloaded = ui_load_map(local->game, "maps\\survive_01.map") && world_hook_generation() != probe.epoch;
        f.phase = phase; f.violations = probe.violations;
        finish(&f, local->game, !f.reloaded || probe.violations); return f;
    }
    if (phase >= 6u) {
        /* VID290/291 have single-frame idle/run art. Require frame progression
           for animated art, while retaining the ground checkpoint's proof. */
        int animation_failed = !f.unit.frame_changes && !f.unit.child_frame_changes &&
            (probe.kind == MONSTER_PROBE_GROUND || probe.animated);
        int failed = probe.violations || !f.unit.decisions || !f.unit.movements || animation_failed;
        f.violations = probe.violations; f.phase = 6u; finish(&f, local->game, failed); return f;
    }
    UnitPuppetPose p = probe.anchor;
    /* 0 idle, 1 east run, 2 stop, 3 west run, 4 stationary turn, 5 idle.
       Coordinates/intent are inputs; frame and timers are never supplied. */
    if (phase == 1u || phase == 2u) p.x += phase == 1u ? (float)(elapsed - 5000u) * 0.02f : 100.0f;
    if (phase == 3u) p.x += 100.0f - (float)(elapsed - 15000u) * 0.02f;
    if (phase == 1u || phase == 3u) { p.moving = 1; p.velocity = 0.02f; p.direction = phase == 1u ? 64u : 192u; }
    if (phase == 2u) p.direction = 64u;
    if (phase == 4u) p.direction = 128u;
    if (unit_puppet_apply(local->game, &p) != 1) { finish(&f, local->game, 1); return f; }
    probe.previous = p; probe.applied = 1;
    if (phase != probe.phase || now - probe.last_log >= 1000u) { f.event = 1; probe.last_log = now; }
    probe.phase = f.phase = phase; f.violations = probe.violations; f.state = probe.state; return f;
}
int monster_probe_stop(void) {
    if (probe.hooks && !unit_puppet_stop()) return 0;
    probe.hooks = 0;
    if (probe.world && !world_hook_stop()) return 0;
    probe.world = 0;
    if (probe.ui) ui_stop();
    probe.ui = 0; probe.state = MONSTER_PROBE_OFF; return 1;
}
