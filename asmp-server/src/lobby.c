/* Room state of the relay server: who is in the room, who hosts it, who is ready,
   which map is selected, and the start/load barrier that lets gameplay begin only
   after every participant loaded the map. Layer: asmp-server (relay). No sockets
   and no clock of its own: server.c passes events and the current time. */

#include <string.h>
#include "lobby.h"

/* Mixes the seed with the room counter so a reset room never reuses the old id. */
static uint32_t next_room_id(Lobby* lobby)
{
    uint32_t value = lobby->room_seed ^ (++lobby->rooms_created * 0x9E3779B9u);
    value ^= value >> 16;
    value *= 0x85EBCA6Bu;
    value ^= value >> 13;
    return value ? value : 1u;
}

/* Every visible change gets a new revision; a start request names the revision it saw. */
static void touch(Lobby* lobby)
{
    ++lobby->revision;
}

/* Starts a fresh open room with no members; the map and capacity stay. */
static void open_new_room(Lobby* lobby)
{
    memset(lobby->members, 0, sizeof(lobby->members));
    lobby->room = next_room_id(lobby);
    lobby->settings_revision = 1;
    lobby->phase = MP_LOBBY_OPEN;
    lobby->has_host = 0;
    lobby->host_id = 0;
    lobby->host_session = 0;
    lobby->run_epoch = 0;
    lobby->notice = MP_LOBBY_NOTICE_NONE;
    lobby->started_at = 0;
    touch(lobby);
}

int lobby_init(Lobby* lobby, uint32_t seed, unsigned int capacity, const char* map)
{
    if (!lobby || capacity < 1 || capacity > MP_ROSTER_MAX_ENTRIES || !mp_mode_for_map(map) ||
        strlen(map) >= sizeof(lobby->map))
        return 0;

    memset(lobby, 0, sizeof(*lobby));
    lobby->room_seed = seed;
    lobby->capacity = capacity;
    strcpy_s(lobby->map, sizeof(lobby->map), map);
    open_new_room(lobby);
    return 1;
}

/* Returns the member `id` if it is present with exactly this session, else NULL. */
static LobbyMember* find_member(Lobby* lobby, uint32_t id, uint32_t session)
{
    if (id >= lobby->capacity)
        return NULL;

    LobbyMember* member = &lobby->members[id];
    if (!member->present || member->session != session)
        return NULL;
    return member;
}

static int is_host(const Lobby* lobby, uint32_t id, uint32_t session)
{
    return lobby->has_host && lobby->host_id == id && lobby->host_session == session;
}

static int room_empty(const Lobby* lobby)
{
    for (unsigned int i = 0; i < lobby->capacity; ++i)
    {
        if (lobby->members[i].present)
            return 0;
    }
    return 1;
}

static void clear_ready(Lobby* lobby)
{
    for (unsigned int i = 0; i < lobby->capacity; ++i)
        lobby->members[i].ready = 0;
}

static void clear_participants(Lobby* lobby)
{
    for (unsigned int i = 0; i < lobby->capacity; ++i)
    {
        lobby->members[i].participant = 0;
        lobby->members[i].loaded = 0;
    }
}

/* Returns the room from a start to the open lobby and records why. The run epoch
   stays, so clients can tell which start was cancelled. */
static void cancel_start(Lobby* lobby, enum MpLobbyNotice reason)
{
    lobby->phase = MP_LOBBY_OPEN;
    lobby->notice = reason;
    lobby->started_at = 0;
    clear_participants(lobby);
    clear_ready(lobby);
    touch(lobby);
}

enum LobbyResult lobby_join(Lobby* lobby, uint32_t id, uint32_t session, const char* name)
{
    if (!lobby || id >= lobby->capacity || !session || !mp_is_player_name(name))
        return LOBBY_UNCHANGED;

    LobbyMember* member = &lobby->members[id];
    if (member->present && member->session == session)
        return LOBBY_UNCHANGED; /* Repeated hello after a lost welcome. */

