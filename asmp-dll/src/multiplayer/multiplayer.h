#ifndef ASMP_MULTIPLAYER_H
#define ASMP_MULTIPLAYER_H
#include "../game/actor.h"
#include "client/state_client.h"
enum RemoteState { RS_IDLE, RS_WAITING, RS_SPAWNING, RS_SPAWNED, RS_BACKOFF, RS_ABANDONED };
typedef struct RemoteResult {
    unsigned int id;
    ActorResult actor;
    enum RemoteState state;
    int cleanup_failed;
} RemoteResult;
typedef struct MultiplayerFrame {
    uint32_t world_epoch;
    unsigned int count, shots_applied, shots_discarded;
    RemoteResult remote[MP_MAX_PEERS];
} MultiplayerFrame;
/* Explicit engine binding keeps the coordinator independently testable. */
int multiplayer_initialize(uintptr_t image_base, const ActorEngine* api);
int multiplayer_enable(uintptr_t image_base);
/* Worker -> game: latest state replaces previous state; shots stay events. */
void multiplayer_publish(const PeerState peers[MP_MAX_PEERS], DWORD now);
void multiplayer_receive_shot(const ShotEvent*, DWORD now);
/* Called only after the original engine update. */
MultiplayerFrame multiplayer_tick(const Snapshot*, enum ProbeResult, DWORD now);
/* Game -> worker: captured local attacks, never native calls from the worker. */
void multiplayer_capture_shot(int x, int y, unsigned int weapon);
int multiplayer_take_local_shot(MpShot*);
void multiplayer_draw(void);
int multiplayer_is_replica(uintptr_t entity);
void multiplayer_request_stop(void);
int multiplayer_stopped(void);
/* Worker, after game-thread cleanup and hooks stop. Refuses live leftovers. */
int multiplayer_stop(void);
enum RemoteState multiplayer_remote_state(unsigned int id);
#endif
