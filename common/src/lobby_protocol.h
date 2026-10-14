/* Lobby wire format shared by the relay server and the client DLL: one room snapshot
   sent by the server, and the small requests clients send (ready, settings, start,
   loaded, character). Layer: common/ (protocol). Words are big-endian; strings are
   length-prefixed ASCII without a terminator, like protocol.h. */

#ifndef MP_LOBBY_PROTOCOL_H
#define MP_LOBBY_PROTOCOL_H

#include "protocol.h"

#define MPT_S_LOBBY          0x40
#define MPT_C_LOBBY_READY    0x41
#define MPT_C_LOBBY_SETTINGS 0x42
#define MPT_C_LOBBY_START    0x43
#define MPT_C_LOBBY_LOADED   0x44
#define MPT_C_LOBBY_PROFILE  0x45

/* Version 2: every roster entry carries the player's character. */
#define MP_LOBBY_VERSION 2u

/* Every packet family a client and the server must agree on, packed into one
   word. A lobby snapshot with another value comes from an incompatible build. */
#define MP_COMPAT_ID                                                                               \
    ((MP_STATE_VERSION << 24) | (MP_SHOT_VERSION << 16) | (MP_HANDSHAKE_VERSION << 8) |            \
     MP_LOBBY_VERSION)

/* The server resends the snapshot this often even without changes. */
#define MP_LOBBY_INTERVAL_MS 1000u

/* A started game must finish loading on every participant within this time,
   otherwise the start is cancelled and the room returns to the lobby. */
#define MP_LOBBY_LOAD_TIMEOUT_MS 30000u

#define MP_LOBBY_REQUEST_SIZE     16
#define MP_LOBBY_START_SIZE       12
#define MP_LOBBY_LOADED_SIZE      12
#define MP_LOBBY_PROFILE_SIZE     12
#define MP_LOBBY_HEADER_SIZE      48
#define MP_LOBBY_ENTRY_WORDS_SIZE 16 /* id, session, flags, character */
#define MP_LOBBY_MAX_SIZE                                                                          \
    (MP_LOBBY_HEADER_SIZE + 1 + MP_MAX_MAP_NAME_LEN + 4 +                                          \
     MP_ROSTER_MAX_ENTRIES * (MP_LOBBY_ENTRY_WORDS_SIZE + 1 + MP_MAX_NAME_LEN))

enum MpLobbyPhase
{
    MP_LOBBY_OPEN = 1,     /* Players join, set ready; the host may start. */
    MP_LOBBY_STARTING = 2, /* Participants are loading the map. */
    MP_LOBBY_IN_GAME = 3,  /* Every participant loaded; gameplay is allowed. */
    MP_LOBBY_CLOSED = 4    /* The host left; the room will not continue. */
};

/* Why the last start was cancelled, shown until the next change of the room. */
enum MpLobbyNotice
{
    MP_LOBBY_NOTICE_NONE = 0,
    MP_LOBBY_NOTICE_LOAD_TIMEOUT = 1,
    MP_LOBBY_NOTICE_PLAYER_LEFT = 2
};

/* The mode follows the map file name: level_*.map is campaign, survive_*.map survival. */
enum MpGameMode
{
    MP_MODE_CAMPAIGN = 1,
    MP_MODE_SURVIVAL = 2
};

/* Per-player flags in a snapshot entry. */
enum MpLobbyEntryFlag
{
    MP_LOBBY_ENTRY_READY = 1,
    MP_LOBBY_ENTRY_PARTICIPANT = 2, /* Part of the current start or game. */
    MP_LOBBY_ENTRY_LOADED = 4       /* Participant that reported its map as loaded. */
};
#define MP_LOBBY_ENTRY_FLAGS_ALL                                                                   \
    (MP_LOBBY_ENTRY_READY | MP_LOBBY_ENTRY_PARTICIPANT | MP_LOBBY_ENTRY_LOADED)

/* The hero a player picked in the room: the game's PlayerSex "male" or "female". */
enum MpCharacter
{
    MP_CHARACTER_MALE = 0,
    MP_CHARACTER_FEMALE = 1,
    MP_CHARACTER_COUNT = 2
};

typedef struct MpLobbyEntry
{
    uint32_t id;
    uint32_t session;
    uint32_t flags;
    uint32_t character; /* enum MpCharacter */
    char name[MP_MAX_NAME_LEN + 1];
} MpLobbyEntry;

