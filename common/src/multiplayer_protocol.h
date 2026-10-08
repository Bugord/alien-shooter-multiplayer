#ifndef MULTIPLAYER_PROTOCOL
#define MULTIPLAYER_PROTOCOL

#include <stdint.h>

#define MP_MAX_NAME_LEN              15
#define MP_MAX_MAP_NAME_LEN          24
#define MP_USER_SYNC_UPDATE_RATE_MS  10000
#define MP_ACTOR_SYNC_UPDATE_RATE_MS 30

typedef struct MpServerConfiguration
{
    uint8_t max_clients;
    uint16_t user_sync_update_rate_ms;
    uint16_t actor_sync_update_rate_ms; /* Unused; keeps the response layout. */
    char map_name[MP_MAX_MAP_NAME_LEN];
} MpServerConfiguration;

typedef struct MpUser
{
    char name[MP_MAX_NAME_LEN + 1];
} MpUser;

typedef enum MpPacketType
{
    MPT_C_CONNECTION_REQUEST = 0x10,
    MPT_S_CONNECTION_RESPONSE,
    MPT_C_USER_SYNC,
    MPT_S_USERS_SYNC,
} MpPacketType;

typedef struct MpCPacketConnectionRequest
{
    uint8_t name_len;
    char name[];
} MpCPacketConnectionRequest;

typedef struct MpSPacketConnectionResponse
{
    MpServerConfiguration server_configuration;
} MpSPacketConnectionResponse;

typedef struct MpCPacketUserSync
{
    MpUser mp_user;
} MpCPacketUserSync;

typedef struct MpSPacketUsersSyncItem
{
    uint8_t id;
    MpUser mp_user;
} MpSPacketUsersSyncItem;

typedef struct MpSPacketUsersSync
{
    uint8_t num_items;
    MpSPacketUsersSyncItem items[];
} MpSPacketUsersSync;

#endif /* MULTIPLAYER_PROTOCOL */
