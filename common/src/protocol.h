#ifndef MP_PROTOCOL_H
#define MP_PROTOCOL_H
#include <stdint.h>
#include <string.h>
#include <math.h>

/* Explicit big-endian words, no pointers/padding. */
#define MPT_C_STATE 0x30
#define MPT_S_STATE 0x31
#define MP_STATE_VERSION 3u
#define MP_STATE_SIZE 112
#define MP_STATE_RELAY_SIZE (8 + MP_STATE_SIZE)
#define MP_STATE_SEND_MS 33u
typedef struct MpState {
    uint32_t sequence, active, tick;
    float x, y, z;
    int32_t health, weapon_slot, current_ammo;
    uint32_t animation, direction, stored_ammo[9];
    float velocity;
    uint32_t moving, torso_direction, torso_present;
    uint32_t world_low, world_high, world_epoch;
} MpState;

static inline void mp_put_u32(uint8_t* p, uint32_t n) {
    p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
}
static inline uint32_t mp_get_u32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) | p[3];
}
static inline int mp_sequence_newer(uint32_t a, uint32_t b) {
    uint32_t delta = a - b;
    return delta != 0 && delta < 0x80000000u;
}
static inline void mp_state_encode(uint8_t p[MP_STATE_SIZE], const MpState* s) {
    uint32_t xyz[3];
    memcpy(&xyz[0], &s->x, 4); memcpy(&xyz[1], &s->y, 4); memcpy(&xyz[2], &s->z, 4);
    mp_put_u32(p, MP_STATE_VERSION);
    mp_put_u32(p + 4, s->sequence); mp_put_u32(p + 8, s->active);
    mp_put_u32(p + 12, s->tick);
    for (int i = 0; i < 3; ++i) mp_put_u32(p + 16 + i * 4, xyz[i]);
    mp_put_u32(p + 28, (uint32_t)s->health);
    mp_put_u32(p + 32, s->animation); mp_put_u32(p + 36, s->direction);
    mp_put_u32(p + 40, (uint32_t)s->weapon_slot);
    mp_put_u32(p + 44, (uint32_t)s->current_ammo);
    for (int i = 0; i < 9; ++i) mp_put_u32(p + 48 + i * 4, s->stored_ammo[i]);
    uint32_t velocity;
    memcpy(&velocity, &s->velocity, 4);
    mp_put_u32(p + 84, velocity);
    mp_put_u32(p + 88, s->moving);
    mp_put_u32(p + 92, s->torso_direction);
    mp_put_u32(p + 96, s->torso_present);
    mp_put_u32(p + 100, s->world_low); mp_put_u32(p + 104, s->world_high);
    mp_put_u32(p + 108, s->world_epoch);
}
static inline int mp_state_decode(const uint8_t* p, int length, MpState* s) {
    if (length != MP_STATE_SIZE || mp_get_u32(p) != MP_STATE_VERSION) return 0;
    memset(s, 0, sizeof(*s));
    s->sequence = mp_get_u32(p + 4); s->active = mp_get_u32(p + 8);
    s->tick = mp_get_u32(p + 12);
    uint32_t bits = mp_get_u32(p + 16); memcpy(&s->x, &bits, 4);
    bits = mp_get_u32(p + 20); memcpy(&s->y, &bits, 4);
    bits = mp_get_u32(p + 24); memcpy(&s->z, &bits, 4);
    bits = mp_get_u32(p + 28); memcpy(&s->health, &bits, 4);
    s->animation = mp_get_u32(p + 32); s->direction = mp_get_u32(p + 36);
    bits = mp_get_u32(p + 40); memcpy(&s->weapon_slot, &bits, 4);
    bits = mp_get_u32(p + 44); memcpy(&s->current_ammo, &bits, 4);
    for (int i = 0; i < 9; ++i) s->stored_ammo[i] = mp_get_u32(p + 48 + i * 4);
    bits = mp_get_u32(p + 84); memcpy(&s->velocity, &bits, 4);
    s->moving = mp_get_u32(p + 88);
    s->torso_direction = mp_get_u32(p + 92);
    s->torso_present = mp_get_u32(p + 96);
    s->world_low = mp_get_u32(p + 100); s->world_high = mp_get_u32(p + 104);
    s->world_epoch = mp_get_u32(p + 108);
    return s->active <= 1 && isfinite(s->x) && isfinite(s->y) && isfinite(s->z) &&
        s->weapon_slot >= -1 && s->weapon_slot < 10 && s->torso_present <= 1 &&
        s->direction <= 255u && s->torso_direction <= 255u && s->moving <= 1 && isfinite(s->velocity);
}
/* Normalize only ASCII game filenames; / and \\ are equivalent. */
static inline int mp_is_level_path(const char* path) {
    if (!path || !*path || strlen(path) >= 128u || strstr(path, "..")) return 0;
    const char* file = path;
    for (const char* p = path; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c > 126 || c == ':') return 0;
        if (c == '/' || c == '\\') file = p + 1;
    }
    char normalized[128]; size_t i;
    for (i = 0; file[i]; ++i) normalized[i] = file[i] >= 'A' && file[i] <= 'Z' ? (char)(file[i] + 'a' - 'A') : file[i];
    normalized[i] = 0;
    return i > 10u && (!strncmp(normalized, "level_", 6) || !strncmp(normalized, "survive_", 8)) &&
        !strcmp(normalized + i - 4, ".map");
}
static inline void mp_world_key(const char* path, uint32_t* low, uint32_t* high) {
    uint64_t key = UINT64_C(14695981039346656037);
    while (*path) {
        unsigned char c = (unsigned char)*path++;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 'a' - 'A');
        if (c == '\\') c = '/';
        key = (key ^ c) * UINT64_C(1099511628211);
    }
    *low = (uint32_t)key; *high = (uint32_t)(key >> 32);
}
#define MPT_C_SHOT 0x32
#define MPT_S_SHOT 0x33
#define MP_SHOT_SIZE 32
#define MP_SHOT_RELAY_SIZE (8 + MP_SHOT_SIZE)
typedef struct MpShot {
    uint32_t sequence, world_low, world_high, world_epoch, weapon;
    int32_t x, y;
} MpShot;
static inline void mp_shot_encode(uint8_t p[MP_SHOT_SIZE], const MpShot* s) {
    mp_put_u32(p, MP_STATE_VERSION); mp_put_u32(p + 4, s->sequence);
    mp_put_u32(p + 8, s->world_low); mp_put_u32(p + 12, s->world_high);
    mp_put_u32(p + 16, s->world_epoch); mp_put_u32(p + 20, s->weapon);
    mp_put_u32(p + 24, (uint32_t)s->x); mp_put_u32(p + 28, (uint32_t)s->y);
}
static inline int mp_shot_decode(const uint8_t* p, int length, MpShot* s) {
    if (length != MP_SHOT_SIZE || mp_get_u32(p) != MP_STATE_VERSION) return 0;
    memset(s, 0, sizeof(*s));
    s->sequence = mp_get_u32(p + 4); s->world_low = mp_get_u32(p + 8);
    s->world_high = mp_get_u32(p + 12); s->world_epoch = mp_get_u32(p + 16);
    s->weapon = mp_get_u32(p + 20);
    uint32_t bits = mp_get_u32(p + 24); memcpy(&s->x, &bits, 4);
    bits = mp_get_u32(p + 28); memcpy(&s->y, &bits, 4);
    /* Actual game maps fit far inside these bounds; keep native integer aim
       conversions away from overflow for received events. */
    return s->weapon < 10u && s->world_epoch && (s->world_low || s->world_high) &&
        s->x >= -1000000 && s->x <= 1000000 && s->y >= -1000000 && s->y <= 1000000;
}
#endif
