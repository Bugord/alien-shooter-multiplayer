/* Production runtime: validates the EXE profile, installs the hooks in a fixed order,
   runs the worker loop (session commands, the hosting player's own server, network,
   room requests) and dispatches the game tick, then shuts everything down in reverse
   order. Layer: multiplayer/ (orchestration). Start, worker_step and stop run on the
   worker thread; runtime_game_tick on the game thread. */

#include <string.h>
#include "runtime.h"
#include "server_process.h"
#include "lobby_requests.h"
#include "client/transport.h"
#include "../game/world_hook.h"
#include "../game/ui.h"

/* A connection that never gets the welcome gives up after this time. */
#define RUNTIME_CONNECT_TIMEOUT_MS 10000u

/* After the host disconnects, its server runs this long so guests receive the
   closed-room snapshot before the process ends. */
#define SERVER_LINGER_MS 500u

#define RUNTIME_STATS_MS 5000u
enum RuntimeStage
{
    ST_NONE,
    ST_BOUND,
    ST_SESSION,
    ST_COORDINATOR,
    ST_ACTION,
    ST_WORLD,
    ST_MONSTER,
    ST_TICK
};
enum NetworkState
{
    NS_OFF,
    NS_HOSTING, /* Our server is starting; the client connects when it is ready. */
    NS_CONNECTING,
    NS_CONNECTED,
    NS_FAILED /* The failure is published; waiting for the next session command. */
};
static struct
{
    volatile LONG state, game_thread;
    DWORD worker_thread;
    enum RuntimeStage stage;
    uintptr_t base;
    RuntimeConfig config;
    RuntimeBindings bindings;
    StateClient* client;
    enum NetworkState network;
    enum SessionFailure failure;
    ConnectionRequest command;
    LobbyRequests requests;
    char lan_address[16]; /* Host: found once the connection to our server stands. */
    struct
    {
        ServerProcess process;
        int lingering; /* Stop at `stop_at`; the host already left. */
        DWORD stop_at;
    } server;
    unsigned int generation, display_ticks;
    DWORD connecting_at, stats_at, monster_sent_at;
    RuntimeFrame latest;
    enum DisplayResult display, logged_display;
    enum SessionState logged_session;
} runtime;
static SRWLOCK frame_lock = SRWLOCK_INIT;

/* Tick hook callback on the game thread: builds a frame and runs runtime_game_tick. */
static void on_tick(void* game, LONG tick, DWORD now)
{
    RuntimeFrame frame = {0};
    frame.tick = tick;
    frame.milliseconds = now;
    runtime_game_tick(game, &frame);
}

/* Display hook callback (game thread): replica bars and names, then the room panel. */
static void draw_overlays(void)
{
    multiplayer_draw();
    session_draw();
}

/* Current lifecycle state; thread-safe. */
enum RuntimeState runtime_state(void)
{
    return (enum RuntimeState)InterlockedCompareExchange(&runtime.state, 0, 0);
}

/* Checks the player accessor, the game tick function and its vtable slot against the
   supported Steam EXE before anything is hooked. */
