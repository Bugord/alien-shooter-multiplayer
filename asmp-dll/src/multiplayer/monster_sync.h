#ifndef ASMP_MONSTER_SYNC_H
#define ASMP_MONSTER_SYNC_H
#include "client/state_client.h"
#include "../game/unit_combat.h"

enum MonsterRole { MONSTER_DISABLED, MONSTER_HOST, MONSTER_CLIENT };
enum MonsterSyncState { MS_OFF, MS_WAITING, MS_ACTIVE, MS_BACKOFF, MS_ABANDONED, MS_STOPPING };
typedef struct MonsterSyncFrame {
    enum MonsterSyncState state;
    enum MonsterRole role;
    UnitPuppetSample unit;
    unsigned int event, cleanup, attacks, deaths, dropped, rejected;
    uint32_t monster_id, host_epoch;
} MonsterSyncFrame;
int monster_sync_enable(uintptr_t base, enum MonsterRole);
int monster_sync_initialize(uintptr_t base, enum MonsterRole, const UnitPuppetEngine*, ActorCreate);
/* Worker only; all handoffs are bounded. No engine work in these operations. */
void monster_sync_publish(const MonsterState*, int local_authority, DWORD now);
void monster_sync_receive(const MonsterEvent*, DWORD now);
int monster_sync_take_state(MpMonsterState*);
int monster_sync_take_event(MpMonsterEvent*);
/* Game thread, after native update; epoch comes from the shared world owner. */
MonsterSyncFrame monster_sync_tick(const Snapshot*, enum ProbeResult, uint32_t epoch, uint32_t tick, DWORD now);
void monster_sync_request_stop(void);
int monster_sync_stopped(void);
int monster_sync_stop(void);
#endif
