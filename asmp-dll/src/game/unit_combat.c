#include <string.h>
#include <float.h>
#include "unit_combat.h"

enum CombatFixtureState { COMBAT_FIXTURE_OFF, COMBAT_FIXTURE_READY, COMBAT_FIXTURE_OWNED,
    COMBAT_FIXTURE_FAULT, COMBAT_FIXTURE_ABANDONED };
typedef struct CombatSlot {
    uintptr_t game, root, child, source, part_source;
    unsigned int source_index, part_index;
    int host;
    unsigned char root_vid[STEAM_VID_SIZE], child_vid[STEAM_VID_SIZE];
    unsigned char source_before[STEAM_VID_SIZE], part_before[STEAM_VID_SIZE];
    enum CombatFixtureState state;
    struct { uintptr_t entity, vid; } baseline[2048];
    unsigned int baseline_count;
    int capture_baseline;
} CombatSlot;
static CombatSlot fixtures[UNIT_COMBAT_CAPACITY];
static unsigned int combat_slot;
#define fixture (fixtures[combat_slot])
static struct {
    uintptr_t base;
    ActorCreate create;
    ActorDestroy destroy;
    UnitAnimation animation;
    int initialized;
} shared;
int unit_combat_select(unsigned int slot) {
    if (slot >= UNIT_COMBAT_CAPACITY) return 0;
    combat_slot = slot; return 1;
}
static int owned_descriptor(uintptr_t vid) {
    for (unsigned int i = 0; i < UNIT_COMBAT_CAPACITY; ++i)
        if (vid == (uintptr_t)fixtures[i].root_vid || vid == (uintptr_t)fixtures[i].child_vid) return 1;
    return 0;
}

