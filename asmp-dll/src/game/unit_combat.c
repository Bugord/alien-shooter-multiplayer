#include <string.h>
#include <float.h>
#include "unit_combat.h"
#include "../../../common/src/monster_vids.h"

enum CombatFixtureState
{
    COMBAT_FIXTURE_OFF,
    COMBAT_FIXTURE_READY,
    COMBAT_FIXTURE_OWNED,
    COMBAT_FIXTURE_FAULT,
    COMBAT_FIXTURE_ABANDONED
};
#define COMBAT_FORM_LIMIT   32u
#define COMBAT_ENTITY_LIMIT 128u
typedef struct CombatSlot
{
    uintptr_t game, root, child, source, part_source;
    unsigned int source_index, part_index;
    int host, predictive;
    unsigned char root_vid[STEAM_VID_SIZE], child_vid[STEAM_VID_SIZE];
    unsigned char source_before[STEAM_VID_SIZE], part_before[STEAM_VID_SIZE];
    enum CombatFixtureState state;
    struct
    {
        uintptr_t entity, vid;
    } baseline[2048];
    unsigned int baseline_count;
    int capture_baseline;
    unsigned char forms[COMBAT_FORM_LIMIT][STEAM_VID_SIZE];
    unsigned char form_behaviors[COMBAT_FORM_LIMIT][STEAM_BEHAVIOR_SIZE];
    unsigned char root_behavior[STEAM_BEHAVIOR_SIZE];
    uintptr_t form_sources[COMBAT_FORM_LIMIT];
    unsigned int form_classes[COMBAT_FORM_LIMIT], form_colors[COMBAT_FORM_LIMIT],
        form_cost[COMBAT_FORM_LIMIT], form_count;
    unsigned int current_vid, form_revision, transitions;
    int transitioning;
} CombatSlot;
static CombatSlot fixtures[UNIT_COMBAT_CAPACITY];
static unsigned int combat_slot;
#define fixture (fixtures[combat_slot])
static struct
{
    uintptr_t base;
    ActorCreate create;
    ActorDestroy destroy;
    UnitAnimation animation;
    int initialized;
} shared;
int unit_combat_select(unsigned int slot)
{
    if (slot >= UNIT_COMBAT_CAPACITY)
        return 0;
    combat_slot = slot;
    return 1;
}
static int owned_descriptor(uintptr_t vid)
{
    for (unsigned int i = 0; i < UNIT_COMBAT_CAPACITY; ++i)
    {
        if (vid == (uintptr_t)fixtures[i].root_vid || vid == (uintptr_t)fixtures[i].child_vid)
            return 1;
        for (unsigned int j = 0; j < fixtures[i].form_count; ++j)
            if (vid == (uintptr_t)fixtures[i].forms[j])
                return 1;
    }
    return 0;
}
static int private_form(uintptr_t vid)
{
    for (unsigned int i = 0; i < fixture.form_count; ++i)
        if (vid == (uintptr_t)fixture.forms[i])
            return (int)i;
    return -1;
}

