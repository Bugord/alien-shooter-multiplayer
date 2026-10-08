#ifndef ASMP_STEAM_MULTIPLAYER_H
#define ASMP_STEAM_MULTIPLAYER_H
#include "../../game/steam/steam_actor.h"
#include "../client/steam_state_client.h"
enum SteamRemoteState { RS_IDLE, RS_WAITING, RS_SPAWNING, RS_SPAWNED, RS_BACKOFF, RS_ABANDONED };
typedef struct SteamRemoteResult {
    unsigned int id;
    ActorResult actor;
    enum SteamRemoteState state;
    int cleanup_failed;
} SteamRemoteResult;
typedef struct SteamMultiplayerFrame {
    uint32_t world_epoch;
    unsigned int count, shots_applied, shots_discarded;
    SteamRemoteResult remote[STEAM_MAX_PEERS];
} SteamMultiplayerFrame;
/* Explicit engine binding keeps the coordinator independently testable. */
int steam_multiplayer_initialize(uintptr_t image_base, const ActorEngine* api);
int steam_multiplayer_enable(uintptr_t image_base);
/* Worker -> game: latest state replaces previous state; shots stay events. */
void steam_multiplayer_publish(const SteamPeerState peers[STEAM_MAX_PEERS], DWORD now);
void steam_multiplayer_receive_shot(const SteamShotEvent*, DWORD now);
/* Called only after the original engine update. */
SteamMultiplayerFrame steam_multiplayer_tick(const Snapshot*, enum ProbeResult, DWORD now);
/* Game -> worker: captured local attacks, never native calls from the worker. */
void steam_multiplayer_capture_shot(int x, int y, unsigned int weapon);
int steam_multiplayer_take_local_shot(MpSteamShot*);
void steam_multiplayer_draw(void);
int steam_multiplayer_is_replica(uintptr_t entity);
void steam_multiplayer_request_stop(void);
int steam_multiplayer_stopped(void);
/* Worker, after game-thread cleanup and hooks stop. Refuses live leftovers. */
int steam_multiplayer_stop(void);
enum SteamRemoteState steam_multiplayer_remote_state(unsigned int id);
#endif