static int validate_profile(uintptr_t base)
{
    static const unsigned char accessor[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x83, 0xE0,
                                             0x03, 0x8B, 0x84, 0x81, 0x44, 0x02, 0x00, 0x00,
                                             0x8B, 0x40, 0x10, 0x5D, 0xC2, 0x04, 0x00};
    static const unsigned char prefix[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
    __try
    {
        return !memcmp((void*)(base + STEAM_PLAYER_ACCESSOR_RVA), accessor, sizeof(accessor)) &&
               !memcmp((void*)(base + STEAM_GAME_TICK_RVA), prefix, sizeof(prefix)) &&
               *(uintptr_t*)(base + STEAM_GAME_VTABLE_RVA +
                             STEAM_GAME_TICK_SLOT * sizeof(uintptr_t)) ==
                   base + STEAM_GAME_TICK_RVA;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

/* Starts with the real engine bindings. Returns 1 on success; on 0 everything that was
   installed has been rolled back. */
int runtime_start(uintptr_t base, const RuntimeConfig* config)
{
    if (!validate_profile(base))
        return 0;
    RuntimeBindings bindings = {actor_engine_bind,     session_enable,     action_hook_install,
                                action_hook_stop,      world_hook_install, world_hook_stop,
                                world_hook_generation, tick_hook_install,  tick_hook_stop,
                                display_hook_install,  display_hook_stop};
    return runtime_start_with_bindings(base, config, &bindings);
}

/* Same as runtime_start with injectable bindings (tests). Starts the stages in the order of
   RuntimeStage; on failure it rolls back through runtime_stop. */
int runtime_start_with_bindings(uintptr_t base, const RuntimeConfig* config,
                                const RuntimeBindings* bindings)
{
    if (!base || !config || !bindings || !bindings->actor_bind || !bindings->session_enable ||
        !bindings->action_install || !bindings->action_stop || !bindings->world_install ||
        !bindings->world_stop || !bindings->world_generation || !bindings->tick_install ||
        !bindings->tick_stop || !bindings->display_install || !bindings->display_stop ||
        (config->direct &&
         (!memchr(config->host, 0, sizeof(config->host)) || !config->host[0] ||
          !memchr(config->name, 0, sizeof(config->name)) || !config->name[0] || !config->port)) ||
        runtime_state() != RT_OFF)
        return 0;
    if (config->monster_role < MONSTER_DISABLED || config->monster_role > MONSTER_CLIENT ||
        (config->monster_role && !config->direct))
        return 0;
    memset(&runtime, 0, sizeof(runtime));
    runtime.base = base;
    runtime.config = *config;
    runtime.bindings = *bindings;
    runtime.worker_thread = GetCurrentThreadId();
    runtime.display = runtime.logged_display = DISPLAY_PENDING;
    runtime.logged_session = SS_OFFLINE;
    runtime.display_ticks = 30;
    InterlockedExchange(&runtime.state, RT_STARTING);
    ActorEngine actor;
    if (!bindings->actor_bind(base, &actor))
        goto fail;
    runtime.stage = ST_BOUND;
    if (!bindings->session_enable(base))
        goto fail;
    runtime.stage = ST_SESSION;
    if (!multiplayer_initialize(base, &actor))
        goto fail;
    runtime.stage = ST_COORDINATOR;
    if (config->direct && !session_direct(config->host, config->port, config->name))
        goto fail;
    /* Attempted hook stages participate in rollback even if install fails. */
    runtime.stage = ST_ACTION;
    if (!bindings->action_install(base, multiplayer_is_replica, multiplayer_capture_shot))
        goto fail;
    runtime.stage = ST_WORLD;
    if (!bindings->world_install(base))
        goto fail;
    runtime.stage = ST_MONSTER;
    if (config->monster_role && !monster_sync_enable(base, config->monster_role))
        goto fail;
    if (config->monster_gallery && (!config->monster_role || !monster_sync_gallery(1)))
        goto fail;
    runtime.stage = ST_TICK;
    if (bindings->tick_install((void* volatile*)(base + STEAM_GAME_VTABLE_RVA +
                                                 STEAM_GAME_TICK_SLOT * sizeof(uintptr_t)),
                               (void*)(base + STEAM_GAME_TICK_RVA), on_tick) != SLOT_OK)
        goto fail;
    /* Display is configured PENDING; its first install runs on the game thread. */
    InterlockedExchange(&runtime.state, RT_RUNNING);
    if (config->log)
        fprintf(config->log,
                "# RUNTIME started session=1 actors=1 action=1 world=1 tick=1 display=pending\n");
    return 1;
fail:
    if (config->log)
        fprintf(config->log, "# RUNTIME startup failed stage=%d; rolling back\n", runtime.stage);
    runtime_stop(3000u);
    return 0;
}

/* One game-thread tick: probes the game and advances the session, the remote players
   and the monsters. The first caller becomes the game thread; other threads are ignored. */
void runtime_game_tick(void* game, RuntimeFrame* out)
{
    enum RuntimeState state = runtime_state();
    if (state != RT_RUNNING && state != RT_STARTING && state != RT_STOPPING)
        return;
    LONG thread = (LONG)GetCurrentThreadId(),
         owner = InterlockedCompareExchange(&runtime.game_thread, thread, 0);
    if (owner && owner != thread)
        return;
    out->result = probe_read(runtime.base, &out->snapshot);
    if (out->snapshot.game != (uintptr_t)game)
        return;
    out->snapshot.world_load = runtime.bindings.world_generation();
    if (state != RT_STOPPING)
    {
        out->session_result = session_tick(&out->snapshot, out->result, out->milliseconds);
        if (out->session_result)
        {
            out->result = probe_read(runtime.base, &out->snapshot);
            out->snapshot.world_load = runtime.bindings.world_generation();
        }
        if (runtime.display == DISPLAY_PENDING && ++runtime.display_ticks >= 30u)
        {
            runtime.display_ticks = 0;
            runtime.display = runtime.bindings.display_install(runtime.base, draw_overlays);
        }
    }
    out->session = session_state();
    out->display = runtime.display;
    out->multiplayer = multiplayer_tick(&out->snapshot, out->result, out->milliseconds);
    if (runtime.config.monster_role)
        out->monster = monster_sync_tick(&out->snapshot, out->result, out->multiplayer.world_epoch,
                                         (uint32_t)out->tick, out->milliseconds);
    if (TryAcquireSRWLockExclusive(&frame_lock))
    {
        runtime.latest = *out;
        ReleaseSRWLockExclusive(&frame_lock);
    }
    if (runtime.config.observer)
        runtime.config.observer(out);
}

/* Converts the frame into the local MpState and hands it to the network client. */
static void publish_frame(const RuntimeFrame* frame)
{
    const Snapshot* sample = &frame->snapshot;
    MpState state = {0};
    /* Before the room's load barrier the player stays inactive for everyone else. */
    if (probe_in_level(sample, frame->result) && frame->session == SS_IN_GAME)
    {
        state.world_low = sample->world_low;
        state.world_high = sample->world_high;
        state.world_epoch = frame->multiplayer.world_epoch;
        state.active = frame->result == PROBE_OK && sample->health > 0;
        state.tick = (uint32_t)frame->tick;
        state.x = sample->x;
        state.y = sample->y;
        state.z = sample->z;
        state.health = sample->health;
        state.max_health = sample->max_health > 0 ? sample->max_health : 0;
        state.animation = sample->animation;
        state.direction = sample->direction;
        state.weapon_slot = sample->weapon_slot;
        state.velocity = sample->velocity;
        state.moving = sample->moving;
        state.torso_direction = sample->torso_direction;
        state.torso_present = sample->torso_present;
        state.current_ammo = sample->current_ammo;
        memcpy(state.stored_ammo, sample->stored_ammo, sizeof(state.stored_ammo));
    }
    state_client_publish(runtime.client, &state, frame->milliseconds);
}

/* Stops our server now, including one still lingering after the host left. */
static void stop_server(void)
{
    server_process_stop(&runtime.server.process, runtime.config.log);
    runtime.server.lingering = 0;
}

/* Lets our server outlive the host's disconnect briefly (see SERVER_LINGER_MS). */
static void linger_server(DWORD now)
{
    if (runtime.server.process.state == SERVER_IDLE)
        return;
    runtime.server.lingering = 1;
    runtime.server.stop_at = now + SERVER_LINGER_MS;
}

static void stop_lingering_server(DWORD now)
{
    if (runtime.server.lingering && (LONG)(now - runtime.server.stop_at) >= 0)
        stop_server();
}

/* Ends the current connection with a reason the session will show. */
static void fail_connection(enum SessionFailure failure)
{
    state_client_destroy(runtime.client);
    runtime.client = NULL;
    runtime.network = NS_FAILED;
    runtime.failure = failure;
    stop_server();
}

static void open_client(const char* host, unsigned short port)
{
    runtime.client = state_client_create(host, port, runtime.command.name, runtime.config.log);
    if (runtime.client)
        runtime.network = NS_CONNECTING;
    else
        fail_connection(SF_NO_ANSWER); /* Unresolvable address or socket failure. */
}

/* Starts our own relay server for a host command; the client follows when it is ready. */
static void start_server(DWORD now)
{
    wchar_t path[MAX_PATH];
    stop_server(); /* A server lingering from an earlier room holds the port. */
    if (!server_process_default_path(path, MAX_PATH) ||
        !server_process_start(&runtime.server.process, path, runtime.command.port, now,
                              runtime.config.log))
    {
        fail_connection(SF_SERVER_MISSING);
        return;
    }
    runtime.network = NS_HOSTING;
}

/* Serves a new session command: closes the old connection and opens the new one. */
static void serve_session_request(DWORD now)
{
    ConnectionRequest command;
    if (!session_take_request(&command))
        return;

    state_client_destroy(runtime.client);
    runtime.client = NULL;
    runtime.network = NS_OFF;
    linger_server(now);

    runtime.command = command;
    runtime.generation = command.generation;
    runtime.connecting_at = now;
    runtime.failure = SF_NONE;
    runtime.lan_address[0] = 0;
    if (command.command == SESSION_CONNECT)
        open_client(command.host, command.port);
    else if (command.command == SESSION_HOST)
        start_server(now);
}

static enum SessionFailure server_failure(enum ServerProcessState state)
{
    if (state == SERVER_PORT_IN_USE)
        return SF_PORT_IN_USE;
    if (state == SERVER_MISSING)
        return SF_SERVER_MISSING;
    return SF_SERVER_FAILED;
}

/* Waits for our server to become ready, and notices when it ends while in use. */
static void advance_server(DWORD now)
{
    enum ServerProcessState state =
        server_process_poll(&runtime.server.process, now, runtime.config.log);

    if (runtime.network == NS_HOSTING)
    {
        if (state == SERVER_READY)
            open_client(runtime.command.host, runtime.command.port);
        else if (state != SERVER_STARTING)
            fail_connection(server_failure(state));
        return;
    }
    if (runtime.server.lingering)
        return;
    int in_use = runtime.network == NS_CONNECTING || runtime.network == NS_CONNECTED;
    if (in_use && (state == SERVER_EXITED || state == SERVER_PORT_IN_USE))
        fail_connection(SF_LOST);
}

/* True once the client's link can no longer become ready. */
static int link_ended(ClientLink link)
{
    return link == LINK_NO_ANSWER || link == LINK_REFUSED || link == LINK_LOST ||
           link == LINK_INCOMPATIBLE;
}

/* Maps how the client's link ended to the reason shown in the menu. */
static enum SessionFailure link_failure(ClientLink link)
{
    if (link == LINK_INCOMPATIBLE)
        return SF_INCOMPATIBLE;
    if (link == LINK_NO_ANSWER)
        return SF_NO_ANSWER;
    if (link == LINK_REFUSED)
        return SF_REFUSED;
    return SF_LOST;
}

/* Promotes a welcomed client to connected, or fails a link that ended or never
   finished the handshake. */
static void track_connection(DWORD now)
{
    if (runtime.network != NS_CONNECTING && runtime.network != NS_CONNECTED)
        return;

    ClientLink link = state_client_link(runtime.client);
    if (link == LINK_INCOMPATIBLE)
    {
        fail_connection(SF_INCOMPATIBLE);
        return;
    }
    if (state_client_connected(runtime.client))
    {
        /* The room header tells the host which address friends type. */
        if (runtime.network != NS_CONNECTED && runtime.command.command == SESSION_HOST)
            transport_udp_lan_address(runtime.lan_address, sizeof(runtime.lan_address));
        runtime.network = NS_CONNECTED;
        return;
    }
    if (runtime.network == NS_CONNECTED || link_ended(link))
    {
        fail_connection(runtime.network == NS_CONNECTED ? SF_LOST : link_failure(link));
        return;
    }
    if (now - runtime.connecting_at >= RUNTIME_CONNECT_TIMEOUT_MS)
    {
        /* Answered but never welcomed: an older build or not an ASMP server. */
        fail_connection(link == LINK_HANDSHAKE ? SF_INCOMPATIBLE : SF_NO_ANSWER);
    }
}

/* Publishes the current link (connection, failure and room) to the session. */
static void publish_link(void)
{
    if (!runtime.generation || runtime.command.command == SESSION_DISCONNECT)
        return;

    SessionLink link;
    memset(&link, 0, sizeof(link));
    link.connected = runtime.network == NS_CONNECTED;
    link.failure = runtime.failure;
    const char* map = state_client_map(runtime.client);
    if (map)
        strcpy_s(link.map, sizeof(link.map), map);
    link.lobby_known = state_client_lobby(runtime.client, &link.lobby, &link.local_id);
    strcpy_s(link.local_address, sizeof(link.local_address), runtime.lan_address);
    session_publish_link(runtime.generation, &link);
}

static int send_ready(void* context, const MpLobbyReady* request)
{
    return state_client_send_lobby_ready(context, request);
}

static int send_start(void* context, const MpLobbyStart* request)
{
    return state_client_send_lobby_start(context, request);
}

static int send_loaded(void* context, const MpLobbyLoaded* request)
{
    return state_client_send_lobby_loaded(context, request);
}

static int send_settings(void* context, const MpLobbySettings* request)
{
    return state_client_send_lobby_settings(context, request);
}

static int send_profile(void* context, const MpLobbyProfile* request)
{
    return state_client_send_lobby_profile(context, request);
}

/* Repeats the room requests of the game thread until the snapshot shows them. */
static void send_lobby_requests(DWORD now)
{
    MpLobby lobby;
    uint32_t local_id = 0;
    LobbyIntent intent;
    if (runtime.network != NS_CONNECTED || !runtime.command.lobby)
        return;
    if (!state_client_lobby(runtime.client, &lobby, &local_id) || !session_read_intent(&intent))
        return;
    if (intent.generation != runtime.generation)
        return;

    LobbySender sender = {runtime.client, send_ready,    send_start,
                          send_loaded,    send_settings, send_profile};
    lobby_requests_step(&runtime.requests, &intent, &lobby, local_id, now, &sender);
}

/* Remote players: latest peer states to the game thread, shots in both directions. */
static void exchange_players(DWORD now)
{
    PeerState peers[MP_MAX_PEERS];
    for (unsigned int id = 0; id < MP_MAX_PEERS; ++id)
        state_client_peer_info(runtime.client, id, now, &peers[id]);
    multiplayer_publish(peers, now);

    ShotEvent received;
    while (state_client_take_shot(runtime.client, &received))
        multiplayer_receive_shot(&received, now);

    MpShot outgoing;
    while (multiplayer_take_local_shot(&outgoing))
        state_client_send_shot(runtime.client, &outgoing, now);
}

/* Opt-in monster authority: states and events between the coordinator and the relay. */
static void exchange_monsters(DWORD now)
{
    uint32_t authority_id, authority_session;
    int is_local = 0;
    state_client_monster_authority(runtime.client, &authority_id, &authority_session, &is_local);
    for (unsigned int slot = 0; slot < MP_MONSTER_CAPACITY; ++slot)
    {
        MonsterState source = {0};
        state_client_monster_at(runtime.client, slot, now, &source);
        monster_sync_publish_at(slot, &source, is_local, now);
    }

    MonsterEvent event;
    while (state_client_take_monster_event(runtime.client, &event))
        monster_sync_receive(&event, now);

    MpMonsterState outgoing_state;
    if (now - runtime.monster_sent_at >= MP_STATE_SEND_MS)
    {
        for (unsigned int count = 0;
             count < MP_MONSTER_CAPACITY && monster_sync_take_state(&outgoing_state); ++count)
            state_client_publish_monster(runtime.client, &outgoing_state, now);
        runtime.monster_sent_at = now;
    }

    MpMonsterEvent outgoing_event;
    while (monster_sync_take_event(&outgoing_event))
        state_client_send_monster_event(runtime.client, &outgoing_event, now);
}

/* Logs display/session changes and periodic network statistics. */
static void log_changes(const RuntimeFrame* frame, DWORD now)
{
    if (runtime.config.log && frame->display != runtime.logged_display)
    {
        fprintf(runtime.config.log, "# DISPLAY result=%d\n", frame->display);
        runtime.logged_display = frame->display;
    }
    if (runtime.config.log && frame->session != runtime.logged_session)
    {
        fprintf(runtime.config.log, "# SESSION state=%d\n", frame->session);
        runtime.logged_session = frame->session;
    }
    if (now - runtime.stats_at >= RUNTIME_STATS_MS)
    {
        state_client_stats(runtime.client);
        runtime.stats_at = now;
    }
}

/* One worker-thread step: session commands, our server, the network client, the room
   and the gameplay exchanges. */
void runtime_worker_step(DWORD now)
{
    if (runtime_state() != RT_RUNNING || GetCurrentThreadId() != runtime.worker_thread)
        return;

    serve_session_request(now);
    advance_server(now);
    stop_lingering_server(now);

    RuntimeFrame frame;
    AcquireSRWLockShared(&frame_lock);
    frame = runtime.latest;
    ReleaseSRWLockShared(&frame_lock);
    if (frame.tick)
        publish_frame(&frame);

    state_client_update(runtime.client, now);
    if (runtime.config.monster_role == MONSTER_HOST)
        state_client_monster_claim(runtime.client);
    track_connection(now);
    publish_link();
    send_lobby_requests(now);

    exchange_players(now);
    if (runtime.config.monster_role)
        exchange_monsters(now);
    log_changes(&frame, now);
}

/* Stops the coordinators on the game thread first, then unhooks in reverse order. A timeout
   or a foreign hook returns 0 and keeps STOPPING, so a later call can finish. */
int runtime_stop(DWORD timeout_ms)
{
    if (runtime_state() == RT_OFF)
        return 1;
    if (GetCurrentThreadId() != runtime.worker_thread)
        return 0;
    DWORD started = GetTickCount();
    InterlockedExchange(&runtime.state, RT_STOPPING);
    if (runtime.stage >= ST_SESSION)
        session_stop();
    if (runtime.config.monster_role && runtime.stage >= ST_MONSTER)
        monster_sync_request_stop();
    if (runtime.stage >= ST_COORDINATOR)
    {
        multiplayer_request_stop();
        while (!multiplayer_stopped() || (runtime.config.monster_role &&
                                          runtime.stage >= ST_MONSTER && !monster_sync_stopped()))
        {
            if (GetTickCount() - started >= timeout_ms)
            {
                if (runtime.config.log)
                    fprintf(runtime.config.log, "# RUNTIME cleanup timeout; game thread paused or "
                                                "native entities remain; hooks retained\n");
                state_client_destroy(runtime.client);
                runtime.client = NULL;
                runtime.network = NS_OFF;
                stop_server();
                return 0;
            }
            Sleep(1);
        }
    }
    if (runtime.config.monster_role && runtime.stage >= ST_MONSTER && !monster_sync_stop())
        return 0;
    int display_ok = runtime.stage < ST_TICK || runtime.bindings.display_stop();
    int action_ok = runtime.stage < ST_ACTION || runtime.bindings.action_stop();
    int world_ok = runtime.stage < ST_WORLD || runtime.bindings.world_stop();
    enum SlotResult tick_result = runtime.stage < ST_TICK ? SLOT_OK : runtime.bindings.tick_stop();
    while (tick_result == SLOT_BUSY && GetTickCount() - started < timeout_ms)
    {
        Sleep(1);
        tick_result = runtime.bindings.tick_stop();
    }
    state_client_stats(runtime.client);
    state_client_destroy(runtime.client);
    runtime.client = NULL;
    runtime.network = NS_OFF;
    stop_server();
    if (runtime.config.log)
        fprintf(runtime.config.log, "# HOOK_RESTORE action=%d display=%d world=%d tick=%d\n",
                action_ok, display_ok, world_ok, tick_result);
    if (!display_ok || !action_ok || !world_ok || tick_result != SLOT_OK)
        return 0;
    if (runtime.stage >= ST_COORDINATOR && !multiplayer_stop())
        return 0;
    if (runtime.stage >= ST_SESSION)
        ui_stop();
    AcquireSRWLockExclusive(&frame_lock);
    memset(&runtime.latest, 0, sizeof(runtime.latest));
    ReleaseSRWLockExclusive(&frame_lock);
    runtime.stage = ST_NONE;
    runtime.base = 0;
    InterlockedExchange(&runtime.game_thread, 0);
    InterlockedExchange(&runtime.state, RT_OFF);
    return 1;
}