int unit_combat_bind(uintptr_t base, ActorCreate* out) {
    static const unsigned char signature[] = {0x55,0x8B,0xEC,0x6A,0xFF};
    *out = NULL;
    __try {
        if (memcmp((void*)(base + STEAM_ACTOR_CREATE_RVA), signature, sizeof(signature)) ||
            *(uintptr_t*)(base + STEAM_GAME_VTABLE_RVA + 8u * sizeof(uintptr_t)) != base + STEAM_ACTOR_CREATE_RVA) return 0;
        *out = (ActorCreate)(base + STEAM_ACTOR_CREATE_RVA); return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
int unit_combat_init(uintptr_t base, ActorCreate create, ActorDestroy destroy, UnitAnimation animation) {
    if (shared.initialized || !base || !create || !destroy || !animation) return 0;
    memset(fixtures, 0, sizeof(fixtures)); combat_slot = 0;
    shared.base = base; shared.create = create; shared.destroy = destroy;
    shared.animation = animation; shared.initialized = 1;
    for (unsigned int i = 0; i < UNIT_COMBAT_CAPACITY; ++i) fixtures[i].state = COMBAT_FIXTURE_READY;
    return 1;
}
static int supported(uintptr_t vid, unsigned int index, unsigned int class_id) {
    return vid && *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET) == index &&
        *(unsigned int*)(vid + STEAM_VID_CLASS_OFFSET) == class_id &&
        *(unsigned int*)(vid + STEAM_VID_CATEGORY_OFFSET) < STEAM_TICK_ENTITY_LIST_COUNT &&
        !*(uintptr_t*)(vid + STEAM_VID_SERVICE_OFFSET) &&
        !(*(unsigned int*)(vid + STEAM_VID_RESOURCE_FLAGS_OFFSET) & STEAM_VID_SPECIAL_FACTORY_FLAG);
}
static void sanitize(unsigned char* destination, uintptr_t source) {
    memcpy(destination, (void*)source, STEAM_VID_SIZE);
    memset(destination + STEAM_VID_COUNTERS_OFFSET, 0, STEAM_VID_COUNTERS_SIZE);
    memset(destination + STEAM_VID_CREATIONS_OFFSET, 0, STEAM_ANIMATION_COUNT * 4u);
    for (unsigned int i = 0; i < STEAM_ANIMATION_COUNT; ++i)
        *(int*)(destination + STEAM_VID_ANIMATION_SCRIPTS_OFFSET + i * 4u) = -1;
    *(int*)(destination + STEAM_VID_DELETE_SCRIPT_OFFSET) = -1;
    *(int*)(destination + STEAM_VID_DAMAGE_SCRIPT_OFFSET) = -1;
    *(int*)(destination + STEAM_VID_COLLISION_SCRIPT_OFFSET) = -1;
    *(int*)(destination + STEAM_VID_DAMAGE_OFFSET) = 0;
}
int unit_combat_spawn_type(const Snapshot* local, unsigned int index, const UnitPuppetPose* pose, int host) {
    if (fixture.state != COMBAT_FIXTURE_READY || fixture.game || !local || !local->game || !local->in_level || local->health <= 0) return 0;
    if (index >= 1024u || !pose || !_finite(pose->x) || !_finite(pose->y) || !_finite(pose->z) || pose->direction > 255u) return 0;
    __try {
        if (*(uintptr_t*)local->game != shared.base + STEAM_GAME_VTABLE_RVA) return 0;
        uintptr_t source = *(uintptr_t*)(local->game + STEAM_GAME_VID_TABLE_OFFSET + index * 4u);
        if (!supported(source, index, STEAM_UNIT_CLASS)) return 0;
        uintptr_t child = *(uintptr_t*)(source + STEAM_VID_LINKED_OFFSET);
        unsigned int part_index = child ? *(unsigned int*)(child + STEAM_VID_INDEX_OFFSET) : 0;
        if (child && (part_index >= 1024u || !supported(child, part_index, STEAM_PUPPET_PART_CLASS) ||
            *(uintptr_t*)(child + STEAM_VID_LINKED_OFFSET))) return 0;
        memcpy(fixture.source_before, (void*)source, STEAM_VID_SIZE);
        if (child) memcpy(fixture.part_before, (void*)child, STEAM_VID_SIZE);
        fixture.source = source; fixture.part_source = child;
        fixture.source_index = index; fixture.part_index = part_index; fixture.host = !!host;
        sanitize(fixture.root_vid, source);
        if (child) sanitize(fixture.child_vid, child);
        *(uintptr_t*)(fixture.root_vid + STEAM_VID_LINKED_OFFSET) = child ? (uintptr_t)fixture.child_vid : 0;
        if (host) {
            /* Native incoming damage uses the original health/resistance fields.
               Only death creation links survive; scripts and outgoing attacks
               stay inert before factory entry. Native successors are map owned. */
            for (unsigned int animation = STEAM_ANIM_DEATH; animation <= STEAM_ANIM_GIB_DEATH; ++animation) {
                *(uintptr_t*)(fixture.root_vid + STEAM_VID_CREATIONS_OFFSET + animation * 4u) =
                    *(uintptr_t*)(source + STEAM_VID_CREATIONS_OFFSET + animation * 4u);
                if (child) *(uintptr_t*)(fixture.child_vid + STEAM_VID_CREATIONS_OFFSET + animation * 4u) =
                    *(uintptr_t*)(child + STEAM_VID_CREATIONS_OFFSET + animation * 4u);
            }
        }
        fixture.game = local->game;
        fixture.state = COMBAT_FIXTURE_OWNED;
        fixture.baseline_count = 0; fixture.capture_baseline = 1;
        UnitCombatSample before = unit_combat_sample(local->game, 0);
        fixture.capture_baseline = 0;
        if (before.fault || before.private_entities) { fixture.state = COMBAT_FIXTURE_FAULT; return -1; }
        fixture.root = (uintptr_t)shared.create((void*)local->game, NULL, fixture.root_vid,
            pose->x, pose->y, pose->z, pose->direction, NULL);
        /* Find identity through the map lists before dereferencing the result.
           A failed factory may still have created a linked private part. */
        UnitCombatSample s = unit_combat_sample(local->game, 1);
        if (!fixture.root || s.root != fixture.root || (!!s.child != !!child) || s.private_entities != (child ? 2u : 1u) || !s.sources_unchanged || s.fault) {
            fixture.state = COMBAT_FIXTURE_FAULT; return -1;
        }
        fixture.child = s.child;
        if (*(uintptr_t*)s.root != shared.base + STEAM_UNIT_VTABLE_RVA ||
            *(uintptr_t*)(s.root + STEAM_ENTITY_CHILD_OFFSET) != s.child ||
            (s.child && (*(uintptr_t*)s.child != shared.base + STEAM_SPRITE_VTABLE_RVA ||
                *(uintptr_t*)(s.child + STEAM_ENTITY_PARENT_OFFSET) != s.root))) {
            fixture.state = COMBAT_FIXTURE_FAULT; return -1;
        }
        shared.animation((void*)s.root, NULL, STEAM_ANIM_IDLE);
        if (s.child) shared.animation((void*)s.child, NULL, STEAM_ANIM_IDLE);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fixture.state = fixture.game ? COMBAT_FIXTURE_FAULT : COMBAT_FIXTURE_READY; return -1;
    }
}
int unit_combat_spawn(const Snapshot* local) {
    if (!local) return 0;
    __try {
        uintptr_t source = *(uintptr_t*)(local->game + STEAM_GAME_VID_TABLE_OFFSET + 50u * 4u);
        uintptr_t child = source ? *(uintptr_t*)(source + STEAM_VID_LINKED_OFFSET) : 0;
        if (!supported(child, 108u, STEAM_PUPPET_PART_CLASS)) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    UnitPuppetPose pose = {0}; pose.x = local->x + 80.0f; pose.y = local->y;
    pose.z = local->z; pose.direction = local->direction;
    return unit_combat_spawn_type(local, 50u, &pose, 0);
}
UnitCombatSample unit_combat_sample(uintptr_t game, int compare_sources) {
    UnitCombatSample s = {0}; s.sources_unchanged = !compare_sources;
    __try {
        if (fixture.state == COMBAT_FIXTURE_OFF || !game || *(uintptr_t*)game != shared.base + STEAM_GAME_VTABLE_RVA) { s.fault = 1; return s; }
        for (unsigned int cat = 0; cat < STEAM_TICK_ENTITY_LIST_COUNT; ++cat) {
            uintptr_t list = game + STEAM_WORLD_LIST_OFFSET + cat * STEAM_LIST_SIZE;
            unsigned int count = *(unsigned int*)(list + 4), capacity = *(unsigned int*)(list + 8);
            uintptr_t* entries = *(uintptr_t**)(list + 12);
            if (!count) continue;
            if (!entries || count > capacity || count > 65536u) { s.fault = 1; return s; }
            for (unsigned int i = 0; i < count; ++i) {
                uintptr_t e = entries[i]; if (!e) continue;
                uintptr_t vid = *(uintptr_t*)(e + STEAM_ENTITY_VID_OFFSET);
                if (vid == (uintptr_t)fixture.root_vid) { ++s.private_entities; s.root = e; }
                else if (vid == (uintptr_t)fixture.child_vid) { ++s.private_entities; s.child = e; }
                else {
                    ++s.other_entities;
                    if (owned_descriptor(vid)) continue;
                    if (fixture.capture_baseline) {
                        if (fixture.baseline_count >= sizeof(fixture.baseline)/sizeof(fixture.baseline[0])) { s.fault = 1; return s; }
                        fixture.baseline[fixture.baseline_count].entity = e;
                        fixture.baseline[fixture.baseline_count++].vid = vid;
                    } else {
                        unsigned int known = 0;
                        for (unsigned int j = 0; j < fixture.baseline_count; ++j)
                            if (fixture.baseline[j].entity == e && fixture.baseline[j].vid == vid) { known = 1; break; }
                        if (!known) ++s.unexpected_entities;
                    }
                }
            }
        }
        if (compare_sources && game == fixture.game && fixture.source &&
            *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + fixture.source_index * 4u) == fixture.source) {
            s.sources_unchanged = !memcmp((void*)fixture.source, fixture.source_before, STEAM_VID_SIZE);
            if (fixture.part_source)
                s.sources_unchanged = s.sources_unchanged &&
                    *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + fixture.part_index * 4u) == fixture.part_source &&
                    !memcmp((void*)fixture.part_source, fixture.part_before, STEAM_VID_SIZE);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { s.fault = 1; }
    return s;
}
int unit_combat_remove(uintptr_t game) {
    if (fixture.state == COMBAT_FIXTURE_OFF || !fixture.game) return 1;
    UnitCombatSample s = unit_combat_sample(game, 0);
    if (s.fault) return 0;
    if (!s.root && !s.child && !s.private_entities && !s.fault) {
        fixture.root = fixture.child = fixture.game = 0; fixture.state = COMBAT_FIXTURE_READY; return 1;
    }
    /* A native fault is never retried in this world. */
    if (fixture.state == COMBAT_FIXTURE_ABANDONED) return 0;
    fixture.state = COMBAT_FIXTURE_ABANDONED;
    __try {
        if (s.root && s.private_entities <= 2u && *(uintptr_t*)s.root == shared.base + STEAM_UNIT_VTABLE_RVA &&
            *(uintptr_t*)(s.root + STEAM_ENTITY_CHILD_OFFSET) == s.child &&
            (!s.child || (*(uintptr_t*)s.child == shared.base + STEAM_SPRITE_VTABLE_RVA &&
                !*(uintptr_t*)(s.child + STEAM_ENTITY_CHILD_OFFSET) &&
                *(uintptr_t*)(s.child + STEAM_ENTITY_PARENT_OFFSET) == s.root)))
            shared.destroy((void*)s.root, NULL, 1);
        else if (s.child && !s.root && s.private_entities == 1u && *(uintptr_t*)s.child == shared.base + STEAM_SPRITE_VTABLE_RVA &&
            !*(uintptr_t*)(s.child + STEAM_ENTITY_PARENT_OFFSET)) {
            if (*(uintptr_t*)(s.child + STEAM_ENTITY_CHILD_OFFSET)) return 0;
            /* A factory failure can leave only its private part registered.
               Its deleting destructor is still validated by unit_puppet_bind. */
            ActorDestroy destroy_part = (ActorDestroy)((uintptr_t*)(shared.base + STEAM_SPRITE_VTABLE_RVA))[0];
            destroy_part((void*)s.child, NULL, 1);
        } else return 0;
        s = unit_combat_sample(game, 0);
        if (s.fault || s.root || s.child || s.private_entities) return 0;
        fixture.root = fixture.child = fixture.game = 0; fixture.state = COMBAT_FIXTURE_READY; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
int unit_combat_stop(void) {
    for (unsigned int i = 0; i < UNIT_COMBAT_CAPACITY; ++i) if (fixtures[i].game) return 0;
    for (unsigned int i = 0; i < UNIT_COMBAT_CAPACITY; ++i) fixtures[i].state = COMBAT_FIXTURE_OFF;
    shared.initialized = 0; return 1;
}