    memset(member, 0, sizeof(*member));
    member->present = 1;
    member->session = session;
    strcpy_s(member->name, sizeof(member->name), name);

    if (!lobby->has_host && lobby->phase == MP_LOBBY_OPEN)
    {
        lobby->has_host = 1;
        lobby->host_id = id;
        lobby->host_session = session;
    }
    touch(lobby);
    return LOBBY_CHANGED;
}

enum LobbyResult lobby_leave(Lobby* lobby, uint32_t id, uint32_t session)
{
    if (!lobby)
        return LOBBY_UNCHANGED;

    LobbyMember* member = find_member(lobby, id, session);
    if (!member)
        return LOBBY_UNCHANGED;

    int was_participant = member->participant;
    memset(member, 0, sizeof(*member));

    if (room_empty(lobby))
    {
        open_new_room(lobby);
        return LOBBY_CHANGED;
    }

    if (is_host(lobby, id, session))
    {
        /* No host migration: the room ends and its members return to the menu. */
        lobby->phase = MP_LOBBY_CLOSED;
        lobby->has_host = 0;
        clear_participants(lobby);
        clear_ready(lobby);
        touch(lobby);
        return LOBBY_CHANGED;
    }

    if (lobby->phase == MP_LOBBY_STARTING && was_participant)
    {
        cancel_start(lobby, MP_LOBBY_NOTICE_PLAYER_LEFT);
        return LOBBY_CHANGED;
    }

    touch(lobby);
    return LOBBY_CHANGED;
}

enum LobbyResult lobby_set_ready(Lobby* lobby, uint32_t id, uint32_t session,
                                 const MpLobbyReady* request)
{
    if (!lobby || !request || lobby->phase != MP_LOBBY_OPEN)
        return LOBBY_UNCHANGED;
    if (request->room != lobby->room || request->settings_revision != lobby->settings_revision)
        return LOBBY_UNCHANGED;

    LobbyMember* member = find_member(lobby, id, session);
    if (!member || is_host(lobby, id, session))
        return LOBBY_UNCHANGED; /* The host's start request is its consent. */

    int ready = request->ready != 0;
    if (member->ready == ready)
        return LOBBY_UNCHANGED;

    member->ready = ready;
    lobby->notice = MP_LOBBY_NOTICE_NONE;
    touch(lobby);
    return LOBBY_CHANGED;
}

enum LobbyResult lobby_change_settings(Lobby* lobby, uint32_t id, uint32_t session,
                                       const MpLobbySettings* request)
{
    if (!lobby || !request || lobby->phase != MP_LOBBY_OPEN || !is_host(lobby, id, session))
        return LOBBY_UNCHANGED;
    if (request->room != lobby->room || request->settings_revision != lobby->settings_revision)
        return LOBBY_UNCHANGED;
    if (!mp_mode_for_map(request->map) || strlen(request->map) >= sizeof(lobby->map))
        return LOBBY_UNCHANGED;
    if (!strcmp(request->map, lobby->map))
        return LOBBY_UNCHANGED;

    strcpy_s(lobby->map, sizeof(lobby->map), request->map);
    ++lobby->settings_revision;
    clear_ready(lobby); /* Players agreed to the old settings, not to these. */
    lobby->notice = MP_LOBBY_NOTICE_NONE;
    touch(lobby);
    return LOBBY_CHANGED;
}

/* True when every present member other than the host is ready. */
static int everyone_ready(const Lobby* lobby)
{
    for (unsigned int i = 0; i < lobby->capacity; ++i)
    {
        const LobbyMember* member = &lobby->members[i];
        if (!member->present || is_host(lobby, i, member->session))
            continue;
        if (!member->ready)
            return 0;
    }
    return 1;
}

