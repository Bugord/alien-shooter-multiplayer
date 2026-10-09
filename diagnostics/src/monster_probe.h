#ifndef ASMP_MONSTER_PROBE_H
#define ASMP_MONSTER_PROBE_H
#include "../../asmp-dll/src/game/unit_puppet.h"
#include "../../asmp-dll/src/game/unit_combat.h"
enum MonsterProbeState { MONSTER_PROBE_OFF, MONSTER_PROBE_MENU, MONSTER_PROBE_SEARCH,
    MONSTER_PROBE_RUNNING, MONSTER_PROBE_DONE, MONSTER_PROBE_FAILED };
enum MonsterProbeKind { MONSTER_PROBE_GROUND, MONSTER_PROBE_FLYING, MONSTER_PROBE_FLYING_RELOAD, MONSTER_PROBE_COMBAT };
enum MonsterCombatPhase { MONSTER_COMBAT_IDLE, MONSTER_COMBAT_DAMAGE, MONSTER_COMBAT_ATTACK,
    MONSTER_COMBAT_RECOVER, MONSTER_COMBAT_DEATH, MONSTER_COMBAT_DISPOSED };
typedef struct MonsterProbeFrame {
    enum MonsterProbeState state;
    unsigned int phase, violations;
    int event, cleanup, reloaded;
    UnitPuppetSample unit;
    UnitCombatSample combat;
    int combat_event, damage_blocked;
} MonsterProbeFrame;
int monster_probe_enable(uintptr_t base);
int monster_probe_enable_kind(uintptr_t base, enum MonsterProbeKind);
MonsterProbeFrame monster_probe_tick(const Snapshot*, enum ProbeResult, DWORD now, int stop);
int monster_probe_stop(void);
#endif
