#ifndef MP_MONSTER_PROTOCOL_H
#define MP_MONSTER_PROTOCOL_H
#include "protocol.h"

/* Opt-in host authority; bounded IDs for the isolated death gallery. */
#define MPT_C_MONSTER_CLAIM          0x38
#define MPT_S_MONSTER_AUTHORITY      0x39
#define MPT_C_MONSTER_STATE          0x3A
#define MPT_S_MONSTER_STATE          0x3B
#define MPT_C_MONSTER_EVENT          0x3C
#define MPT_S_MONSTER_EVENT          0x3D
#define MP_MONSTER_VERSION           4u
#define MP_MONSTER_CAPACITY          64u
#define MP_MONSTER_STATE_SIZE        84
#define MP_MONSTER_EVENT_SIZE        72
#define MP_MONSTER_STATE_CLIENT_SIZE (4 + MP_MONSTER_STATE_SIZE)
#define MP_MONSTER_EVENT_CLIENT_SIZE (4 + MP_MONSTER_EVENT_SIZE)
#define MP_MONSTER_STATE_RELAY_SIZE  (8 + MP_MONSTER_STATE_SIZE)
#define MP_MONSTER_EVENT_RELAY_SIZE  (8 + MP_MONSTER_EVENT_SIZE)
#define MP_MONSTER_EXPIRY_MS         1000u
enum MpMonsterEventKind
{
    MP_MONSTER_ATTACK = 1,
    MP_MONSTER_DEATH = 2,
    MP_MONSTER_FORM = 3
};
enum MpMonsterFormPhase
{
    MP_MONSTER_FORM_ACTIVE = 0,
    MP_MONSTER_FORM_ARMOR = 1
};
typedef struct MpMonsterState
{
    uint32_t sequence, world_low, world_high, world_epoch, monster_id, vid, tick, alive;
    float x, y, z, velocity;
    uint32_t direction, moving;
    int32_t health;
    uint32_t event_sequence, death_animation; /* Alive: 0; dead: host-selected 15 or 16. */
    uint32_t origin_vid, form_revision, form_phase;
} MpMonsterState;
typedef struct MpMonsterEvent
{
    uint32_t sequence, world_low, world_high, world_epoch, monster_id, tick, kind;
    float x, y, z;
    uint32_t direction, death_animation;
    uint32_t vid, origin_vid, form_revision, form_phase, from_vid;
} MpMonsterEvent;
static inline int mp_monster_world(uint32_t low, uint32_t high, uint32_t epoch)
{
    return (low || high) && epoch;
}
static inline int mp_monster_pose(float x, float y, float z, uint32_t direction)
{
    return isfinite(x) && isfinite(y) && isfinite(z) && direction <= 255u &&
           fabsf(x) <= 1000000.0f && fabsf(y) <= 1000000.0f && fabsf(z) <= 1000000.0f;
}
static inline int mp_monster_death_animation(uint32_t animation)
{
    return animation == 15u || animation == 16u;
}
static inline int mp_monster_origin(uint32_t vid)
{
    return vid >= 40u && vid <= 90u && vid % 10u == 0u;
}
static inline int mp_monster_frozen(uint32_t vid)
{
    return vid >= 49u && vid <= 99u && vid % 10u == 9u;
}
static inline int mp_monster_vid(uint32_t vid)
{
    return mp_monster_origin(vid) || mp_monster_frozen(vid);
}
static inline int mp_monster_reachable(uint32_t origin, uint32_t vid)
{
    if (!mp_monster_origin(origin) || !mp_monster_vid(vid))
        return 0;
    uint32_t family = mp_monster_frozen(vid) ? vid - 9u : vid;
    return family == origin || (origin == 70u && family == 60u) || (origin == 90u && family == 80u);
}
static inline int mp_monster_form_valid(uint32_t vid, uint32_t origin, uint32_t revision,
                                        uint32_t phase)
{
    return revision && mp_monster_reachable(origin, vid) &&
           (phase == MP_MONSTER_FORM_ACTIVE ||
            (phase == MP_MONSTER_FORM_ARMOR && (vid == 70u || vid == 90u)));
}
/* Snapshots may skip intermediate armor/freeze packets, but never undo a form. */
static inline int mp_monster_form_forward(uint32_t from, uint32_t from_phase, uint32_t to,
                                          uint32_t to_phase)
{
    if (from == to && from_phase == to_phase)
        return 1;
    if (mp_monster_frozen(from))
        return 0;
    if (from_phase == MP_MONSTER_FORM_ARMOR)
        return to_phase == MP_MONSTER_FORM_ACTIVE && (to == from - 10u || to == from - 1u);
    if (from_phase != MP_MONSTER_FORM_ACTIVE)
        return 0;
    if (to_phase == MP_MONSTER_FORM_ARMOR)
        return from == to && (from == 70u || from == 90u);
    return to_phase == MP_MONSTER_FORM_ACTIVE &&
           (to == from + 9u ||
            ((from == 70u || from == 90u) && (to == from - 10u || to == from - 1u)));
}
static inline int mp_monster_state_progresses(const MpMonsterState* old, const MpMonsterState* next)
{
    if (!old || old->world_low != next->world_low || old->world_high != next->world_high ||
        old->world_epoch != next->world_epoch || old->monster_id != next->monster_id)
        return 1;
    if (!mp_sequence_newer(next->sequence, old->sequence) || old->origin_vid != next->origin_vid ||
        (!old->alive && next->alive))
        return 0;
    if (old->form_revision == next->form_revision)
        return old->vid == next->vid && old->form_phase == next->form_phase;
    return old->alive && mp_sequence_newer(next->form_revision, old->form_revision) &&
           mp_monster_form_forward(old->vid, old->form_phase, next->vid, next->form_phase);
}
static inline int mp_monster_event_matches(const MpMonsterState* s, const MpMonsterEvent* e)
{
    if (s->world_low != e->world_low || s->world_high != e->world_high ||
        s->world_epoch != e->world_epoch || s->monster_id != e->monster_id ||
        s->origin_vid != e->origin_vid)
        return 0;
    if (s->form_revision == e->form_revision)
        return s->vid == e->vid && s->form_phase == e->form_phase &&
               (s->alive || e->kind == MP_MONSTER_DEATH);
    return s->alive && e->kind == MP_MONSTER_FORM &&
           mp_sequence_newer(e->form_revision, s->form_revision) &&
           mp_monster_form_forward(s->vid, s->form_phase, e->vid, e->form_phase);
}
static inline int mp_monster_id(uint32_t id)
{
    return id > 0u && id <= MP_MONSTER_CAPACITY;
}
static inline void mp_monster_state_encode(uint8_t p[MP_MONSTER_STATE_SIZE],
                                           const MpMonsterState* s)
{
    mp_put_u32(p, MP_MONSTER_VERSION);
    mp_put_u32(p + 4, s->sequence);
    mp_put_u32(p + 8, s->world_low);
    mp_put_u32(p + 12, s->world_high);
    mp_put_u32(p + 16, s->world_epoch);
    mp_put_u32(p + 20, s->monster_id);
    mp_put_u32(p + 24, s->vid);
    mp_put_u32(p + 28, s->tick);
    mp_put_u32(p + 32, s->alive);
    uint32_t bits;
    memcpy(&bits, &s->x, 4);
    mp_put_u32(p + 36, bits);
    memcpy(&bits, &s->y, 4);
    mp_put_u32(p + 40, bits);
    memcpy(&bits, &s->z, 4);
    mp_put_u32(p + 44, bits);
    memcpy(&bits, &s->velocity, 4);
    mp_put_u32(p + 48, bits);
    mp_put_u32(p + 52, s->direction);
    mp_put_u32(p + 56, s->moving);
    mp_put_u32(p + 60, (uint32_t)s->health);
    mp_put_u32(p + 64, s->event_sequence);
    mp_put_u32(p + 68, s->death_animation);
    mp_put_u32(p + 72, s->origin_vid);
    mp_put_u32(p + 76, s->form_revision);
    mp_put_u32(p + 80, s->form_phase);
}
static inline int mp_monster_state_decode(const uint8_t* p, int length, MpMonsterState* s)
{
    if (length != MP_MONSTER_STATE_SIZE || mp_get_u32(p) != MP_MONSTER_VERSION)
        return 0;
    memset(s, 0, sizeof(*s));
    s->sequence = mp_get_u32(p + 4);
    s->world_low = mp_get_u32(p + 8);
    s->world_high = mp_get_u32(p + 12);
    s->world_epoch = mp_get_u32(p + 16);
    s->monster_id = mp_get_u32(p + 20);
    s->vid = mp_get_u32(p + 24);
    s->tick = mp_get_u32(p + 28);
    s->alive = mp_get_u32(p + 32);
    uint32_t bits = mp_get_u32(p + 36);
    memcpy(&s->x, &bits, 4);
    bits = mp_get_u32(p + 40);
    memcpy(&s->y, &bits, 4);
    bits = mp_get_u32(p + 44);
    memcpy(&s->z, &bits, 4);
    bits = mp_get_u32(p + 48);
    memcpy(&s->velocity, &bits, 4);
    s->direction = mp_get_u32(p + 52);
    s->moving = mp_get_u32(p + 56);
    bits = mp_get_u32(p + 60);
    memcpy(&s->health, &bits, 4);
    s->event_sequence = mp_get_u32(p + 64);
    s->death_animation = mp_get_u32(p + 68);
    s->origin_vid = mp_get_u32(p + 72);
    s->form_revision = mp_get_u32(p + 76);
    s->form_phase = mp_get_u32(p + 80);
    return mp_monster_world(s->world_low, s->world_high, s->world_epoch) &&
           mp_monster_id(s->monster_id) && mp_monster_vid(s->vid) &&
           mp_monster_form_valid(s->vid, s->origin_vid, s->form_revision, s->form_phase) &&
           s->alive <= 1u && s->moving <= 1u &&
           (s->form_phase != MP_MONSTER_FORM_ARMOR ||
            (s->alive && !s->moving && s->velocity == 0.0f)) &&
           (s->alive ? !s->death_animation : mp_monster_death_animation(s->death_animation)) &&
           s->health >= -MP_MAX_HEALTH_LIMIT && s->health <= MP_MAX_HEALTH_LIMIT &&
           (!s->alive || s->health > 0) && mp_monster_pose(s->x, s->y, s->z, s->direction) &&
           isfinite(s->velocity) && fabsf(s->velocity) <= 1000.0f;
}
static inline void mp_monster_event_encode(uint8_t p[MP_MONSTER_EVENT_SIZE],
                                           const MpMonsterEvent* e)
{
    mp_put_u32(p, MP_MONSTER_VERSION);
    mp_put_u32(p + 4, e->sequence);
    mp_put_u32(p + 8, e->world_low);
    mp_put_u32(p + 12, e->world_high);
    mp_put_u32(p + 16, e->world_epoch);
    mp_put_u32(p + 20, e->monster_id);
    mp_put_u32(p + 24, e->tick);
    mp_put_u32(p + 28, e->kind);
    uint32_t bits;
    memcpy(&bits, &e->x, 4);
    mp_put_u32(p + 32, bits);
    memcpy(&bits, &e->y, 4);
    mp_put_u32(p + 36, bits);
    memcpy(&bits, &e->z, 4);
    mp_put_u32(p + 40, bits);
    mp_put_u32(p + 44, e->direction);
    mp_put_u32(p + 48, e->death_animation);
    mp_put_u32(p + 52, e->vid);
    mp_put_u32(p + 56, e->origin_vid);
    mp_put_u32(p + 60, e->form_revision);
    mp_put_u32(p + 64, e->form_phase);
    mp_put_u32(p + 68, e->from_vid);
}
static inline int mp_monster_event_decode(const uint8_t* p, int length, MpMonsterEvent* e)
{
    if (length != MP_MONSTER_EVENT_SIZE || mp_get_u32(p) != MP_MONSTER_VERSION)
        return 0;
    memset(e, 0, sizeof(*e));
    e->sequence = mp_get_u32(p + 4);
    e->world_low = mp_get_u32(p + 8);
    e->world_high = mp_get_u32(p + 12);
    e->world_epoch = mp_get_u32(p + 16);
    e->monster_id = mp_get_u32(p + 20);
    e->tick = mp_get_u32(p + 24);
    e->kind = mp_get_u32(p + 28);
    uint32_t bits = mp_get_u32(p + 32);
    memcpy(&e->x, &bits, 4);
    bits = mp_get_u32(p + 36);
    memcpy(&e->y, &bits, 4);
    bits = mp_get_u32(p + 40);
    memcpy(&e->z, &bits, 4);
    e->direction = mp_get_u32(p + 44);
    e->death_animation = mp_get_u32(p + 48);
    e->vid = mp_get_u32(p + 52);
    e->origin_vid = mp_get_u32(p + 56);
    e->form_revision = mp_get_u32(p + 60);
    e->form_phase = mp_get_u32(p + 64);
    e->from_vid = mp_get_u32(p + 68);
    return mp_monster_world(e->world_low, e->world_high, e->world_epoch) &&
           mp_monster_id(e->monster_id) && mp_monster_pose(e->x, e->y, e->z, e->direction) &&
           mp_monster_form_valid(e->vid, e->origin_vid, e->form_revision, e->form_phase) &&
           ((e->kind == MP_MONSTER_ATTACK && e->form_phase == MP_MONSTER_FORM_ACTIVE &&
             !mp_monster_frozen(e->vid) && !e->death_animation && !e->from_vid) ||
            (e->kind == MP_MONSTER_DEATH && e->form_phase == MP_MONSTER_FORM_ACTIVE &&
             mp_monster_death_animation(e->death_animation) && !e->from_vid) ||
            (e->kind == MP_MONSTER_FORM && mp_monster_reachable(e->origin_vid, e->from_vid) &&
             ((e->form_phase == MP_MONSTER_FORM_ARMOR && e->from_vid == e->vid &&
               e->death_animation == 15u) ||
              (e->form_phase == MP_MONSTER_FORM_ACTIVE && !e->death_animation &&
               mp_monster_origin(e->from_vid) &&
               (e->vid == e->from_vid + 9u ||
                ((e->from_vid == 70u || e->from_vid == 90u) &&
                 (e->vid == e->from_vid - 10u || e->vid == e->from_vid - 1u)))))));
}
#endif