enum LobbyResult lobby_start(Lobby* lobby, uint32_t id, uint32_t session,
                             const MpLobbyStart* request, unsigned long now)
{
    if (!lobby || !request || lobby->phase != MP_LOBBY_OPEN || !is_host(lobby, id, session))
        return LOBBY_UNCHANGED;

    /* A repeated or stale start names an old revision and is ignored. */
    if (request->room != lobby->room || request->revision != lobby->revision)
        return LOBBY_UNCHANGED;
    if (!everyone_ready(lobby))
        return LOBBY_UNCHANGED;

    ++lobby->run_epoch;
    if (!lobby->run_epoch)
        lobby->run_epoch = 1;

    for (unsigned int i = 0; i < lobby->capacity; ++i)
    {
        LobbyMember* member = &lobby->members[i];
        member->participant = member->present;
        member->loaded = 0;
        member->ready = 0;
    }

    lobby->phase = MP_LOBBY_STARTING;
    lobby->notice = MP_LOBBY_NOTICE_NONE;
    lobby->started_at = now;
    touch(lobby);
    return LOBBY_CHANGED;
}

static int everyone_loaded(const Lobby* lobby)
{
    for (unsigned int i = 0; i < lobby->capacity; ++i)
    {
        const LobbyMember* member = &lobby->members[i];
        if (member->present && member->participant && !member->loaded)
            return 0;
    }
    return 1;
}

enum LobbyResult lobby_mark_loaded(Lobby* lobby, uint32_t id, uint32_t session,
                                   const MpLobbyLoaded* request)
{
    if (!lobby || !request || lobby->phase != MP_LOBBY_STARTING)
        return LOBBY_UNCHANGED;
    if (request->room != lobby->room || request->run_epoch != lobby->run_epoch)
        return LOBBY_UNCHANGED;

    LobbyMember* member = find_member(lobby, id, session);
    if (!member || !member->participant || member->loaded)
        return LOBBY_UNCHANGED;

    member->loaded = 1;
    if (everyone_loaded(lobby))
    {
        lobby->phase = MP_LOBBY_IN_GAME;
        lobby->started_at = 0;
    }
    touch(lobby);
    return LOBBY_CHANGED;
}

enum LobbyResult lobby_tick(Lobby* lobby, unsigned long now)
{
    if (!lobby || lobby->phase != MP_LOBBY_STARTING)
        return LOBBY_UNCHANGED;
    if (now - lobby->started_at < MP_LOBBY_LOAD_TIMEOUT_MS)
        return LOBBY_UNCHANGED;

    cancel_start(lobby, MP_LOBBY_NOTICE_LOAD_TIMEOUT);
    return LOBBY_CHANGED;
}

/* Packs one member's state into the wire flags. */
static uint32_t member_flags(const LobbyMember* member)
{
    uint32_t flags = 0;
    if (member->ready)
        flags |= MP_LOBBY_ENTRY_READY;
    if (member->participant)
        flags |= MP_LOBBY_ENTRY_PARTICIPANT;
    if (member->loaded)
        flags |= MP_LOBBY_ENTRY_LOADED;
    return flags;
}

void lobby_snapshot(const Lobby* lobby, MpLobby* out)
{
    memset(out, 0, sizeof(*out));
    out->room = lobby->room;
    out->revision = lobby->revision;
    out->settings_revision = lobby->settings_revision;
    out->phase = lobby->phase;
    out->host_id = lobby->has_host ? lobby->host_id : UINT32_MAX;
    out->host_session = lobby->has_host ? lobby->host_session : 0;
    out->run_epoch = lobby->run_epoch;
    out->mode = mp_mode_for_map(lobby->map);
    out->compat = MP_COMPAT_ID;
    out->notice = lobby->notice;
    out->capacity = lobby->capacity;
    strcpy_s(out->map, sizeof(out->map), lobby->map);

    for (unsigned int i = 0; i < lobby->capacity; ++i)
    {
        const LobbyMember* member = &lobby->members[i];
        if (!member->present)
            continue;

        MpLobbyEntry* entry = &out->entries[out->count++];
        entry->id = i;
        entry->session = member->session;
        entry->flags = member_flags(member);
        strcpy_s(entry->name, sizeof(entry->name), member->name);
    }
}