typedef struct MpLobby
{
    uint32_t room;              /* Random per room; never 0. */
    uint32_t revision;          /* Grows with every visible change. */
    uint32_t settings_revision; /* Grows when mode or map change; resets ready. */
    uint32_t phase;             /* enum MpLobbyPhase */
    uint32_t host_id;
    uint32_t host_session;
    uint32_t run_epoch; /* Grows with every start; 0 before the first one. */
    uint32_t mode;      /* enum MpGameMode */
    uint32_t compat;    /* MP_COMPAT_ID of the server build */
    uint32_t notice;    /* enum MpLobbyNotice */
    uint32_t capacity;
    char map[MP_MAX_MAP_NAME_LEN];
    unsigned int count;
    MpLobbyEntry entries[MP_ROSTER_MAX_ENTRIES];
} MpLobby;

/* Client -> server requests, already decoded. */
typedef struct MpLobbyReady
{
    uint32_t room;
    uint32_t settings_revision;
    uint32_t ready;
} MpLobbyReady;

typedef struct MpLobbySettings
{
    uint32_t room;
    uint32_t settings_revision;
    char map[MP_MAX_MAP_NAME_LEN];
} MpLobbySettings;

typedef struct MpLobbyStart
{
    uint32_t room;
    uint32_t revision;
} MpLobbyStart;

typedef struct MpLobbyLoaded
{
    uint32_t room;
    uint32_t run_epoch;
} MpLobbyLoaded;

typedef struct MpLobbyProfile
{
    uint32_t room;
    uint32_t character; /* enum MpCharacter */
} MpLobbyProfile;

/* Returns the mode for a valid level path, or 0. */
static inline uint32_t mp_mode_for_map(const char* map)
{
    if (!mp_is_level_path(map))
        return 0;

    const char* file = map;
    for (const char* p = map; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
            file = p + 1;
    }

    char first = file[0];
    if (first == 's' || first == 'S')
        return MP_MODE_SURVIVAL;

    return MP_MODE_CAMPAIGN;
}

/* Finds the entry of player `id`, or NULL. */
static inline const MpLobbyEntry* mp_lobby_entry(const MpLobby* lobby, uint32_t id)
{
    for (unsigned int i = 0; i < lobby->count; ++i)
    {
        if (lobby->entries[i].id == id)
            return &lobby->entries[i];
    }
    return NULL;
}

/* Checks the rules that hold for every snapshot the server may send. */
static inline int mp_lobby_valid(const MpLobby* lobby)
{
    int started = lobby->phase == MP_LOBBY_STARTING || lobby->phase == MP_LOBBY_IN_GAME;

    if (!lobby->room || lobby->phase < MP_LOBBY_OPEN || lobby->phase > MP_LOBBY_CLOSED)
        return 0;
    if (lobby->notice > MP_LOBBY_NOTICE_PLAYER_LEFT)
        return 0;
    if (lobby->capacity < 1 || lobby->capacity > MP_ROSTER_MAX_ENTRIES)
        return 0;
    if (lobby->count > lobby->capacity)
        return 0;
    if (!lobby->settings_revision || lobby->mode != mp_mode_for_map(lobby->map))
        return 0;
    if (started && !lobby->run_epoch)
        return 0;

    uint32_t seen = 0;
    for (unsigned int i = 0; i < lobby->count; ++i)
    {
        const MpLobbyEntry* entry = &lobby->entries[i];
        uint32_t participant = entry->flags & MP_LOBBY_ENTRY_PARTICIPANT;

        if (entry->id >= MP_ROSTER_MAX_ENTRIES || (seen & (1u << entry->id)) || !entry->session)
            return 0;
        if (entry->flags & ~(uint32_t)MP_LOBBY_ENTRY_FLAGS_ALL)
            return 0;
        if ((entry->flags & MP_LOBBY_ENTRY_LOADED) && !participant)
            return 0;
        if (participant && !started)
            return 0;
        if (entry->character >= MP_CHARACTER_COUNT)
            return 0;
        if (!mp_is_player_name(entry->name))
            return 0;
        seen |= 1u << entry->id;
    }

    /* An open or running room always contains its host. */
    if (lobby->phase != MP_LOBBY_CLOSED)
    {
        const MpLobbyEntry* host = mp_lobby_entry(lobby, lobby->host_id);
        if (!host || host->session != lobby->host_session)
            return 0;
    }
    return 1;
}

