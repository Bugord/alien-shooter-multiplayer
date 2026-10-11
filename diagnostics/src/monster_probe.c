#include <string.h>
#include <math.h>
#include "monster_probe.h"
#include "../../asmp-dll/src/game/ui.h"
#include "../../asmp-dll/src/game/world_hook.h"

/* Explicit, offline experiment. No sockets, no replica lifecycle and no
   game-thread logging. This controller feeds a scripted stand-in host pose. */
static struct
{
    enum MonsterProbeState state;
    UnitPuppet* puppet;  /* The probe always drives puppet slot 0. */
    UnitCombat* fixture; /* ...and combat fixture slot 0. */
    UnitDeath* death;    /* ...and death presentation slot 0. */
    DWORD entered, last_log;
    DWORD combat_world_ready;
    uintptr_t game;
    uint32_t low, high, epoch;
    UnitPuppetPose anchor, previous;
    unsigned int phase, violations;
    int ui, world, hooks, applied, animated;
    enum MonsterProbeKind kind;
    int combat_initialized, combat_spawned, damage_blocked, attack_seen, death_seen, player_health;
    int visual_initialized, visual_stage;
    DWORD visual_at;
    unsigned int corpse_seen, gibs_seen;
} probe;
static int combat_mode(void)
{
    return probe.kind == MONSTER_PROBE_COMBAT || probe.kind == MONSTER_PROBE_DEATH;
}
int monster_probe_enable_kind(uintptr_t base, enum MonsterProbeKind kind)
{
    if (probe.ui || probe.world || probe.hooks || probe.combat_initialized)
        return 0;
    memset(&probe, 0, sizeof(probe));
    probe.puppet = unit_puppet_slot(0);
    probe.fixture = unit_combat_slot(0);
    probe.death = unit_death_slot(0);
    UnitPuppetEngine api;
    if (kind != MONSTER_PROBE_GROUND && kind != MONSTER_PROBE_FLYING &&
        kind != MONSTER_PROBE_FLYING_RELOAD && kind != MONSTER_PROBE_COMBAT &&
        kind != MONSTER_PROBE_DEATH)
        return 0;
    if (!unit_puppet_bind(base, &api))
        return 0;
    probe.kind = kind;
    if (combat_mode())
    {
        ActorCreate create;
        if (!unit_combat_bind(base, &create) ||
            !unit_combat_init(base, create, api.destroy, api.animation))
            return 0;
        probe.combat_initialized = 1;
        if (kind == MONSTER_PROBE_DEATH)
        {
            if (!unit_death_init(base, create))
            {
                monster_probe_stop();
                return 0;
            }
            probe.visual_initialized = 1;
        }
    }
    probe.ui = ui_bind(base);
    if (!probe.ui)
    {
        monster_probe_stop();
        return 0;
    }
    probe.world = world_hook_install(base);
    if (!probe.world)
    {
        monster_probe_stop();
        return 0;
    }
    /* Even a failed install can retain a foreign/protection-failed hook. */
    probe.hooks = 1;
    if (!unit_puppet_install(base, &api))
    {
        monster_probe_stop();
        return 0;
    }
    probe.state = MONSTER_PROBE_MENU;
    return 1;
}
int monster_probe_enable(uintptr_t base)
{
    return monster_probe_enable_kind(base, MONSTER_PROBE_GROUND);
}
static void finish(MonsterProbeFrame* f, uintptr_t game, int failed)
{
    f->cleanup = unit_puppet_release(probe.puppet, game);
    if (probe.combat_initialized && !unit_combat_remove(probe.fixture, game))
        f->cleanup = 0;
    if (probe.visual_initialized && !unit_death_remove(probe.death, game))
        f->cleanup = 0;
    probe.state = failed || !f->cleanup ? MONSTER_PROBE_FAILED : MONSTER_PROBE_DONE;
    f->state = probe.state;
    f->event = 1;
}
static void visual_tick(MonsterProbeFrame* f, const Snapshot* local, DWORD now)
{
    f->visual = unit_death_sample(probe.death, local->game);
    f->phase = 6u + (unsigned int)probe.visual_stage;
    if (f->visual.corpses)
        probe.corpse_seen = 1;
    if (f->visual.gibs > probe.gibs_seen)
        probe.gibs_seen = f->visual.gibs;
    if (f->visual.fault || local->health != probe.player_health)
    {
        ++probe.violations;
        finish(f, local->game, 1);
        return;
    }
    if (probe.visual_stage == 1 && now - probe.visual_at >= 3000u)
    {
        if (!probe.corpse_seen || !unit_death_remove(probe.death, local->game) ||
            unit_death_play(probe.death, local->game, &probe.anchor, STEAM_ANIM_GIB_DEATH) != 1)
        {
            ++probe.violations;
            finish(f, local->game, 1);
            return;
        }
        probe.visual_stage = 2;
        probe.visual_at = now;
        f->event = 1;
    }
    else if (probe.visual_stage == 2 && now - probe.visual_at >= 15000u)
    {
        if (probe.gibs_seen < 8u)
            ++probe.violations;
        finish(f, local->game, probe.violations != 0);
    }
    f->violations = probe.violations;
    if (now - probe.last_log >= 100u)
    {
        f->event = 1;
        probe.last_log = now;
    }
}
static void combat_tick(MonsterProbeFrame* f, const Snapshot* local, DWORD now)
{
    DWORD elapsed = now - probe.entered;
    f->combat_event = 1;
    f->combat = unit_combat_sample(probe.fixture, local->game, 1);
    if (f->combat.fault || !f->combat.sources_unchanged || f->combat.unexpected_entities ||
        local->health != probe.player_health)
        ++probe.violations;
    if (f->unit.goal || f->unit.command || f->unit.stack || f->unit.behavior ||
        f->unit.child_goal || f->unit.child_command || f->unit.child_stack)
        ++probe.violations;
    unsigned int phase = elapsed < 3000u    ? MONSTER_COMBAT_IDLE
                         : elapsed < 5000u  ? MONSTER_COMBAT_DAMAGE
                         : elapsed < 8000u  ? MONSTER_COMBAT_ATTACK
                         : elapsed < 10000u ? MONSTER_COMBAT_RECOVER
                                            : MONSTER_COMBAT_DEATH;
    f->phase = phase;
    f->damage_blocked = probe.damage_blocked;
    if (phase < MONSTER_COMBAT_DEATH &&
        (f->unit.state != UNIT_PUPPET_ACTIVE || f->combat.private_entities != 2u))
    {
        ++probe.violations;
        f->violations = probe.violations;
        finish(f, local->game, 1);
        return;
    }
    if (phase <= MONSTER_COMBAT_DAMAGE &&
        (f->unit.animation != STEAM_ANIM_IDLE || f->unit.child_animation != STEAM_ANIM_IDLE ||
         f->unit.moving || f->unit.velocity != 0.0f || fabsf(f->unit.x - probe.anchor.x) > 0.01f ||
         fabsf(f->unit.y - probe.anchor.y) > 0.01f || fabsf(f->unit.z - probe.anchor.z) > 0.01f))
        ++probe.violations;
    if (phase == MONSTER_COMBAT_DAMAGE && !probe.damage_blocked)
    {
        probe.damage_blocked = unit_puppet_test_damage(probe.puppet, local->game) == 1;
        if (!probe.damage_blocked)
            ++probe.violations;
        f->event = 1;
    }
    if (phase == MONSTER_COMBAT_ATTACK && !probe.attack_seen)
    {
        if (unit_puppet_replay(probe.puppet, local->game, 1u, STEAM_ANIM_ATTACK) != 1 ||
            unit_puppet_replay(probe.puppet, local->game, 1u, STEAM_ANIM_ATTACK) != 0)
            ++probe.violations;
        f->unit = unit_puppet_sample(probe.puppet, local->game);
        probe.attack_seen =
            f->unit.animation == STEAM_ANIM_ATTACK || f->unit.child_animation == STEAM_ANIM_ATTACK;
        if (!probe.attack_seen)
            ++probe.violations;
        f->event = 1;
    }
    if (phase == MONSTER_COMBAT_RECOVER)
    {
        if (!probe.attack_seen || f->unit.animation != STEAM_ANIM_IDLE ||
            f->unit.child_animation != STEAM_ANIM_IDLE || !f->unit.attack_frame_changes ||
            f->unit.replayed != 1u || f->unit.duplicates != 1u)
            ++probe.violations;
    }
    if (phase == MONSTER_COMBAT_DEATH && !probe.death_seen)
    {
        if (!probe.damage_blocked || !probe.attack_seen || probe.phase != MONSTER_COMBAT_RECOVER ||
            unit_puppet_replay(probe.puppet, local->game, 2u, STEAM_ANIM_DEATH) != 1 ||
            unit_puppet_replay(probe.puppet, local->game, 2u, STEAM_ANIM_DEATH) != 0)
            ++probe.violations;
        f->unit = unit_puppet_sample(probe.puppet, local->game);
        probe.death_seen = f->unit.animation == STEAM_ANIM_DEATH;
        if (!probe.death_seen)
            ++probe.violations;
        f->event = 1;
    }
    else if (phase == MONSTER_COMBAT_DEATH && !f->combat.private_entities)
    {
        f->phase = MONSTER_COMBAT_DISPOSED;
        f->violations = probe.violations;
        f->damage_blocked = probe.damage_blocked;
        if (probe.kind == MONSTER_PROBE_DEATH && !probe.violations)
        {
            if (!unit_puppet_release(probe.puppet, local->game) ||
                !unit_combat_remove(probe.fixture, local->game) ||
                unit_death_play(probe.death, local->game, &probe.anchor, STEAM_ANIM_DEATH) != 1)
            {
                ++probe.violations;
                finish(f, local->game, 1);
                return;
            }
            probe.visual_stage = 1;
            probe.visual_at = now;
            f->event = 1;
            return;
        }
        finish(f, local->game,
               probe.violations || !probe.death_seen || f->unit.replayed != 2u ||
                   f->unit.duplicates != 2u);
        return;
    }
    if (elapsed > 20000u || probe.violations)
    {
        f->violations = probe.violations;
        finish(f, local->game, 1);
        return;
    }
    f->phase = phase;
    f->violations = probe.violations;
    f->damage_blocked = probe.damage_blocked;
    if (phase != probe.phase || now - probe.last_log >= 500u)
    {
        f->event = 1;
        probe.last_log = now;
    }
    probe.phase = phase;
    f->state = probe.state;
}
MonsterProbeFrame monster_probe_tick(const Snapshot* local, enum ProbeResult result, DWORD now,
                                     int stop)
{
    MonsterProbeFrame f = {0};
    f.state = probe.state;
    if (probe.state == MONSTER_PROBE_OFF || probe.state == MONSTER_PROBE_DONE ||
        probe.state == MONSTER_PROBE_FAILED)
        return f;
    if (stop)
    {
        f.unit = unit_puppet_sample(probe.puppet, local->game);
        finish(&f, local->game, 0);
        return f;
    }
    if (probe.state == MONSTER_PROBE_MENU)
    {
        if (!local->game)
            return f;
        if (!probe.entered)
            probe.entered = now;
        if (now - probe.entered < 2000u)
            return f;
        if (!ui_load_map(local->game, "maps\\survive_01.map"))
        {
            finish(&f, local->game, 1);
            return f;
        }
        probe.state = MONSTER_PROBE_SEARCH;
        probe.entered = now;
        f.state = probe.state;
        f.event = 1;
        return f;
    }
    if (probe.state == MONSTER_PROBE_SEARCH)
    {
        if (now - probe.entered > 45000u)
        {
            finish(&f, local->game, 1);
            return f;
        }
        if (result != PROBE_OK || !local->in_level || local->health <= 0)
            return f;
        if (combat_mode() && !probe.combat_spawned)
        {
            /* load_map can expose the player before the first full world tick
               removes temporary map objects and finishes player initialization. */
            if (!probe.combat_world_ready)
                probe.combat_world_ready = now;
            if (now - probe.combat_world_ready < 2000u)
                return f;
            int spawned = unit_combat_spawn(probe.fixture, local);
            if (spawned != 1)
            {
                finish(&f, local->game, 1);
                return f;
            }
            probe.combat_spawned = 1;
        }
        int attached = unit_puppet_attach_first(probe.puppet, local->game, local->player);
        if (attached < 0)
        {
            finish(&f, local->game, 1);
            return f;
        }
        if (!attached)
            return f;
        probe.game = local->game;
        probe.low = local->world_low;
        probe.high = local->world_high;
        probe.epoch = world_hook_generation();
        probe.anchor.x = local->x + 80;
        probe.anchor.y = local->y;
        probe.anchor.z = local->z;
        if (probe.kind == MONSTER_PROBE_FLYING || probe.kind == MONSTER_PROBE_FLYING_RELOAD)
        {
            UnitPuppetSample captured = unit_puppet_sample(probe.puppet, local->game);
            if (captured.vid != 290u)
            {
                finish(&f, local->game, 1);
                return f;
            }
            probe.anchor.z = captured.z;
        }
        probe.anchor.direction = local->direction;
        if (combat_mode())
        {
            UnitCombatSample combat = unit_combat_sample(probe.fixture, local->game, 1);
            UnitPuppetSample captured = unit_puppet_sample(probe.puppet, local->game);
            if (captured.entity != combat.root)
            {
                finish(&f, local->game, 1);
                return f;
            }
            probe.anchor.x = captured.x;
            probe.anchor.y = captured.y;
            probe.anchor.z = captured.z;
            probe.player_health = local->health;
        }
        probe.entered = now;
        probe.state = MONSTER_PROBE_RUNNING;
        f.event = 1;
    }
    f.unit = unit_puppet_sample(probe.puppet, local->game);
    if (probe.state != MONSTER_PROBE_RUNNING)
        return f;
    if (f.unit.frame_last > f.unit.frame_first ||
        f.unit.child_frame_last > f.unit.child_frame_first)
        probe.animated = 1;
    if (local->game != probe.game || !local->in_level || local->world_low != probe.low ||
        local->world_high != probe.high || world_hook_generation() != probe.epoch ||
        result != PROBE_OK || local->health <= 0 ||
        (!combat_mode() && f.unit.state != UNIT_PUPPET_ACTIVE))
    {
        finish(&f, local->game, 1);
        return f;
    }
    if (probe.visual_stage)
    {
        visual_tick(&f, local, now);
        return f;
    }
    if (combat_mode())
    {
        combat_tick(&f, local, now);
        return f;
    }
    /* Observe BEFORE applying the next host pose. Overwriting first would hide
       native drift, random steering or StopMove between snapshots. */
    if (probe.applied &&
        (f.unit.behavior || f.unit.goal || f.unit.command || f.unit.stack || f.unit.child_goal ||
         f.unit.child_command || f.unit.child_stack ||
         f.unit.animation != (probe.previous.moving && probe.previous.velocity != 0
                                  ? STEAM_ANIM_RUN
                                  : STEAM_ANIM_IDLE) ||
         fabsf(f.unit.x - probe.previous.x) > 0.01f || fabsf(f.unit.y - probe.previous.y) > 0.01f ||
         fabsf(f.unit.z - probe.previous.z) > 0.01f ||
         f.unit.direction != probe.previous.direction || f.unit.moving != probe.previous.moving ||
         fabsf(f.unit.velocity - probe.previous.velocity) > 0.00001f))
        ++probe.violations;
    DWORD elapsed = now - probe.entered;
    unsigned int phase = elapsed / 5000u;
    if (probe.kind == MONSTER_PROBE_FLYING_RELOAD && elapsed >= 10000u)
    {
        /* A separate live cleanup case. Reload once, then finish immediately:
           never apply the old pose after load_map invalidates the entity. */
        f.reloaded = ui_load_map(local->game, "maps\\survive_01.map") &&
                     world_hook_generation() != probe.epoch;
        f.phase = phase;
        f.violations = probe.violations;
        finish(&f, local->game, !f.reloaded || probe.violations);
        return f;
    }
    if (phase >= 6u)
    {
        /* VID290/291 have single-frame idle/run art. Require frame progression
           for animated art, while retaining the ground checkpoint's proof. */
        int animation_failed = !f.unit.frame_changes && !f.unit.child_frame_changes &&
                               (probe.kind == MONSTER_PROBE_GROUND || probe.animated);
        int failed = probe.violations || !f.unit.decisions || !f.unit.movements || animation_failed;
        f.violations = probe.violations;
        f.phase = 6u;
        finish(&f, local->game, failed);
        return f;
    }
    UnitPuppetPose p = probe.anchor;
    /* 0 idle, 1 east run, 2 stop, 3 west run, 4 stationary turn, 5 idle.
       Coordinates/intent are inputs; frame and timers are never supplied. */
    if (phase == 1u || phase == 2u)
        p.x += phase == 1u ? (float)(elapsed - 5000u) * 0.02f : 100.0f;
    if (phase == 3u)
        p.x += 100.0f - (float)(elapsed - 15000u) * 0.02f;
    if (phase == 1u || phase == 3u)
    {
        p.moving = 1;
        p.velocity = 0.02f;
        p.direction = phase == 1u ? 64u : 192u;
    }
    if (phase == 2u)
        p.direction = 64u;
    if (phase == 4u)
        p.direction = 128u;
    if (unit_puppet_apply(probe.puppet, local->game, &p) != 1)
    {
        finish(&f, local->game, 1);
        return f;
    }
    probe.previous = p;
    probe.applied = 1;
    if (phase != probe.phase || now - probe.last_log >= 1000u)
    {
        f.event = 1;
        probe.last_log = now;
    }
    probe.phase = f.phase = phase;
    f.violations = probe.violations;
    f.state = probe.state;
    return f;
}
int monster_probe_stop(void)
{
    if (probe.visual_initialized && !unit_death_stop())
        return 0;
    probe.visual_initialized = 0;
    if (probe.combat_initialized && !unit_combat_stop())
        return 0;
    probe.combat_initialized = 0;
    if (probe.hooks && !unit_puppet_stop())
        return 0;
    probe.hooks = 0;
    if (probe.world && !world_hook_stop())
        return 0;
    probe.world = 0;
    if (probe.ui)
        ui_stop();
    probe.ui = 0;
    probe.state = MONSTER_PROBE_OFF;
    return 1;
}
