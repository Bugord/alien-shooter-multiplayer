#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <share.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "profile.h"
#include "probe.h"
#include "diag_tick.h"
#include "../../asmp-dll/src/game/display_hook.h"
#include "../../asmp-dll/src/multiplayer/runtime.h"
#include "../../asmp-dll/src/game/clock.h"

static HMODULE self;
static HANDLE worker;
static volatile LONG status = DIAG_STARTING;
static DWORD monster_kill_delay, monster_test_started;
static uint32_t monster_test_epoch;
static int monster_test_triggered;
static unsigned int monster_test_death = STEAM_ANIM_DEATH;
static int monster_forms_test;
static unsigned int monster_forms_phase;
static volatile LONG monster_forms_results[2];
static int monster_prediction_test;
static unsigned int monster_prediction_phase;
static volatile LONG monster_prediction_result;
static void observe_runtime(const RuntimeFrame* frame)
{
    diag_tick_observe(frame);
    if (monster_prediction_test && frame->monster.role == MONSTER_CLIENT &&
        frame->monster.gallery_active)
    {
        if (monster_test_epoch != frame->monster.host_epoch)
        {
            monster_test_epoch = frame->monster.host_epoch;
            monster_test_started = frame->milliseconds;
            monster_prediction_phase = 0;
        }
        if (!monster_prediction_phase && frame->milliseconds - monster_test_started >= 3000u)
        {
            int ok = 1;
            /* Deliberately unconfirmed native hits: one ice, one gib, two armor
               successors and one nonlethal hit must all roll back to the host.
               This drives native bindings, not weapon/projectile collision. */
            uintptr_t game = frame->snapshot.game;
            int ice = unit_puppet_prediction_test_hit(unit_puppet_slot(0u), game, 100000, 1);
            ok = ice == 1 && ok;
            int gib = unit_puppet_prediction_test_hit(unit_puppet_slot(8u), game, 100000, 0);
            ok = gib == 1 && ok;
            int medium_armor = unit_puppet_prediction_test_hit(
                unit_puppet_slot(32u), game, frame->monster.gallery[32].health, 0);
            ok = medium_armor == 1 && ok;
            int large_armor = unit_puppet_prediction_test_hit(unit_puppet_slot(40u), game,
                                                              frame->monster.gallery[40].health, 0);
            ok = large_armor == 1 && ok;
            int nonlethal = unit_puppet_prediction_test_hit(unit_puppet_slot(16u), game, 1, 0);
            ok = nonlethal == 1 && ok;
            monster_prediction_phase = 1u;
            InterlockedExchange(&monster_prediction_result, ok ? 1 : -1);
        }
        return;
    }
    if (monster_forms_test && frame->monster.role == MONSTER_HOST && frame->monster.gallery_count)
    {
        if (monster_test_epoch != frame->monster.host_epoch)
        {
            monster_test_epoch = frame->monster.host_epoch;
            monster_test_started = frame->milliseconds;
            monster_forms_phase = 0;
        }
        if (frame->monster.gallery_active && !monster_forms_phase &&
            frame->milliseconds - monster_test_started >= 5000u)
        {
            const unsigned int frozen[] = {0u, 8u, 16u, 24u, 33u, 41u};
            int ok = 1;
            uintptr_t game = frame->snapshot.game;
            for (unsigned int i = 0; i < sizeof(frozen) / sizeof(frozen[0]); ++i)
            {
                ok = unit_puppet_host_freeze(unit_puppet_slot(frozen[i]), game) == 1 && ok;
            }
            const unsigned int armored[] = {32u, 40u};
            for (unsigned int i = 0; i < sizeof(armored) / sizeof(armored[0]); ++i)
            {
                UnitPuppet* puppet = unit_puppet_slot(armored[i]);
                ok = unit_puppet_host_kill_variant(puppet, game, STEAM_ANIM_DEATH) == 1 && ok;
            }
            monster_forms_phase = 1u;
            InterlockedExchange(&monster_forms_results[0], ok ? 1 : -1);
        }
        if (monster_forms_phase == 1u && frame->milliseconds - monster_test_started >= 10000u)
        {
            const unsigned int killed[] = {0u, 8u, 16u, 24u, 32u, 33u, 40u, 41u};
            int ok = 1;
            uintptr_t game = frame->snapshot.game;
            for (unsigned int i = 0; i < sizeof(killed) / sizeof(killed[0]); ++i)
            {
                UnitPuppet* puppet = unit_puppet_slot(killed[i]);
                ok = unit_puppet_host_kill_variant(puppet, game, STEAM_ANIM_DEATH) == 1 && ok;
            }
            monster_forms_phase = 2u;
            InterlockedExchange(&monster_forms_results[1], ok ? 1 : -1);
        }
        return;
    }
    if (!monster_kill_delay || frame->monster.role != MONSTER_HOST || !frame->monster.unit.entity)
        return;
    if (monster_test_epoch != frame->monster.host_epoch)
    {
        monster_test_epoch = frame->monster.host_epoch;
        monster_test_started = frame->milliseconds;
        monster_test_triggered = 0;
    }
    if (!monster_test_triggered && frame->milliseconds - monster_test_started >= monster_kill_delay)
    {
        monster_test_triggered = 1;
        unsigned int count = frame->monster.gallery_count ? frame->monster.gallery_count : 1u;
        for (unsigned int slot = 0; slot < count; ++slot)
        {
            UnitPuppet* puppet = unit_puppet_slot(slot);
            unit_puppet_host_kill_variant(puppet, frame->snapshot.game, monster_test_death);
        }
    }
}
#pragma comment(linker, "/EXPORT:AsmpDiagGetStatus=_AsmpDiagGetStatus@0")
#pragma comment(linker, "/EXPORT:AsmpDiagWait=_AsmpDiagWait@4")
DWORD WINAPI AsmpDiagGetStatus(void)
{
    return (DWORD)InterlockedCompareExchange(&status, 0, 0);
}
DWORD WINAPI AsmpDiagWait(DWORD timeout)
{
    return WaitForSingleObject(worker, timeout);
}