/* Writes a length-prefixed string; returns the bytes used, or 0 when it does not fit. */
static inline size_t mp_write_string(uint8_t* out, size_t capacity, const char* text)
{
    size_t length = strlen(text);
    if (length > 255 || capacity < length + 1)
        return 0;
    out[0] = (uint8_t)length;
    memcpy(out + 1, text, length);
    return length + 1;
}

/* Server -> client snapshot. Returns the packet length, or 0 for an invalid lobby. */
static inline int mp_lobby_encode(uint8_t* out, size_t capacity, const MpLobby* lobby)
{
    if (!mp_lobby_valid(lobby) || capacity < MP_LOBBY_HEADER_SIZE)
        return 0;

    mp_put_u32(out, MP_LOBBY_VERSION);
    mp_put_u32(out + 4, lobby->room);
    mp_put_u32(out + 8, lobby->revision);
    mp_put_u32(out + 12, lobby->settings_revision);
    mp_put_u32(out + 16, lobby->phase);
    mp_put_u32(out + 20, lobby->host_id);
    mp_put_u32(out + 24, lobby->host_session);
    mp_put_u32(out + 28, lobby->run_epoch);
    mp_put_u32(out + 32, lobby->mode);
    mp_put_u32(out + 36, lobby->compat);
    mp_put_u32(out + 40, lobby->notice);
    mp_put_u32(out + 44, lobby->capacity);

    size_t at = MP_LOBBY_HEADER_SIZE;
    size_t used = mp_write_string(out + at, capacity - at, lobby->map);
    if (!used)
        return 0;
    at += used;

    if (capacity < at + 4)
        return 0;
    mp_put_u32(out + at, lobby->count);
    at += 4;

    for (unsigned int i = 0; i < lobby->count; ++i)
    {
        const MpLobbyEntry* entry = &lobby->entries[i];
        if (capacity < at + MP_LOBBY_ENTRY_WORDS_SIZE)
            return 0;
        mp_put_u32(out + at, entry->id);
        mp_put_u32(out + at + 4, entry->session);
        mp_put_u32(out + at + 8, entry->flags);
        mp_put_u32(out + at + 12, entry->character);
        at += MP_LOBBY_ENTRY_WORDS_SIZE;

        used = mp_write_string(out + at, capacity - at, entry->name);
        if (!used)
            return 0;
        at += used;
    }
    return (int)at;
}

/* Decodes and validates a snapshot. Returns 1 on success. */
static inline int mp_lobby_decode(const uint8_t* p, int length, MpLobby* lobby)
{
    if (length < MP_LOBBY_HEADER_SIZE || mp_get_u32(p) != MP_LOBBY_VERSION)
        return 0;

    memset(lobby, 0, sizeof(*lobby));
    lobby->room = mp_get_u32(p + 4);
    lobby->revision = mp_get_u32(p + 8);
    lobby->settings_revision = mp_get_u32(p + 12);
    lobby->phase = mp_get_u32(p + 16);
    lobby->host_id = mp_get_u32(p + 20);
    lobby->host_session = mp_get_u32(p + 24);
    lobby->run_epoch = mp_get_u32(p + 28);
    lobby->mode = mp_get_u32(p + 32);
    lobby->compat = mp_get_u32(p + 36);
    lobby->notice = mp_get_u32(p + 40);
    lobby->capacity = mp_get_u32(p + 44);

    size_t total = (size_t)length;
    size_t at = MP_LOBBY_HEADER_SIZE;
    size_t used;
    if (!mp_read_string(p + at, total - at, lobby->map, sizeof(lobby->map), &used))
        return 0;
    at += used;

    if (total < at + 4)
        return 0;
    uint32_t count = mp_get_u32(p + at);
    at += 4;
    if (count > MP_ROSTER_MAX_ENTRIES)
        return 0;

    for (uint32_t i = 0; i < count; ++i)
    {
        MpLobbyEntry* entry = &lobby->entries[i];
        if (total < at + MP_LOBBY_ENTRY_WORDS_SIZE)
            return 0;
        entry->id = mp_get_u32(p + at);
        entry->session = mp_get_u32(p + at + 4);
        entry->flags = mp_get_u32(p + at + 8);
        entry->character = mp_get_u32(p + at + 12);
        at += MP_LOBBY_ENTRY_WORDS_SIZE;

        if (!mp_read_string(p + at, total - at, entry->name, sizeof(entry->name), &used))
            return 0;
        at += used;
    }
    lobby->count = count;

    return at == total && mp_lobby_valid(lobby);
}

