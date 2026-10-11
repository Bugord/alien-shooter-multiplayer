#ifndef MP_MONSTER_VIDS_H
#define MP_MONSTER_VIDS_H
#include <stdint.h>

/* Native object IDs (VIDs) of the UNIT monsters the mod can mirror. Shared by
   the wire protocol and the game layer; this header has no other dependencies.
   The armored forms 70 and 90 transform into 60 and 80 when their armor breaks.
   A frozen copy of a monster uses its VID + 9 (49, 59, ...). */
enum MonsterVid
{
    MONSTER_VID_SLUG = 40,
    MONSTER_VID_MONSTER1 = 50,
    MONSTER_VID_MONSTER2 = 60,
    MONSTER_VID_MONSTER2_ARMORED = 70,
    MONSTER_VID_TANK = 80,
    MONSTER_VID_TANK_ARMORED = 90,
    MONSTER_VID_FIRST = MONSTER_VID_SLUG,
    MONSTER_VID_LAST = MONSTER_VID_TANK_ARMORED,
    MONSTER_VID_STEP = 10,
    MONSTER_VID_FROZEN_DELTA = 9
};

/* A VID the host can spawn: 40, 50, ..., 90. */
static inline int mp_monster_origin(uint32_t vid)
{
    return vid >= MONSTER_VID_FIRST && vid <= MONSTER_VID_LAST && vid % MONSTER_VID_STEP == 0u;
}

/* A frozen copy: 49, 59, ..., 99. */
static inline int mp_monster_frozen(uint32_t vid)
{
    return vid >= MONSTER_VID_FIRST + MONSTER_VID_FROZEN_DELTA &&
           vid <= MONSTER_VID_LAST + MONSTER_VID_FROZEN_DELTA &&
           vid % MONSTER_VID_STEP == MONSTER_VID_FROZEN_DELTA;
}

static inline int mp_monster_vid(uint32_t vid)
{
    return mp_monster_origin(vid) || mp_monster_frozen(vid);
}

/* 70 and 90 carry armor and transform when it breaks. */
static inline int mp_monster_armored(uint32_t vid)
{
    return vid == MONSTER_VID_MONSTER2_ARMORED || vid == MONSTER_VID_TANK_ARMORED;
}

/* The living form after armor breaks: 70 -> 60, anything else -> 80. Call it
   only for 70 or 90. */
static inline uint32_t mp_monster_armor_successor(uint32_t vid)
{
    return vid == MONSTER_VID_MONSTER2_ARMORED ? MONSTER_VID_MONSTER2 : MONSTER_VID_TANK;
}
#endif