int unit_combat_bind(uintptr_t base, ActorCreate* out)
{
    static const unsigned char signature[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
    static const unsigned char cannon[] = {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0xE8, 0x55};
    static const unsigned char terrain[] = {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0xE8, 0x15};
    *out = NULL;
    __try
    {
        if (memcmp((void*)(base + STEAM_ACTOR_CREATE_RVA), signature, sizeof(signature)) ||
            *(uintptr_t*)(base + STEAM_GAME_VTABLE_RVA + 8u * sizeof(uintptr_t)) !=
                base + STEAM_ACTOR_CREATE_RVA ||
            memcmp((void*)(base + STEAM_CANNON_DESTROY_RVA), cannon, sizeof(cannon)) ||
            memcmp((void*)(base + STEAM_TERRAIN_DESTROY_RVA), terrain, sizeof(terrain)) ||
            *(uintptr_t*)(base + STEAM_CANNON_VTABLE_RVA) != base + STEAM_CANNON_DESTROY_RVA ||
            *(uintptr_t*)(base + STEAM_TERRAIN_VTABLE_RVA) != base + STEAM_TERRAIN_DESTROY_RVA)
            return 0;
        *out = (ActorCreate)(base + STEAM_ACTOR_CREATE_RVA);
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}
int unit_combat_init(uintptr_t base, ActorCreate create, ActorDestroy destroy,
                     UnitAnimation animation)
{
    if (shared.initialized || !base || !create || !destroy || !animation)
        return 0;
    memset(fixtures, 0, sizeof(fixtures));
    combat_slot = 0;
    shared.base = base;
    shared.create = create;
    shared.destroy = destroy;
    shared.animation = animation;
    shared.initialized = 1;
    for (unsigned int i = 0; i < UNIT_COMBAT_CAPACITY; ++i)
        fixtures[i].state = COMBAT_FIXTURE_READY;
    return 1;
}
static int supported(uintptr_t vid, unsigned int index, unsigned int class_id)
{
    return vid && *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET) == index &&
           *(unsigned int*)(vid + STEAM_VID_CLASS_OFFSET) == class_id &&
           *(unsigned int*)(vid + STEAM_VID_CATEGORY_OFFSET) < STEAM_TICK_ENTITY_LIST_COUNT &&
           !*(uintptr_t*)(vid + STEAM_VID_SERVICE_OFFSET) &&
           !(*(unsigned int*)(vid + STEAM_VID_RESOURCE_FLAGS_OFFSET) &
             STEAM_VID_SPECIAL_FACTORY_FLAG);
}
static void sanitize(unsigned char* destination, uintptr_t source)
{
    memcpy(destination, (void*)source, STEAM_VID_SIZE);
    memset(destination + STEAM_VID_COUNTERS_OFFSET, 0, STEAM_VID_COUNTERS_SIZE);
    memset(destination + STEAM_VID_CREATIONS_OFFSET, 0, STEAM_ANIMATION_COUNT * 4u);
    for (unsigned int i = 0; i < STEAM_ANIMATION_COUNT; ++i)
        *(int*)(destination + STEAM_VID_ANIMATION_SCRIPTS_OFFSET + i * 4u) = -1;
    *(int*)(destination + STEAM_VID_DELETE_SCRIPT_OFFSET) = -1;
    *(int*)(destination + STEAM_VID_DAMAGE_SCRIPT_OFFSET) = -1;
    *(int*)(destination + STEAM_VID_COLLISION_SCRIPT_OFFSET) = -1;
    *(int*)(destination + STEAM_VID_DAMAGE_OFFSET) = 0;
    *(int*)(destination + STEAM_VID_CREATE_SCRIPT_OFFSET) = -1;
}
static void preserve_death(unsigned char* destination, uintptr_t source)
{
    for (unsigned int a = STEAM_ANIM_DEATH; a <= STEAM_ANIM_GIB_DEATH; ++a)
        *(uintptr_t*)(destination + STEAM_VID_CREATIONS_OFFSET + a * 4u) =
            *(uintptr_t*)(source + STEAM_VID_CREATIONS_OFFSET + a * 4u);
}
/* Only the verified armor chain admits a live UNIT successor. Other visual
   graph nodes remain harmless, with bounded recursion and multiplicity. */
static int clone_form(uintptr_t game, uintptr_t source, unsigned int expected, uintptr_t* output)
{
    if (!source || expected >= 1024u ||
        *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + expected * 4u) != source)
        return 0;
    unsigned int class_id = *(unsigned int*)(source + STEAM_VID_CLASS_OFFSET);
    if (!supported(source, expected, class_id) ||
        (class_id != 2u && class_id != 5u && class_id != 8u && class_id != 9u) ||
        !*(uintptr_t*)(source + STEAM_VID_BEHAVIOR_OFFSET))
        return 0;
    for (unsigned int i = 0; i < fixture.form_count; ++i)
        if (fixture.form_sources[i] == source)
        {
            if (fixture.form_colors[i] != 2u)
                return 0;
            *output = (uintptr_t)fixture.forms[i];
            return 1;
        }
    if (fixture.form_count >= COMBAT_FORM_LIMIT)
        return 0;
    unsigned int i = fixture.form_count++;
    fixture.form_sources[i] = source;
    fixture.form_classes[i] = class_id;
    fixture.form_colors[i] = 1u;
    unsigned char* d = fixture.forms[i];
    sanitize(d, source);
    unsigned int cost = 1u;
    if (class_id != STEAM_UNIT_CLASS && !fixture.predictive)
    {
        *(unsigned int*)(d + STEAM_VID_HEALTH_OFFSET) = 0;
        *(unsigned int*)(d + STEAM_VID_COLLISION_MASK_OFFSET) = 0;
    }
    uintptr_t linked = *(uintptr_t*)(source + STEAM_VID_LINKED_OFFSET);
    if (linked)
    {
        uintptr_t private_link;
        if (class_id == 9u || *(unsigned int*)(linked + STEAM_VID_CLASS_OFFSET) != 9u ||
            *(uintptr_t*)(linked + STEAM_VID_LINKED_OFFSET) ||
            !clone_form(game, linked, *(unsigned int*)(linked + STEAM_VID_INDEX_OFFSET),
                        &private_link))
            return 0;
        *(uintptr_t*)(d + STEAM_VID_LINKED_OFFSET) = private_link;
        int child_index = private_form(private_link);
        if (child_index < 0 || fixture.form_cost[child_index] > COMBAT_ENTITY_LIMIT - cost)
            return 0;
        cost += fixture.form_cost[child_index];
    }
    if (class_id == STEAM_UNIT_CLASS)
    {
        uintptr_t behavior = *(uintptr_t*)(source + STEAM_VID_BEHAVIOR_OFFSET);
        if (!behavior)
            return 0;
        memcpy(fixture.form_behaviors[i], (void*)behavior, STEAM_BEHAVIOR_SIZE);
        *(unsigned int*)(fixture.form_behaviors[i] + STEAM_BEHAVIOR_UNIT_FLAGS_OFFSET) = 0;
        *(uintptr_t*)(d + STEAM_VID_BEHAVIOR_OFFSET) = (uintptr_t)fixture.form_behaviors[i];
        /* Successor and frozen forms receive original resistance/health but no
           scripts, attacks or births. Their ordinary final death stays native. */
        if (fixture.host)
            preserve_death(d, source);
    }
    else if (class_id != 9u && fixture.host)
    {
        for (unsigned int a = 0; a < STEAM_ANIMATION_COUNT; ++a)
        {
            uintptr_t target = *(uintptr_t*)(source + STEAM_VID_CREATIONS_OFFSET + a * 4u);
            if (!target)
                continue;
            unsigned int n = *(unsigned int*)(source + STEAM_VID_CREATION_COUNT_OFFSET + a * 4u);
            unsigned int target_index = *(unsigned int*)(target + STEAM_VID_INDEX_OFFSET);
            unsigned int target_class = *(unsigned int*)(target + STEAM_VID_CLASS_OFFSET);
            uintptr_t private_target;
            if (!n || n > 16u ||
                (target_class == STEAM_UNIT_CLASS &&
                 !(expected == 771u && a == STEAM_ANIM_DEATH &&
                   target_index == MONSTER_VID_MONSTER2 && n == 1u)) ||
                !clone_form(game, target, target_index, &private_target))
                return 0;
            int child_index = private_form(private_target);
            if (child_index < 0 ||
                fixture.form_cost[child_index] > (COMBAT_ENTITY_LIMIT - cost) / n)
                return 0;
            cost += n * fixture.form_cost[child_index];
            *(uintptr_t*)(d + STEAM_VID_CREATIONS_OFFSET + a * 4u) = private_target;
        }
    }
    fixture.form_cost[i] = cost;
    fixture.form_colors[i] = 2u;
    *output = (uintptr_t)d;
    return 1;
}
static int armor_graph(uintptr_t game, unsigned int index, uintptr_t source)
{
    if (!mp_monster_armored(index))
        return 1;
    uintptr_t target = *(uintptr_t*)(source + STEAM_VID_CREATIONS_OFFSET + STEAM_ANIM_DEATH * 4u),
              private_target;
    unsigned int expected = index == MONSTER_VID_MONSTER2_ARMORED ? 771u : MONSTER_VID_TANK;
    if (!target ||
        *(unsigned int*)(source + STEAM_VID_CREATION_COUNT_OFFSET + STEAM_ANIM_DEATH * 4u) != 1u ||
        *(unsigned int*)(target + STEAM_VID_INDEX_OFFSET) != expected ||
        *(unsigned int*)(target + STEAM_VID_CLASS_OFFSET) !=
            (index == MONSTER_VID_MONSTER2_ARMORED ? 5u : STEAM_UNIT_CLASS) ||
        !clone_form(game, target, expected, &private_target))
        return 0;
    *(uintptr_t*)(fixture.root_vid + STEAM_VID_CREATIONS_OFFSET + STEAM_ANIM_DEATH * 4u) =
        private_target;
    return 1;
}
static int spawn_type(const Snapshot* local, unsigned int index, const UnitPuppetPose* pose,
                      int mode)
{
    if (fixture.state != COMBAT_FIXTURE_READY || fixture.game || !local || !local->game ||
        !local->in_level || local->health <= 0)
        return 0;
    if (index >= 1024u || !pose || !_finite(pose->x) || !_finite(pose->y) || !_finite(pose->z) ||
        pose->direction > 255u)
        return 0;
    __try
    {
        if (*(uintptr_t*)local->game != shared.base + STEAM_GAME_VTABLE_RVA)
            return 0;
        uintptr_t source = *(uintptr_t*)(local->game + STEAM_GAME_VID_TABLE_OFFSET + index * 4u);
        if (!supported(source, index, STEAM_UNIT_CLASS))
            return 0;
        uintptr_t child = *(uintptr_t*)(source + STEAM_VID_LINKED_OFFSET);
        unsigned int part_index = child ? *(unsigned int*)(child + STEAM_VID_INDEX_OFFSET) : 0;
        if (child &&
            (part_index >= 1024u || !supported(child, part_index, STEAM_PUPPET_PART_CLASS) ||
             *(uintptr_t*)(child + STEAM_VID_LINKED_OFFSET)))
            return 0;
        memcpy(fixture.source_before, (void*)source, STEAM_VID_SIZE);
        if (child)
            memcpy(fixture.part_before, (void*)child, STEAM_VID_SIZE);
        fixture.source = source;
        fixture.part_source = child;
        fixture.source_index = index;
        fixture.part_index = part_index;
        fixture.host = mode == 1;
        fixture.predictive = mode == 2;
        fixture.form_count = 0;
        fixture.current_vid = index;
        fixture.form_revision = 1u;
        fixture.transitions = 0;
        fixture.transitioning = 0;
        sanitize(fixture.root_vid, source);
        if (child)
            sanitize(fixture.child_vid, child);
        *(uintptr_t*)(fixture.root_vid + STEAM_VID_LINKED_OFFSET) =
            child ? (uintptr_t)fixture.child_vid : 0;
        if (fixture.host || fixture.predictive)
        {
            uintptr_t behavior = *(uintptr_t*)(source + STEAM_VID_BEHAVIOR_OFFSET);
            /* Armor successors must be stationary from constructor entry,
               before a coordinator has an opportunity to attach their hooks. */
            if (fixture.predictive || mp_monster_armored(index))
            {
                if (!behavior)
                    return 0;
                memcpy(fixture.root_behavior, (void*)behavior, STEAM_BEHAVIOR_SIZE);
                *(unsigned int*)(fixture.root_behavior + STEAM_BEHAVIOR_UNIT_FLAGS_OFFSET) = 0;
                *(uintptr_t*)(fixture.root_vid + STEAM_VID_BEHAVIOR_OFFSET) =
                    (uintptr_t)fixture.root_behavior;
            }
            /* Native incoming damage uses the original health/resistance fields.
               Only death creation links survive; scripts and outgoing attacks
               stay inert before factory entry. Native successors are map owned. */
            for (unsigned int animation = STEAM_ANIM_DEATH;
                 fixture.host && animation <= STEAM_ANIM_GIB_DEATH; ++animation)
            {
                *(uintptr_t*)(fixture.root_vid + STEAM_VID_CREATIONS_OFFSET + animation * 4u) =
                    *(uintptr_t*)(source + STEAM_VID_CREATIONS_OFFSET + animation * 4u);
                if (child)
                    *(uintptr_t*)(fixture.child_vid + STEAM_VID_CREATIONS_OFFSET + animation * 4u) =
                        *(uintptr_t*)(child + STEAM_VID_CREATIONS_OFFSET + animation * 4u);
            }
            if (fixture.host && !armor_graph(local->game, index, source))
                return 0;
        }
        fixture.game = local->game;
        fixture.state = COMBAT_FIXTURE_OWNED;
        fixture.baseline_count = 0;
        fixture.capture_baseline = 1;
        UnitCombatSample before = unit_combat_sample(local->game, 0);
        fixture.capture_baseline = 0;
        if (before.fault || before.private_entities)
        {
            fixture.state = COMBAT_FIXTURE_FAULT;
            return -1;
        }
        uintptr_t created =
            (uintptr_t)shared.create((void*)local->game, NULL, fixture.root_vid, pose->x, pose->y,
                                     pose->z, pose->direction, NULL);
        fixture.root = created;
        /* Find identity through the map lists before dereferencing the result.
           A failed factory may still have created a linked private part. */
        UnitCombatSample s = unit_combat_sample(local->game, 1);
        if (!created || s.root != created || (!!s.child != !!child) ||
            s.private_entities != (child ? 2u : 1u) || !s.sources_unchanged || s.fault)
        {
            fixture.state = COMBAT_FIXTURE_FAULT;
            return -1;
        }
        fixture.child = s.child;
        if (*(uintptr_t*)s.root != shared.base + STEAM_UNIT_VTABLE_RVA ||
            *(uintptr_t*)(s.root + STEAM_ENTITY_CHILD_OFFSET) != s.child ||
            (s.child && (*(uintptr_t*)s.child != shared.base + STEAM_SPRITE_VTABLE_RVA ||
                         *(uintptr_t*)(s.child + STEAM_ENTITY_PARENT_OFFSET) != s.root)))
        {
            fixture.state = COMBAT_FIXTURE_FAULT;
            return -1;
        }
        shared.animation((void*)s.root, NULL, STEAM_ANIM_IDLE);
        if (s.child)
            shared.animation((void*)s.child, NULL, STEAM_ANIM_IDLE);
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        fixture.state = fixture.game ? COMBAT_FIXTURE_FAULT : COMBAT_FIXTURE_READY;
        return -1;
    }
}
int unit_combat_spawn_type(const Snapshot* local, unsigned int index, const UnitPuppetPose* pose,
                           int host)
{
    return spawn_type(local, index, pose, host ? 1 : 0);
}
int unit_combat_spawn_predictive(const Snapshot* local, unsigned int index,
                                 const UnitPuppetPose* pose)
{
    return spawn_type(local, index, pose, 2);
}
int unit_combat_spawn(const Snapshot* local)
{
    if (!local)
        return 0;
    __try
    {
        uintptr_t source =
            *(uintptr_t*)(local->game + STEAM_GAME_VID_TABLE_OFFSET + MONSTER_VID_MONSTER1 * 4u);
        uintptr_t child = source ? *(uintptr_t*)(source + STEAM_VID_LINKED_OFFSET) : 0;
        if (!supported(child, 108u, STEAM_PUPPET_PART_CLASS))
            return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
    UnitPuppetPose pose = {0};
    pose.x = local->x + 80.0f;
    pose.y = local->y;
    pose.z = local->z;
    pose.direction = local->direction;
    return unit_combat_spawn_type(local, MONSTER_VID_MONSTER1, &pose, 0);
}
UnitCombatSample unit_combat_sample(uintptr_t game, int compare_sources)
{
    UnitCombatSample s = {0};
    s.sources_unchanged = !compare_sources;
    __try
    {
        if (fixture.state == COMBAT_FIXTURE_OFF || !game ||
            *(uintptr_t*)game != shared.base + STEAM_GAME_VTABLE_RVA)
        {
            s.fault = 1;
            return s;
        }
        for (unsigned int cat = 0; cat < STEAM_TICK_ENTITY_LIST_COUNT; ++cat)
        {
            uintptr_t list = game + STEAM_WORLD_LIST_OFFSET + cat * STEAM_LIST_SIZE;
            unsigned int count = *(unsigned int*)(list + STEAM_LIST_COUNT_OFFSET),
                         capacity = *(unsigned int*)(list + STEAM_LIST_CAPACITY_OFFSET);
            uintptr_t* entries = *(uintptr_t**)(list + STEAM_LIST_ITEMS_OFFSET);
            if (!count)
                continue;
            if (!entries || count > capacity || count > STEAM_LIST_MAX_ENTRIES)
            {
                s.fault = 1;
                return s;
            }
            for (unsigned int i = 0; i < count; ++i)
            {
                uintptr_t e = entries[i];
                if (!e)
                    continue;
                uintptr_t vid = *(uintptr_t*)(e + STEAM_ENTITY_VID_OFFSET);
                if (vid == (uintptr_t)fixture.root_vid)
                {
                    ++s.private_entities;
                    s.root = e;
                }
                else if (vid == (uintptr_t)fixture.child_vid)
                {
                    ++s.private_entities;
                    s.child = e;
                }
                else if (private_form(vid) >= 0)
                {
                    int f = private_form(vid);
                    ++s.private_entities;
                    unsigned int class_id = fixture.form_classes[f];
                    uintptr_t vt = shared.base + (class_id == 2u   ? STEAM_UNIT_VTABLE_RVA
                                                  : class_id == 5u ? STEAM_CANNON_VTABLE_RVA
                                                  : class_id == 8u ? STEAM_TERRAIN_VTABLE_RVA
                                                                   : STEAM_SPRITE_VTABLE_RVA);
                    if (*(uintptr_t*)e != vt || s.private_entities > COMBAT_ENTITY_LIMIT)
                    {
                        s.fault = 1;
                        return s;
                    }
                    if (class_id == STEAM_UNIT_CLASS)
                    {
                        if (s.root &&
                            *(uintptr_t*)(s.root + STEAM_ENTITY_VID_OFFSET) !=
                                (uintptr_t)fixture.root_vid &&
                            s.root != e)
                        {
                            s.fault = 1;
                            return s;
                        }
                        s.root = e;
                    }
                }
                else
                {
                    ++s.other_entities;
                    if (owned_descriptor(vid))
                        continue;
                    if (fixture.capture_baseline)
                    {
                        if (fixture.baseline_count >=
                            sizeof(fixture.baseline) / sizeof(fixture.baseline[0]))
                        {
                            s.fault = 1;
                            return s;
                        }
                        fixture.baseline[fixture.baseline_count].entity = e;
                        fixture.baseline[fixture.baseline_count++].vid = vid;
                    }
                    else
                    {
                        unsigned int known = 0;
                        for (unsigned int j = 0; j < fixture.baseline_count; ++j)
                            if (fixture.baseline[j].entity == e && fixture.baseline[j].vid == vid)
                            {
                                known = 1;
                                break;
                            }
                        if (!known)
                            ++s.unexpected_entities;
                    }
                }
            }
        }
        if (s.root)
        {
            uintptr_t vid = *(uintptr_t*)(s.root + STEAM_ENTITY_VID_OFFSET);
            unsigned int current = *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET);
            unsigned int animation = *(unsigned int*)(s.root + STEAM_ENTITY_ANIM_OFFSET);
            int pending = (fixture.host || fixture.predictive) && mp_monster_armored(current) &&
                          animation == STEAM_ANIM_DEATH;
            if (current != fixture.current_vid || pending != fixture.transitioning)
            {
                ++fixture.form_revision;
                ++fixture.transitions;
            }
            fixture.current_vid = current;
            fixture.transitioning = pending;
            fixture.root = s.root;
            uintptr_t child = *(uintptr_t*)(s.root + STEAM_ENTITY_CHILD_OFFSET);
            if (child)
            {
                /* Verify current private linked identity through map membership
                   before exposing it. An old armor part is not this successor. */
                uintptr_t expected = *(uintptr_t*)(vid + STEAM_VID_LINKED_OFFSET);
                int found = 0;
                for (unsigned int cat = 0; cat < STEAM_TICK_ENTITY_LIST_COUNT && !found; ++cat)
                {
                    uintptr_t l = game + STEAM_WORLD_LIST_OFFSET + cat * STEAM_LIST_SIZE;
                    unsigned int n = *(unsigned int*)(l + STEAM_LIST_COUNT_OFFSET),
                                 cap = *(unsigned int*)(l + STEAM_LIST_CAPACITY_OFFSET);
                    uintptr_t* items = *(uintptr_t**)(l + STEAM_LIST_ITEMS_OFFSET);
                    if (n > cap || n > STEAM_LIST_MAX_ENTRIES || (n && !items))
                    {
                        s.fault = 1;
                        return s;
                    }
                    for (unsigned int j = 0; j < n; ++j)
                        if (items[j] == child)
                        {
                            found = *(uintptr_t*)child == shared.base + STEAM_SPRITE_VTABLE_RVA &&
                                    *(uintptr_t*)(child + STEAM_ENTITY_VID_OFFSET) == expected &&
                                    *(uintptr_t*)(child + STEAM_ENTITY_PARENT_OFFSET) == s.root &&
                                    !*(uintptr_t*)(child + STEAM_ENTITY_CHILD_OFFSET);
                            break;
                        }
                }
                if (!found)
                {
                    s.fault = 1;
                    return s;
                }
            }
            s.child = child;
            fixture.child = child;
        }
        s.current_vid = fixture.current_vid;
        s.form_revision = fixture.form_revision;
        s.transitions = fixture.transitions;
        s.transitioning = fixture.transitioning;
        if (compare_sources && game == fixture.game && fixture.source &&
            *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + fixture.source_index * 4u) ==
                fixture.source)
        {
            s.sources_unchanged =
                !memcmp((void*)fixture.source, fixture.source_before, STEAM_VID_SIZE);
            if (fixture.part_source)
                s.sources_unchanged =
                    s.sources_unchanged &&
                    *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + fixture.part_index * 4u) ==
                        fixture.part_source &&
                    !memcmp((void*)fixture.part_source, fixture.part_before, STEAM_VID_SIZE);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        s.fault = 1;
    }
    return s;
}
int unit_combat_observe_entity(uintptr_t game, uintptr_t entity, uintptr_t* private_vid,
                               unsigned int* current_vid, unsigned int* revision,
                               int* transitioning, unsigned int requested_vid)
{
    unsigned int saved = combat_slot;
    int result = 0;
    __try
    {
        for (unsigned int i = 0; i < UNIT_COMBAT_CAPACITY; ++i)
        {
            if (fixtures[i].game != game || (!fixtures[i].host && !fixtures[i].predictive))
                continue;
            combat_slot = i;
            uintptr_t identity = *(uintptr_t*)(entity + STEAM_ENTITY_VID_OFFSET);
            if (fixtures[i].root != entity && identity != (uintptr_t)fixture.root_vid &&
                private_form(identity) < 0)
                continue;
            if (requested_vid)
            {
                if (fixture.current_vid < MONSTER_VID_FIRST ||
                    fixture.current_vid > MONSTER_VID_LAST || fixture.current_vid % 10u ||
                    requested_vid != fixture.current_vid + 9u)
                    break;
                uintptr_t source =
                    *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + requested_vid * 4u);
                if (!supported(source, requested_vid, STEAM_UNIT_CLASS) ||
                    !clone_form(game, source, requested_vid, private_vid))
                {
                    result = -1;
                    break;
                }
            }
            else
            {
                UnitCombatSample sample = unit_combat_sample(game, 0);
                if (sample.fault)
                {
                    result = -1;
                    break;
                }
                /* A destructor can run after a successor has already entered
                   the map; retain the previous armor transition for that call. */
                uintptr_t descriptor = *(uintptr_t*)(entity + STEAM_ENTITY_VID_OFFSET);
                unsigned int index = *(unsigned int*)(descriptor + STEAM_VID_INDEX_OFFSET);
                *private_vid = descriptor;
                *current_vid = index;
                *revision = sample.form_revision;
                *transitioning =
                    mp_monster_armored(index) &&
                    *(unsigned int*)(entity + STEAM_ENTITY_ANIM_OFFSET) == STEAM_ANIM_DEATH;
            }
            result = 1;
            break;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        result = -1;
    }
    combat_slot = saved;
    return result;
}
static unsigned int private_class(uintptr_t vid)
{
    if (vid == (uintptr_t)fixture.root_vid)
        return STEAM_UNIT_CLASS;
    if (vid == (uintptr_t)fixture.child_vid)
        return STEAM_PUPPET_PART_CLASS;
    int i = private_form(vid);
    return i >= 0 ? fixture.form_classes[i] : 0u;
}
static int private_registered(uintptr_t game, uintptr_t entity)
{
    for (unsigned int cat = 0; cat < STEAM_TICK_ENTITY_LIST_COUNT; ++cat)
    {
        uintptr_t l = game + STEAM_WORLD_LIST_OFFSET + cat * STEAM_LIST_SIZE;
        unsigned int n = *(unsigned int*)(l + STEAM_LIST_COUNT_OFFSET),
                     cap = *(unsigned int*)(l + STEAM_LIST_CAPACITY_OFFSET);
        uintptr_t* entries = *(uintptr_t**)(l + STEAM_LIST_ITEMS_OFFSET);
        if (n > cap || n > STEAM_LIST_MAX_ENTRIES || (n && !entries))
            return 0;
        for (unsigned int i = 0; i < n; ++i)
            if (entries[i] == entity)
            {
                unsigned int c = private_class(*(uintptr_t*)(entity + STEAM_ENTITY_VID_OFFSET));
                uintptr_t expected = shared.base + (c == 2u   ? STEAM_UNIT_VTABLE_RVA
                                                    : c == 5u ? STEAM_CANNON_VTABLE_RVA
                                                    : c == 8u ? STEAM_TERRAIN_VTABLE_RVA
                                                              : STEAM_SPRITE_VTABLE_RVA);
                return c && *(uintptr_t*)entity == expected;
            }
    }
    return 0;
}
static int remove_forms(uintptr_t game)
{
    /* Disable creation before the first deleting destructor. Linked children
       retain their native recursive ownership, all other graph roots are found
       again by private descriptor identity after each deletion. */
    memset(fixture.root_vid + STEAM_VID_CREATIONS_OFFSET, 0, STEAM_ANIMATION_COUNT * 4u);
    memset(fixture.child_vid + STEAM_VID_CREATIONS_OFFSET, 0, STEAM_ANIMATION_COUNT * 4u);
    for (unsigned int i = 0; i < fixture.form_count; ++i)
        memset(fixture.forms[i] + STEAM_VID_CREATIONS_OFFSET, 0, STEAM_ANIMATION_COUNT * 4u);
    for (unsigned int attempt = 0; attempt < COMBAT_ENTITY_LIMIT; ++attempt)
    {
        uintptr_t found = 0;
        for (unsigned int cat = 0; cat < STEAM_TICK_ENTITY_LIST_COUNT && !found; ++cat)
        {
            uintptr_t l = game + STEAM_WORLD_LIST_OFFSET + cat * STEAM_LIST_SIZE;
            unsigned int n = *(unsigned int*)(l + STEAM_LIST_COUNT_OFFSET);
            uintptr_t* entries = *(uintptr_t**)(l + STEAM_LIST_ITEMS_OFFSET);
            for (unsigned int j = 0; j < n; ++j)
            {
                uintptr_t e = entries[j];
                if (!e || !private_class(*(uintptr_t*)(e + STEAM_ENTITY_VID_OFFSET)))
                    continue;
                if (!private_registered(game, e))
                    return 0;
                uintptr_t child = *(uintptr_t*)(e + STEAM_ENTITY_CHILD_OFFSET);
                if (child && (!private_registered(game, child) ||
                              *(uintptr_t*)(child + STEAM_ENTITY_PARENT_OFFSET) != e ||
                              *(uintptr_t*)(child + STEAM_ENTITY_CHILD_OFFSET)))
                    return 0;
                uintptr_t parent = *(uintptr_t*)(e + STEAM_ENTITY_PARENT_OFFSET);
                if (parent)
                {
                    if (!private_registered(game, parent) ||
                        *(uintptr_t*)(parent + STEAM_ENTITY_CHILD_OFFSET) != e)
                        return 0;
                }
                else
                {
                    found = e;
                    break;
                }
            }
        }
        if (!found)
            break;
        unsigned int c = private_class(*(uintptr_t*)(found + STEAM_ENTITY_VID_OFFSET));
        ActorDestroy destroy =
            c == STEAM_UNIT_CLASS
                ? shared.destroy
                : *(ActorDestroy*)(shared.base + (c == 5u   ? STEAM_CANNON_VTABLE_RVA
                                                  : c == 8u ? STEAM_TERRAIN_VTABLE_RVA
                                                            : STEAM_SPRITE_VTABLE_RVA));
        if (!destroy)
            return 0;
        destroy((void*)found, NULL, 1);
    }
    UnitCombatSample after = unit_combat_sample(game, 0);
    return !after.fault && !after.private_entities;
}
int unit_combat_remove(uintptr_t game)
{
    if (fixture.state == COMBAT_FIXTURE_OFF || !fixture.game)
        return 1;
    UnitCombatSample s = unit_combat_sample(game, 0);
    if (s.fault)
        return 0;
    if (!s.root && !s.child && !s.private_entities && !s.fault)
    {
        fixture.root = fixture.child = fixture.game = 0;
        fixture.state = COMBAT_FIXTURE_READY;
        return 1;
    }
    /* A native fault is never retried in this world. */
    if (fixture.state == COMBAT_FIXTURE_ABANDONED)
        return 0;
    fixture.state = COMBAT_FIXTURE_ABANDONED;
    __try
    {
        if (fixture.form_count)
        {
            if (!remove_forms(game))
                return 0;
            fixture.root = fixture.child = fixture.game = 0;
            fixture.state = COMBAT_FIXTURE_READY;
            return 1;
        }
        if (s.root && s.private_entities <= 2u &&
            *(uintptr_t*)s.root == shared.base + STEAM_UNIT_VTABLE_RVA &&
            *(uintptr_t*)(s.root + STEAM_ENTITY_CHILD_OFFSET) == s.child &&
            (!s.child || (*(uintptr_t*)s.child == shared.base + STEAM_SPRITE_VTABLE_RVA &&
                          !*(uintptr_t*)(s.child + STEAM_ENTITY_CHILD_OFFSET) &&
                          *(uintptr_t*)(s.child + STEAM_ENTITY_PARENT_OFFSET) == s.root)))
            shared.destroy((void*)s.root, NULL, 1);
        else if (s.child && !s.root && s.private_entities == 1u &&
                 *(uintptr_t*)s.child == shared.base + STEAM_SPRITE_VTABLE_RVA &&
                 !*(uintptr_t*)(s.child + STEAM_ENTITY_PARENT_OFFSET))
        {
            if (*(uintptr_t*)(s.child + STEAM_ENTITY_CHILD_OFFSET))
                return 0;
            /* A factory failure can leave only its private part registered.
               Its deleting destructor is still validated by unit_puppet_bind. */
            ActorDestroy destroy_part =
                (ActorDestroy)((uintptr_t*)(shared.base + STEAM_SPRITE_VTABLE_RVA))[0];
            destroy_part((void*)s.child, NULL, 1);
        }
        else
            return 0;
        s = unit_combat_sample(game, 0);
        if (s.fault || s.root || s.child || s.private_entities)
            return 0;
        fixture.root = fixture.child = fixture.game = 0;
        fixture.state = COMBAT_FIXTURE_READY;
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}
int unit_combat_stop(void)
{
    for (unsigned int i = 0; i < UNIT_COMBAT_CAPACITY; ++i)
        if (fixtures[i].game)
            return 0;
    for (unsigned int i = 0; i < UNIT_COMBAT_CAPACITY; ++i)
        fixtures[i].state = COMBAT_FIXTURE_OFF;
    shared.initialized = 0;
    return 1;
}