/* Client -> server: set or clear ready for the given settings revision. */
static inline void mp_lobby_ready_encode(uint8_t out[MP_LOBBY_REQUEST_SIZE],
                                         const MpLobbyReady* ready)
{
    mp_put_u32(out, MP_LOBBY_VERSION);
    mp_put_u32(out + 4, ready->room);
    mp_put_u32(out + 8, ready->settings_revision);
    mp_put_u32(out + 12, ready->ready ? 1u : 0u);
}

static inline int mp_lobby_ready_decode(const uint8_t* p, int length, MpLobbyReady* ready)
{
    if (length != MP_LOBBY_REQUEST_SIZE || mp_get_u32(p) != MP_LOBBY_VERSION)
        return 0;
    ready->room = mp_get_u32(p + 4);
    ready->settings_revision = mp_get_u32(p + 8);
    ready->ready = mp_get_u32(p + 12);
    return ready->room && ready->ready <= 1u;
}

/* Client (host only) -> server: change the map; the mode follows from it. */
static inline int mp_lobby_settings_encode(uint8_t* out, size_t capacity,
                                           const MpLobbySettings* settings)
{
    if (!mp_mode_for_map(settings->map) || capacity < 12)
        return 0;
    mp_put_u32(out, MP_LOBBY_VERSION);
    mp_put_u32(out + 4, settings->room);
    mp_put_u32(out + 8, settings->settings_revision);
    size_t used = mp_write_string(out + 12, capacity - 12, settings->map);
    if (!used)
        return 0;
    return (int)(12 + used);
}

static inline int mp_lobby_settings_decode(const uint8_t* p, int length, MpLobbySettings* settings)
{
    size_t used;
    if (length < 13 || mp_get_u32(p) != MP_LOBBY_VERSION)
        return 0;
    settings->room = mp_get_u32(p + 4);
    settings->settings_revision = mp_get_u32(p + 8);
    if (!mp_read_string(p + 12, (size_t)length - 12, settings->map, sizeof(settings->map), &used) ||
        used != (size_t)length - 12)
        return 0;
    return settings->room && mp_mode_for_map(settings->map);
}

/* Client (host only) -> server: start the room exactly as seen at `revision`. */
static inline void mp_lobby_start_encode(uint8_t out[MP_LOBBY_START_SIZE],
                                         const MpLobbyStart* start)
{
    mp_put_u32(out, MP_LOBBY_VERSION);
    mp_put_u32(out + 4, start->room);
    mp_put_u32(out + 8, start->revision);
}

static inline int mp_lobby_start_decode(const uint8_t* p, int length, MpLobbyStart* start)
{
    if (length != MP_LOBBY_START_SIZE || mp_get_u32(p) != MP_LOBBY_VERSION)
        return 0;
    start->room = mp_get_u32(p + 4);
    start->revision = mp_get_u32(p + 8);
    return start->room != 0;
}

/* Client -> server: this participant loaded the map of `run_epoch`. */
static inline void mp_lobby_loaded_encode(uint8_t out[MP_LOBBY_LOADED_SIZE],
                                          const MpLobbyLoaded* loaded)
{
    mp_put_u32(out, MP_LOBBY_VERSION);
    mp_put_u32(out + 4, loaded->room);
    mp_put_u32(out + 8, loaded->run_epoch);
}

static inline int mp_lobby_loaded_decode(const uint8_t* p, int length, MpLobbyLoaded* loaded)
{
    if (length != MP_LOBBY_LOADED_SIZE || mp_get_u32(p) != MP_LOBBY_VERSION)
        return 0;
    loaded->room = mp_get_u32(p + 4);
    loaded->run_epoch = mp_get_u32(p + 8);
    return loaded->room && loaded->run_epoch;
}

/* Client -> server: this player picked another character. */
static inline void mp_lobby_profile_encode(uint8_t out[MP_LOBBY_PROFILE_SIZE],
                                           const MpLobbyProfile* profile)
{
    mp_put_u32(out, MP_LOBBY_VERSION);
    mp_put_u32(out + 4, profile->room);
    mp_put_u32(out + 8, profile->character);
}

static inline int mp_lobby_profile_decode(const uint8_t* p, int length, MpLobbyProfile* profile)
{
    if (length != MP_LOBBY_PROFILE_SIZE || mp_get_u32(p) != MP_LOBBY_VERSION)
        return 0;
    profile->room = mp_get_u32(p + 4);
    profile->character = mp_get_u32(p + 8);
    return profile->room && profile->character < MP_CHARACTER_COUNT;
}

#endif
