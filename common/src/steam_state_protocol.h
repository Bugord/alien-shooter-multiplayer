#ifndef MP_STEAM_STATE_PROTOCOL_H
#define MP_STEAM_STATE_PROTOCOL_H
#include <stdint.h>
#include <string.h>
#include <math.h>

/* Separate from legacy MpActor: explicit big-endian words, no pointers/padding. */
#define MPT_C_STEAM_STATE 0x30
#define MPT_S_STEAM_STATE 0x31
#define MP_STEAM_STATE_VERSION 1u
#define MP_STEAM_STATE_SIZE 84
#define MP_STEAM_RELAY_SIZE (8 + MP_STEAM_STATE_SIZE)
#define MP_STEAM_SEND_MS 33u
typedef struct MpSteamState {
    uint32_t sequence, active, tick;
    float x, y, z;
    int32_t health, weapon_slot, current_ammo;
    uint32_t animation, direction, stored_ammo[9];
} MpSteamState;

static inline void mp_steam_put(uint8_t* p, uint32_t n) {
    p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
}
static inline uint32_t mp_steam_get(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) | p[3];
}
static inline int mp_steam_newer(uint32_t a, uint32_t b) {
    uint32_t delta = a - b;
    return delta != 0 && delta < 0x80000000u;
}
static inline void mp_steam_encode(uint8_t p[MP_STEAM_STATE_SIZE], const MpSteamState* s) {
    uint32_t xyz[3];
    memcpy(&xyz[0], &s->x, 4); memcpy(&xyz[1], &s->y, 4); memcpy(&xyz[2], &s->z, 4);
    mp_steam_put(p, MP_STEAM_STATE_VERSION);
    mp_steam_put(p + 4, s->sequence); mp_steam_put(p + 8, s->active);
    mp_steam_put(p + 12, s->tick);
    for (int i = 0; i < 3; ++i) mp_steam_put(p + 16 + i * 4, xyz[i]);
    mp_steam_put(p + 28, (uint32_t)s->health);
    mp_steam_put(p + 32, s->animation); mp_steam_put(p + 36, s->direction);
    mp_steam_put(p + 40, (uint32_t)s->weapon_slot);
    mp_steam_put(p + 44, (uint32_t)s->current_ammo);
    for (int i = 0; i < 9; ++i) mp_steam_put(p + 48 + i * 4, s->stored_ammo[i]);
}
static inline int mp_steam_decode(const uint8_t* p, int length, MpSteamState* s) {
    if (length != MP_STEAM_STATE_SIZE || mp_steam_get(p) != MP_STEAM_STATE_VERSION) return 0;
    memset(s, 0, sizeof(*s));
    s->sequence = mp_steam_get(p + 4); s->active = mp_steam_get(p + 8);
    s->tick = mp_steam_get(p + 12);
    uint32_t bits = mp_steam_get(p + 16); memcpy(&s->x, &bits, 4);
    bits = mp_steam_get(p + 20); memcpy(&s->y, &bits, 4);
    bits = mp_steam_get(p + 24); memcpy(&s->z, &bits, 4);
    bits = mp_steam_get(p + 28); memcpy(&s->health, &bits, 4);
    s->animation = mp_steam_get(p + 32); s->direction = mp_steam_get(p + 36);
    bits = mp_steam_get(p + 40); memcpy(&s->weapon_slot, &bits, 4);
    bits = mp_steam_get(p + 44); memcpy(&s->current_ammo, &bits, 4);
    for (int i = 0; i < 9; ++i) s->stored_ammo[i] = mp_steam_get(p + 48 + i * 4);
    return s->active <= 1 && isfinite(s->x) && isfinite(s->y) && isfinite(s->z) &&
        s->weapon_slot >= -1 && s->weapon_slot < 10;
}
#endif
