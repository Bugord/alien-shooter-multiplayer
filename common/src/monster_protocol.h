#ifndef MP_MONSTER_PROTOCOL_H
#define MP_MONSTER_PROTOCOL_H
#include "protocol.h"

/* Opt-in host authority; bounded IDs for the isolated death gallery. */
#define MPT_C_MONSTER_CLAIM 0x38
#define MPT_S_MONSTER_AUTHORITY 0x39
#define MPT_C_MONSTER_STATE 0x3A
#define MPT_S_MONSTER_STATE 0x3B
#define MPT_C_MONSTER_EVENT 0x3C
#define MPT_S_MONSTER_EVENT 0x3D
#define MP_MONSTER_VERSION 3u
#define MP_MONSTER_CAPACITY 64u
#define MP_MONSTER_STATE_SIZE 72
#define MP_MONSTER_EVENT_SIZE 52
#define MP_MONSTER_STATE_CLIENT_SIZE (4 + MP_MONSTER_STATE_SIZE)
#define MP_MONSTER_EVENT_CLIENT_SIZE (4 + MP_MONSTER_EVENT_SIZE)
#define MP_MONSTER_STATE_RELAY_SIZE (8 + MP_MONSTER_STATE_SIZE)
#define MP_MONSTER_EVENT_RELAY_SIZE (8 + MP_MONSTER_EVENT_SIZE)
#define MP_MONSTER_EXPIRY_MS 1000u
enum MpMonsterEventKind { MP_MONSTER_ATTACK = 1, MP_MONSTER_DEATH = 2 };
typedef struct MpMonsterState {
    uint32_t sequence, world_low, world_high, world_epoch, monster_id, vid, tick, alive;
    float x, y, z, velocity;
    uint32_t direction, moving;
    int32_t health;
    uint32_t event_sequence, death_animation; /* Alive: 0; dead: host-selected 15 or 16. */
} MpMonsterState;
typedef struct MpMonsterEvent {
    uint32_t sequence, world_low, world_high, world_epoch, monster_id, tick, kind;
    float x, y, z;
    uint32_t direction, death_animation;
} MpMonsterEvent;
static inline int mp_monster_world(uint32_t low, uint32_t high, uint32_t epoch) {
    return (low || high) && epoch;
}
static inline int mp_monster_pose(float x, float y, float z, uint32_t direction) {
    return isfinite(x) && isfinite(y) && isfinite(z) && direction <= 255u &&
        fabsf(x) <= 1000000.0f && fabsf(y) <= 1000000.0f && fabsf(z) <= 1000000.0f;
}
static inline int mp_monster_death_animation(uint32_t animation) { return animation == 15u || animation == 16u; }
static inline int mp_monster_vid(uint32_t vid) { return vid == 40u || vid == 50u || vid == 60u || vid == 80u; }
static inline int mp_monster_id(uint32_t id) { return id > 0u && id <= MP_MONSTER_CAPACITY; }
static inline void mp_monster_state_encode(uint8_t p[MP_MONSTER_STATE_SIZE], const MpMonsterState* s) {
    mp_put_u32(p, MP_MONSTER_VERSION); mp_put_u32(p + 4, s->sequence);
    mp_put_u32(p + 8, s->world_low); mp_put_u32(p + 12, s->world_high); mp_put_u32(p + 16, s->world_epoch);
    mp_put_u32(p + 20, s->monster_id); mp_put_u32(p + 24, s->vid); mp_put_u32(p + 28, s->tick);
    mp_put_u32(p + 32, s->alive);
    uint32_t bits;
    memcpy(&bits, &s->x, 4); mp_put_u32(p + 36, bits);
    memcpy(&bits, &s->y, 4); mp_put_u32(p + 40, bits);
    memcpy(&bits, &s->z, 4); mp_put_u32(p + 44, bits);
    memcpy(&bits, &s->velocity, 4); mp_put_u32(p + 48, bits);
    mp_put_u32(p + 52, s->direction); mp_put_u32(p + 56, s->moving);
    mp_put_u32(p + 60, (uint32_t)s->health); mp_put_u32(p + 64, s->event_sequence);
    mp_put_u32(p + 68, s->death_animation);
}
static inline int mp_monster_state_decode(const uint8_t* p, int length, MpMonsterState* s) {
    if (length != MP_MONSTER_STATE_SIZE || mp_get_u32(p) != MP_MONSTER_VERSION) return 0;
    memset(s, 0, sizeof(*s));
    s->sequence = mp_get_u32(p + 4); s->world_low = mp_get_u32(p + 8); s->world_high = mp_get_u32(p + 12);
    s->world_epoch = mp_get_u32(p + 16); s->monster_id = mp_get_u32(p + 20); s->vid = mp_get_u32(p + 24);
    s->tick = mp_get_u32(p + 28); s->alive = mp_get_u32(p + 32);
    uint32_t bits = mp_get_u32(p + 36); memcpy(&s->x, &bits, 4);
    bits = mp_get_u32(p + 40); memcpy(&s->y, &bits, 4);
    bits = mp_get_u32(p + 44); memcpy(&s->z, &bits, 4);
    bits = mp_get_u32(p + 48); memcpy(&s->velocity, &bits, 4);
    s->direction = mp_get_u32(p + 52); s->moving = mp_get_u32(p + 56);
    bits = mp_get_u32(p + 60); memcpy(&s->health, &bits, 4); s->event_sequence = mp_get_u32(p + 64);
    s->death_animation = mp_get_u32(p + 68);
    return mp_monster_world(s->world_low, s->world_high, s->world_epoch) && mp_monster_id(s->monster_id) && mp_monster_vid(s->vid) &&
        s->alive <= 1u && s->moving <= 1u &&
        (s->alive ? !s->death_animation : mp_monster_death_animation(s->death_animation)) &&
        s->health >= -MP_MAX_HEALTH_LIMIT && s->health <= MP_MAX_HEALTH_LIMIT && (!s->alive || s->health > 0) &&
        mp_monster_pose(s->x, s->y, s->z, s->direction) && isfinite(s->velocity) &&
        fabsf(s->velocity) <= 1000.0f;
}
static inline void mp_monster_event_encode(uint8_t p[MP_MONSTER_EVENT_SIZE], const MpMonsterEvent* e) {
    mp_put_u32(p, MP_MONSTER_VERSION); mp_put_u32(p + 4, e->sequence);
    mp_put_u32(p + 8, e->world_low); mp_put_u32(p + 12, e->world_high); mp_put_u32(p + 16, e->world_epoch);
    mp_put_u32(p + 20, e->monster_id); mp_put_u32(p + 24, e->tick); mp_put_u32(p + 28, e->kind);
    uint32_t bits;
    memcpy(&bits, &e->x, 4); mp_put_u32(p + 32, bits);
    memcpy(&bits, &e->y, 4); mp_put_u32(p + 36, bits);
    memcpy(&bits, &e->z, 4); mp_put_u32(p + 40, bits);
    mp_put_u32(p + 44, e->direction); mp_put_u32(p + 48, e->death_animation);
}
static inline int mp_monster_event_decode(const uint8_t* p, int length, MpMonsterEvent* e) {
    if (length != MP_MONSTER_EVENT_SIZE || mp_get_u32(p) != MP_MONSTER_VERSION) return 0;
    memset(e, 0, sizeof(*e)); e->sequence = mp_get_u32(p + 4);
    e->world_low = mp_get_u32(p + 8); e->world_high = mp_get_u32(p + 12); e->world_epoch = mp_get_u32(p + 16);
    e->monster_id = mp_get_u32(p + 20); e->tick = mp_get_u32(p + 24); e->kind = mp_get_u32(p + 28);
    uint32_t bits = mp_get_u32(p + 32); memcpy(&e->x, &bits, 4);
    bits = mp_get_u32(p + 36); memcpy(&e->y, &bits, 4);
    bits = mp_get_u32(p + 40); memcpy(&e->z, &bits, 4);
    e->direction = mp_get_u32(p + 44); e->death_animation = mp_get_u32(p + 48);
    return mp_monster_world(e->world_low, e->world_high, e->world_epoch) && mp_monster_id(e->monster_id) &&
        mp_monster_pose(e->x, e->y, e->z, e->direction) &&
        ((e->kind == MP_MONSTER_ATTACK && !e->death_animation) ||
         (e->kind == MP_MONSTER_DEATH && mp_monster_death_animation(e->death_animation)));
}
#endif