static void log_frame(FILE* log, const FrameSample* frame, Snapshot* last,
                      enum ProbeResult* previous, DWORD* last_log)
{
    const Snapshot* sample = &frame->snapshot;
    if (frame->monster_network.event)
    {
        const MonsterSyncFrame* m = &frame->monster_network;
        if (m->gallery_count)
        {
            fprintf(log,
                    "# MONSTER_GALLERY role=%d total=%u active=%u dead=%u faults=%u deaths=%u "
                    "cleanup=%u\n",
                    m->role, m->gallery_count, m->gallery_active, m->gallery_dead,
                    m->gallery_faults, m->deaths, m->cleanup);
            for (unsigned int i = 0; i < m->gallery_count; ++i)
            {
                const MonsterGallerySample* row = &m->gallery[i];
                fprintf(log,
                        "# MONSTER_GALLERY_UNIT id=%u vid=%u state=%d entity=%08lX health=%d "
                        "anim=%u behavior=%u death=%u x=%.3f y=%.3f effects=%u fault=%d "
                        "revision=%u phase=%u presentation=%u prediction=%u\n",
                        row->id, row->vid, row->state, (unsigned long)row->entity, row->health,
                        row->animation, row->behavior, row->death_animation, row->x, row->y,
                        row->effects, row->fault, row->form_revision, row->form_phase,
                        row->presentation_vid, row->prediction);
            }
        }
        fprintf(log,
                "# MONSTER_NETWORK role=%d state=%d id=%u epoch=%u entity=%08lX x=%.3f y=%.3f "
                "z=%.3f direction=%u moving=%u health=%d anim=%u native_frames=%u behavior=%u "
                "attacks=%u deaths=%u dropped=%u rejected=%u cleanup=%u\n",
                m->role, m->state, m->monster_id, m->host_epoch, (unsigned long)m->unit.entity,
                m->unit.x, m->unit.y, m->unit.z, m->unit.direction, m->unit.moving, m->unit.health,
                m->unit.animation, m->unit.frame_changes, m->unit.behavior, m->attacks, m->deaths,
                m->dropped, m->rejected, m->cleanup);
        fprintf(log,
                "# MONSTER_DEATH animation=%u x=%.3f y=%.3f z=%.3f direction=%u effects=%u "
                "corpses=%u gibs=%u fault=%d\n",
                m->death_animation, m->death_pose.x, m->death_pose.y, m->death_pose.z,
                m->death_pose.direction, m->visual.entities, m->visual.corpses, m->visual.gibs,
                m->visual.fault);
        fprintf(log, "# MONSTER_FORM vid=%u revision=%u phase=%u transitions=%u\n", m->vid,
                m->form_revision, m->form_phase, m->forms);
        fprintf(log, "# MONSTER_PREDICTION started=%u confirmed=%u corrected=%u\n", m->predictions,
                m->confirmations, m->corrections);
        if (monster_prediction_test)
            fprintf(log, "# MONSTER_PREDICTION_TEST hits=%ld\n",
                    InterlockedCompareExchange(&monster_prediction_result, 0, 0));
        if (monster_forms_test && m->role == MONSTER_HOST)
            fprintf(log, "# MONSTER_FORM_TEST transitions=%ld final_kills=%ld\n",
                    InterlockedCompareExchange(&monster_forms_results[0], 0, 0),
                    InterlockedCompareExchange(&monster_forms_results[1], 0, 0));
    }
    if (frame->monster.event)
    {
        const MonsterProbeFrame* m = &frame->monster;
        fprintf(log, "# MONSTER_VISUAL phase=%u effects=%u corpses=%u gibs=%u fault=%d\n", m->phase,
                m->visual.entities, m->visual.corpses, m->visual.gibs, m->visual.fault);
        const UnitPuppetSample* u = &m->unit;
        fprintf(
            log,
            "# MONSTER state=%d phase=%u cleanup=%d entity=%08lX vid=%u class=%u anim=%u frame=%u "
            "frame_changes=%u decisions=%u movements=%u blocked=%u violations=%u behavior=%u "
            "goal=%08lX command=%u stack=%u moving=%u direction=%u velocity=%.5f x=%.2f y=%.2f "
            "z=%.2f child=%08lX child_goal=%08lX child_command=%u child_stack=%u\n",
            m->state, m->phase, m->cleanup, (unsigned long)u->entity, u->vid, u->class_id,
            u->animation, u->frame, u->frame_changes, u->decisions, u->movements, u->blocked,
            m->violations, u->behavior, (unsigned long)u->goal, u->command, u->stack, u->moving,
            u->direction, u->velocity, u->x, u->y, u->z, (unsigned long)u->child,
            (unsigned long)u->child_goal, u->child_command, u->child_stack);
        fprintf(log,
                "# MONSTER_FRAMES first=%u last=%u child_first=%u child_frame=%u child_last=%u "
                "child_frame_changes=%u\n",
                u->frame_first, u->frame_last, u->child_frame_first, u->child_frame,
                u->child_frame_last, u->child_frame_changes);
        if (m->reloaded)
            fprintf(log, "# MONSTER_RELOAD completed cleanup=%d violations=%u\n", m->cleanup,
                    m->violations);
        if (m->combat_event)
            fprintf(log,
                    "# MONSTER_COMBAT phase=%u damage_blocked=%d replayed=%u duplicates=%u "
                    "attack_frames=%u health=%d child_anim=%u private_entities=%u "
                    "other_entities=%u unexpected_entities=%u sources_unchanged=%d fault=%d\n",
                    m->phase, m->damage_blocked, u->replayed, u->duplicates,
                    u->attack_frame_changes, u->health, u->child_animation,
                    m->combat.private_entities, m->combat.other_entities,
                    m->combat.unexpected_entities, m->combat.sources_unchanged, m->combat.fault);
    }
    if (frame->actor.event != ACTOR_NONE)
    {
        static const char* events[] = {"none",     "spawned", "removed", "lost",
                                       "rejected", "fault",   "pose"};
        static const char* reasons[] = {
            "none",         "vid-class",    "no-child",    "list-category",
            "factory-null", "unregistered", "entity-type", "local-player-changed",
            "exception",    "weapon",       "precondition"};
        fprintf(log,
                "# DUMMY event=%s entity=%08lX category=%u updates=%u tick=%ld reason=%s "
                "source_class=%u\n",
                events[frame->actor.event], (unsigned long)frame->actor.entity,
                frame->actor.category, frame->actor.updates, frame->tick,
                reasons[frame->actor.reason], frame->actor.source_class);
        if (frame->actor.event == ACTOR_POSE)
            fprintf(log,
                    "# DUMMY_POSE velocity=%.3f moving=%u native_animation=%u native_frame=%u "
                    "torso=%u applied_torso=%u torso_present=%u\n",
                    frame->actor.applied_velocity, frame->actor.applied_moving,
                    frame->actor.native_animation, frame->actor.native_frame,
                    sample->torso_direction, frame->actor.applied_torso,
                    frame->actor.torso_present);
    }
    for (unsigned int i = 0; i < frame->multiplayer.count; ++i)
    {
        const ActorResult* a = &frame->multiplayer.remote[i].actor;
        fprintf(log,
                "# REMOTE_ACTOR id=%u event=%d reason=%d entity=%08lX updates=%u animation=%u "
                "frame=%u torso=%u state=%d cleanup_failed=%d\n",
                frame->multiplayer.remote[i].id, a->event, a->reason, (unsigned long)a->entity,
                a->updates, a->native_animation, a->native_frame, a->applied_torso,
                frame->multiplayer.remote[i].state, frame->multiplayer.remote[i].cleanup_failed);
    }
    if (frame->multiplayer.shots_applied || frame->multiplayer.shots_discarded)
        fprintf(log, "# SHOTS applied=%u discarded=%u\n", frame->multiplayer.shots_applied,
                frame->multiplayer.shots_discarded);
    if (frame->session_result)
        fprintf(log, "# SESSION map_load_result=%d\n", frame->session_result);
    if (frame->result == *previous &&
        (frame->result != PROBE_OK || !memcmp(last, sample, sizeof(*sample))) &&
        frame->milliseconds - *last_log < 5000)
        return;
    fprintf(log, "%lu,%s,%08lX,%08lX,%u,%.3f,%.3f,%.3f,%d,%u,%u,%d,%d,%d,%d", frame->milliseconds,
            probe_result_name(frame->result), (unsigned long)sample->game,
            (unsigned long)sample->player, sample->army_index, sample->x, sample->y, sample->z,
            sample->health, sample->animation, sample->direction, sample->weapon_slot,
            sample->weapon_vid, sample->current_ammo, sample->current_ammo_raw);
    for (unsigned int i = 0; i < STEAM_STORED_AMMO_COUNT; ++i)
        fprintf(log, ",%u", sample->stored_ammo[i]);
    fprintf(log, ",%ld,%.3f,%u,%u,%u\n", frame->tick, sample->velocity, sample->moving,
            sample->torso_present, sample->torso_direction);
    *last_log = frame->milliseconds;
    *last = *sample;
    *previous = frame->result;
    InterlockedExchange(&status, frame->result == PROBE_OK ? DIAG_SAMPLING : DIAG_WAITING);
}

static void log_stats(FILE* log)
{
    TickStats stats = diag_tick_stats();
    fprintf(log, "# TICK_STATS calls=%ld captured=%ld dropped=%ld installed=%ld\n", stats.calls,
            stats.captured, stats.dropped, stats.installed);
    fprintf(log, "# DISPLAY_STATS installed=%d frames=%ld\n", display_hook_ready(),
            display_hook_frames());
}

static DWORD WINAPI run(LPVOID unused)
{
    wchar_t exe[MAX_PATH], directory[MAX_PATH], log_path[MAX_PATH], stop_path[MAX_PATH];
    char digest[65];
    FILE* log = NULL;
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    enum ProbeResult previous = (enum ProbeResult) - 1;
    Snapshot last = {0};
    DWORD last_log = 0;
    DWORD last_stats = clock_ms();
    int hook_attempted = 0;
    int runtime_attempted = 0;
    int monster_attempted = 0;
    (void)unused;
    if (!GetModuleFileNameW(self, directory, MAX_PATH) || !GetModuleFileNameW(NULL, exe, MAX_PATH))
        goto fail;
    wchar_t* slash = wcsrchr(directory, L'\\');
    if (!slash)
        goto fail;
    *slash = 0;
    if (swprintf_s(log_path, MAX_PATH, L"%s\\logs", directory) < 0)
        goto fail;
    if (!CreateDirectoryW(log_path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        goto fail;
    if (swprintf_s(log_path, MAX_PATH, L"%s\\logs\\asmp-diag-%lu.log", directory,
                   GetCurrentProcessId()) < 0)
        goto fail;
    if (swprintf_s(stop_path, MAX_PATH, L"%s\\asmp-diag.stop", directory) < 0)
        goto fail;
    log = _wfsopen(log_path, L"w", _SH_DENYWR);
    if (!log)
        goto fail;
    setvbuf(log, NULL, _IONBF, 0);
    fprintf(log, "ASMP_DIAG loaded pid=%lu image=%08lX mode=tick-hook\n", GetCurrentProcessId(),
            (unsigned long)base);
    if (!hash_file_sha256(exe, digest))
    {
        fprintf(log, "ERROR executable hash could not be read\n");
        goto fail;
    }
    fprintf(log, "exe_sha256=%s\n", digest);
    if (strcmp(digest, STEAM_EXE_SHA256))
    {
        fprintf(log, "REJECTED unsupported executable; no game memory accessed\n");
        InterlockedExchange(&status, DIAG_REJECTED);
        fclose(log);
        return 0;
    }
    /* Also validate the mapped function before any gameplay memory reads. */
    static const unsigned char accessor[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x83, 0xE0,
                                             0x03, 0x8B, 0x84, 0x81, 0x44, 0x02, 0x00, 0x00,
                                             0x8B, 0x40, 0x10, 0x5D, 0xC2, 0x04, 0x00};
    __try
    {
        if (memcmp((void*)(base + STEAM_PLAYER_ACCESSOR_RVA), accessor, sizeof(accessor)))
        {
            fprintf(log, "REJECTED mapped-image signature mismatch\n");
            InterlockedExchange(&status, DIAG_REJECTED);
            fclose(log);
            return 0;
        }
        static const unsigned char tick_prefix[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
        if (memcmp((void*)(base + STEAM_GAME_TICK_RVA), tick_prefix, sizeof(tick_prefix)) ||
            *(uintptr_t*)(base + STEAM_GAME_VTABLE_RVA +
                          STEAM_GAME_TICK_SLOT * sizeof(uintptr_t)) != base + STEAM_GAME_TICK_RVA)
        {
            fprintf(log, "REJECTED tick signature or vtable mismatch; no hook installed\n");
            InterlockedExchange(&status, DIAG_REJECTED);
            fclose(log);
            return 0;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        goto fail;
    }
    fprintf(log, "PROFILE accepted Steam 33100; gameplay reads after original tick\n");
    fprintf(log, "milliseconds,state,game,player,army,x,y,z,health,animation,direction,weapon_slot,"
                 "weapon_vid,current_ammo,current_ammo_raw");
    for (unsigned int i = 0; i < STEAM_STORED_AMMO_COUNT; ++i)
        fprintf(log, ",stored_ammo_slot_%u", i + STEAM_STORED_AMMO_FIRST_SLOT);
    fprintf(log, ",tick,velocity,moving,torso_present,torso_direction\n");
    char dummy_option[8] = {0}, multiplayer_option[8] = {0};
    DWORD dummy_length =
        GetEnvironmentVariableA("ASMP_DIAG_DUMMY", dummy_option, sizeof(dummy_option));
    int dummy_enabled = dummy_length == 1 && dummy_option[0] == '1';
    DWORD multiplayer_length = GetEnvironmentVariableA("ASMP_DIAG_MULTIPLAYER", multiplayer_option,
                                                       sizeof(multiplayer_option));
    char host[16] = {0}, port_text[16] = {0}, name[16] = {0};
    DWORD host_length = GetEnvironmentVariableA("ASMP_DIAG_SERVER", host, sizeof(host));
    int multiplayer_enabled =
        host_length || (multiplayer_length == 1 && multiplayer_option[0] == '1');
    char monster_option[8] = {0};
    DWORD monster_length =
        GetEnvironmentVariableA("ASMP_DIAG_MONSTER", monster_option, sizeof(monster_option));
    int monster_enabled =
        monster_length == 1 && monster_option[0] >= '1' && monster_option[0] <= '5';
    enum MonsterProbeKind monster_kind =
        monster_option[0] == '5'   ? MONSTER_PROBE_DEATH
        : monster_option[0] == '4' ? MONSTER_PROBE_COMBAT
        : monster_option[0] == '3'
            ? MONSTER_PROBE_FLYING_RELOAD
            : (monster_option[0] == '2' ? MONSTER_PROBE_FLYING : MONSTER_PROBE_GROUND);
    if (monster_enabled && (dummy_enabled || multiplayer_enabled))
    {
        fprintf(log, "ERROR monster experiment is offline and exclusive\n");
        goto fail;
    }
    if (dummy_enabled && multiplayer_enabled)
    {
        fprintf(log, "ERROR dummy and multiplayer are mutually exclusive\n");
        goto fail;
    }
    if (multiplayer_enabled)
    {
        RuntimeConfig config = {0};
        config.log = log;
        config.observer = observe_runtime;
        char role[16] = {0};
        DWORD role_length =
            GetEnvironmentVariableA("ASMP_DIAG_MONSTER_NETWORK", role, sizeof(role));
        if (role_length)
        {
            if (!host_length || role_length >= sizeof(role) ||
                (strcmp(role, "host") && strcmp(role, "client")))
            {
                fprintf(log, "ERROR invalid monster network role or no direct connection\n");
                goto fail;
            }
            config.monster_role = !strcmp(role, "host") ? MONSTER_HOST : MONSTER_CLIENT;
            char gallery_option[4] = {0};
            config.monster_gallery =
                GetEnvironmentVariableA("ASMP_DIAG_MONSTER_GALLERY", gallery_option,
                                        sizeof(gallery_option)) == 1u &&
                gallery_option[0] == '1';
            char forms_option[4] = {0};
            monster_forms_test = config.monster_role == MONSTER_HOST && config.monster_gallery &&
                                 GetEnvironmentVariableA("ASMP_DIAG_MONSTER_FORMS", forms_option,
                                                         sizeof(forms_option)) == 1u &&
                                 forms_option[0] == '1';
            char prediction_option[4] = {0};
            monster_prediction_test =
                config.monster_role == MONSTER_CLIENT && config.monster_gallery &&
                GetEnvironmentVariableA("ASMP_DIAG_MONSTER_PREDICTION_TEST", prediction_option,
                                        sizeof(prediction_option)) == 1u &&
                prediction_option[0] == '1';
            char delay[16] = {0};
            DWORD length =
                GetEnvironmentVariableA("ASMP_DIAG_MONSTER_KILL_AFTER", delay, sizeof(delay));
            char variant[16] = {0};
            GetEnvironmentVariableA("ASMP_DIAG_MONSTER_DEATH_VARIANT", variant, sizeof(variant));
            if (!strcmp(variant, "gibs"))
                monster_test_death = STEAM_ANIM_GIB_DEATH;
            if (length)
            {
                char* end;
                unsigned long seconds = strtoul(delay, &end, 10);
                if (length >= sizeof(delay) || *end || seconds < 5u || seconds > 120u)
                {
                    fprintf(log, "ERROR invalid monster kill delay\n");
                    goto fail;
                }
                if (config.monster_role == MONSTER_HOST)
                    monster_kill_delay = seconds * 1000u;
            }
        }
        diag_tick_prepare_observer();
        if (host_length)
        {
            DWORD port_length =
                GetEnvironmentVariableA("ASMP_DIAG_PORT", port_text, sizeof(port_text));
            DWORD name_length = GetEnvironmentVariableA("ASMP_DIAG_NAME", name, sizeof(name));
            char* end = NULL;
            unsigned long port =
                port_length && port_length < sizeof(port_text) ? strtoul(port_text, &end, 10) : 0;
            if (!name_length)
                strcpy_s(name, sizeof(name), "SteamTester");
            if (host_length >= sizeof(host) || name_length >= sizeof(name) || !port ||
                port > 65535 || !end || *end)
            {
                fprintf(log, "ERROR invalid direct connection configuration\n");
                goto fail;
            }
            config.direct = 1;
            config.port = (unsigned short)port;
            strcpy_s(config.host, sizeof(config.host), host);
            strcpy_s(config.name, sizeof(config.name), name);
        }
        runtime_attempted = 1;
        if (!runtime_start(base, &config))
        {
            fprintf(log, "ERROR runtime startup failed\n");
            goto fail;
        }
        fprintf(log, "# MULTIPLAYER enabled native_map menu names health_bars shots\n");
    }
    if (dummy_enabled)
    {
        if (!diag_tick_enable_dummy(base))
        {
            fprintf(log, "REJECTED dummy actor engine signature mismatch\n");
            goto fail;
        }
        fprintf(log, "# DUMMY enabled offset_x=80 lifetime_seconds=60\n");
    }
    enum DiagTickResult hook_result = DIAG_TICK_OK;
    if (monster_enabled)
    {
        monster_attempted = 1;
        if (!diag_tick_enable_monster_kind(base, monster_kind))
        {
            fprintf(log, "REJECTED monster bindings/hooks\n");
            goto fail;
        }
        fprintf(log, "# MONSTER enabled stage=%u auto_map=%s duration=%s\n",
                monster_kind == MONSTER_PROBE_DEATH    ? 4u
                : monster_kind == MONSTER_PROBE_COMBAT ? 3u
                : monster_kind != MONSTER_PROBE_GROUND ? 2u
                                                       : 1u,
                monster_kind == MONSTER_PROBE_DEATH
                    ? "maps/survive_01.map waves=off corpse_and_gibs"
                : monster_kind == MONSTER_PROBE_COMBAT
                    ? "maps/survive_01.map waves=off private_UNIT_vid50 combat_visual_only"
                : monster_kind != MONSTER_PROBE_GROUND
                    ? "maps/survive_01.map waves=off flying_UNIT_vid290"
                    : "maps/survive_01.map UNIT",
                monster_kind == MONSTER_PROBE_DEATH    ? "up_to_35s_after_world_settles"
                : monster_kind == MONSTER_PROBE_COMBAT ? "up_to_20s_after_world_settles"
                                                       : "30s");
        if (monster_kind == MONSTER_PROBE_FLYING_RELOAD)
            fprintf(log, "# MONSTER reload_check=1 after_seconds=10\n");
    }
    if (!multiplayer_enabled)
    {
        hook_attempted = 1;
        hook_result = diag_tick_install((void* volatile*)(base + STEAM_GAME_VTABLE_RVA +
                                                          STEAM_GAME_TICK_SLOT * sizeof(uintptr_t)),
                                        (void*)(base + STEAM_GAME_TICK_RVA), base);
        if (hook_result != DIAG_TICK_OK)
        {
            fprintf(log, "ERROR tick hook installation result=%d\n", hook_result);
            goto fail;
        }
        fprintf(log, "# TICK_HOOK installed slot=%u original=%08lX\n", STEAM_GAME_TICK_SLOT,
                (unsigned long)(base + STEAM_GAME_TICK_RVA));
    }
    InterlockedExchange(&status, DIAG_WAITING);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    /* 1 ms timer resolution: the worker polls the socket and sends states
       every couple of milliseconds instead of in 16-31 ms bursts. */
    timeBeginPeriod(1);
    while (GetFileAttributesW(stop_path) == INVALID_FILE_ATTRIBUTES)
    {
        FrameSample frames[64];
        unsigned int n = diag_tick_drain(frames, 64);
        for (unsigned int i = 0; i < n; ++i)
            log_frame(log, &frames[i], &last, &previous, &last_log);
        DWORD now = clock_ms();
        if (runtime_attempted)
            runtime_worker_step(now);
        if (now - last_stats >= 5000)
        {
            log_stats(log);
            last_stats = now;
        }
        Sleep(2);
    }
    timeEndPeriod(1);
    int cleanup_ok = 1;
    if (runtime_attempted)
        cleanup_ok = runtime_stop(5000u);
    else
    {
        diag_tick_request_dummy_stop();
        diag_tick_request_monster_stop();
        DWORD cleanup_started = GetTickCount();
        while ((!diag_tick_dummy_stopped() || !diag_tick_monster_stopped()) &&
               GetTickCount() - cleanup_started < 5000u)
        {
            FrameSample pending[64];
            unsigned int available = diag_tick_drain(pending, 64);
            for (unsigned int i = 0; i < available; ++i)
                log_frame(log, &pending[i], &last, &previous, &last_log);
            Sleep(20);
        }
        if (!diag_tick_dummy_stopped() || !diag_tick_monster_stopped())
        {
            fprintf(log, "# DIAGNOSTIC cleanup timeout; hook retained\n");
            cleanup_ok = 0;
        }
        else
        {
            hook_result = diag_tick_stop();
            cleanup_ok = hook_result == DIAG_TICK_OK;
        }
    }
    FrameSample frames[64];
    unsigned int n;
    while ((n = diag_tick_drain(frames, 64)) != 0)
        for (unsigned int i = 0; i < n; ++i)
            log_frame(log, &frames[i], &last, &previous, &last_log);
    fprintf(log, "# STOP requested by asmp-diag.stop; hook_restore_result=%d cleanup=%d\n",
            hook_result, cleanup_ok);
    log_stats(log);
    InterlockedExchange(&status, cleanup_ok ? DIAG_STOPPED : DIAG_ERROR);
    fclose(log);
    return 0;
fail:
    if (runtime_attempted)
    {
        int restored = runtime_stop(5000u);
        if (log)
            fprintf(log, "# ERROR runtime cleanup=%d\n", restored);
    }
    if (hook_attempted || monster_attempted)
    {
        enum DiagTickResult restored = diag_tick_stop();
        if (log)
            fprintf(log, "# ERROR diagnostic hook cleanup=%d\n", restored);
    }
    if (log)
    {
        fprintf(log, "ERROR diagnostic initialization failed\n");
        fclose(log);
    }
    InterlockedExchange(&status, DIAG_ERROR);
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        self = instance;
        worker = CreateThread(NULL, 0, run, NULL, 0, NULL);
        if (!worker)
            InterlockedExchange(&status, DIAG_ERROR);
    }
    else if (reason == DLL_PROCESS_DETACH && worker)
        CloseHandle(worker);
    return TRUE;
}
