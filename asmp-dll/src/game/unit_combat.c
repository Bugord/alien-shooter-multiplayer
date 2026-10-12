/* Combat fixtures: private copies of a stock monster form (and its part) created
   through the native factory, used as the host's or a client's visible monster.
   Layer: game/ (engine API). Game thread only, native access inside SEH. The module owns
   the private VID copies; the factory-created entities belong to the map and the stock
   descriptors are only borrowed. */

#include <string.h>
#include <float.h>
#include "unit_combat.h"
#include "../../../common/src/monster_vids.h"
#include "entity_class.h"
#include "world_list.h"

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
/* Room for the foreign map entities recorded before a spawn. */
#define COMBAT_BASELINE_LIMIT 2048u
/* Most entities one animation of a cloned visual form may create. */
#define COMBAT_CREATION_LIMIT 16u
/* The default test monster appears this far to the right of the player. */
#define COMBAT_TEST_SPAWN_OFFSET 80.0f
/* Slot of the native factory in the GAME vtable. */
#define COMBAT_GAME_CREATE_SLOT 8u

/* Depth-first colors of a form while the clone graph is built. */
enum FormColor
{
    FORM_UNVISITED,
    FORM_IN_PROGRESS, /* Seen again before it finished: a cycle, refused. */
    FORM_DONE
};

/* One spawned fixture: private VID copies and the native entities created from
   them. Public functions receive the fixture explicitly. */
struct UnitCombat
{
    enum CombatFixtureState state;
    uintptr_t game;
    uintptr_t root;  /* Native root created by the factory; owned by the map. */
    uintptr_t child; /* Native part of root, or 0. */
    int host;
    int predictive;
    /* Private descriptors owned by this fixture (pinned in the DLL). */
    unsigned char root_vid[STEAM_VID_SIZE];
    unsigned char child_vid[STEAM_VID_SIZE];
    unsigned char root_behavior[STEAM_BEHAVIOR_SIZE];

    /* Borrowed stock descriptors and their bytes at spawn time. */
    struct
    {
        uintptr_t root;
        uintptr_t part;
        unsigned int root_index;
        unsigned int part_index;
        unsigned char root_before[STEAM_VID_SIZE];
        unsigned char part_before[STEAM_VID_SIZE];
    } sources;

    /* Foreign map entities present before the spawn. */
    struct
    {
        struct
        {
            uintptr_t entity;
            uintptr_t vid;
        } entries[COMBAT_BASELINE_LIMIT];
        unsigned int count;
        int capturing;
    } baseline;

    /* Private copies of the successor/visual form graph (DFS colors, costs). */
    struct
    {
        unsigned char vids[COMBAT_FORM_LIMIT][STEAM_VID_SIZE];
        unsigned char behaviors[COMBAT_FORM_LIMIT][STEAM_BEHAVIOR_SIZE];
        uintptr_t sources[COMBAT_FORM_LIMIT];
        unsigned int classes[COMBAT_FORM_LIMIT];
        unsigned int colors[COMBAT_FORM_LIMIT];
        unsigned int cost[COMBAT_FORM_LIMIT];
        unsigned int count;
    } graph;

    /* Current native form of the root. */
    struct
    {
        unsigned int current_vid;
        unsigned int revision;
        unsigned int transitions;
        int transitioning;
    } form;
};

static UnitCombat fixtures[UNIT_COMBAT_CAPACITY];
static struct
{
    uintptr_t base;
    ActorCreate create;
    ActorDestroy destroy;
    UnitAnimation animation;
    int initialized;
} shared;

/* Caller in SEH. The engine's stock descriptor for table index `index`. */
static uintptr_t stock_vid(uintptr_t game, unsigned int index)
{
    return *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + index * sizeof(uintptr_t));
}

/* The creation link (or creation count) entry for animation `animation` of a descriptor. */
static uintptr_t* creation_link(uintptr_t vid, unsigned int animation)
{
    return (uintptr_t*)(vid + STEAM_VID_CREATIONS_OFFSET + animation * sizeof(uintptr_t));
}

static unsigned int creation_count(uintptr_t vid, unsigned int animation)
{
    return *(unsigned int*)(vid + STEAM_VID_CREATION_COUNT_OFFSET +
                            animation * sizeof(unsigned int));
}

/* Caller in SEH. True when `child` is a SPRITE part of `root` with no part of its own. */
static int is_part_of(uintptr_t child, uintptr_t root)
{
    if (*(uintptr_t*)child != shared.base + STEAM_SPRITE_VTABLE_RVA)
        return 0;
    if (*(uintptr_t*)(child + STEAM_ENTITY_CHILD_OFFSET))
        return 0;
    return *(uintptr_t*)(child + STEAM_ENTITY_PARENT_OFFSET) == root;
}

/* Returns fixture `index`, or NULL when out of range. */
UnitCombat* unit_combat_slot(unsigned int index)
{
    if (index >= UNIT_COMBAT_CAPACITY)
        return NULL;
    return &fixtures[index];
}

/* True when `vid` is a private buffer of any fixture. */
static int owned_descriptor(uintptr_t vid)
{
    for (unsigned int i = 0; i < UNIT_COMBAT_CAPACITY; ++i)
    {
        if (vid == (uintptr_t)fixtures[i].root_vid || vid == (uintptr_t)fixtures[i].child_vid)
            return 1;
        for (unsigned int j = 0; j < fixtures[i].graph.count; ++j)
        {
            if (vid == (uintptr_t)fixtures[i].graph.vids[j])
                return 1;
        }
    }
    return 0;
}

/* Index of `vid` among the fixture's private forms, or -1. */
static int private_form(const UnitCombat* fixture, uintptr_t vid)
{
    for (unsigned int i = 0; i < fixture->graph.count; ++i)
    {
        if (vid == (uintptr_t)fixture->graph.vids[i])
            return (int)i;
    }
    return -1;
}

/* Verifies the native factory and returns it in `out`; hooks nothing. */
enum UnitResult unit_combat_bind(uintptr_t base, ActorCreate* out)
{
    static const unsigned char signature[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
    static const unsigned char cannon[] = {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0xE8, 0x55};
    static const unsigned char terrain[] = {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0xE8, 0x15};
    *out = NULL;
    __try
    {
        /* The factory: its first bytes and its GAME vtable slot. */
        uintptr_t create = base + STEAM_ACTOR_CREATE_RVA;
        uintptr_t* game_vtable = (uintptr_t*)(base + STEAM_GAME_VTABLE_RVA);
        if (memcmp((void*)create, signature, sizeof(signature)))
            return UNIT_REFUSED;
        if (game_vtable[COMBAT_GAME_CREATE_SLOT] != create)
            return UNIT_REFUSED;

        /* The CANNON and TERRAIN destructors used to remove cloned visual forms. */
        if (memcmp((void*)(base + STEAM_CANNON_DESTROY_RVA), cannon, sizeof(cannon)))
            return UNIT_REFUSED;
        if (memcmp((void*)(base + STEAM_TERRAIN_DESTROY_RVA), terrain, sizeof(terrain)))
            return UNIT_REFUSED;
        if (*(uintptr_t*)(base + STEAM_CANNON_VTABLE_RVA) != base + STEAM_CANNON_DESTROY_RVA)
            return UNIT_REFUSED;
        if (*(uintptr_t*)(base + STEAM_TERRAIN_VTABLE_RVA) != base + STEAM_TERRAIN_DESTROY_RVA)
            return UNIT_REFUSED;

        *out = (ActorCreate)create;
        return UNIT_OK;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return UNIT_REFUSED;
    }
}

/* Stores the factory and destructors used by every fixture (once, before spawning). */
enum UnitResult unit_combat_init(uintptr_t base, ActorCreate create, ActorDestroy destroy,
                                 UnitAnimation animation)
{
    if (shared.initialized || !base || !create || !destroy || !animation)
        return UNIT_REFUSED;
    memset(fixtures, 0, sizeof(fixtures));
    shared.base = base;
    shared.create = create;
    shared.destroy = destroy;
    shared.animation = animation;
    shared.initialized = 1;
    for (unsigned int i = 0; i < UNIT_COMBAT_CAPACITY; ++i)
        fixtures[i].state = COMBAT_FIXTURE_READY;
    return UNIT_OK;
}

/* True when `vid` is the loaded stock descriptor `index` of class `class_id`, with a valid
   category, no service object and no special factory flag. */
static int supported(uintptr_t vid, unsigned int index, unsigned int class_id)
{
    if (!vid)
        return 0;
    if (*(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET) != index)
        return 0;
    if (*(unsigned int*)(vid + STEAM_VID_CLASS_OFFSET) != class_id)
        return 0;
    if (*(unsigned int*)(vid + STEAM_VID_CATEGORY_OFFSET) >= STEAM_TICK_ENTITY_LIST_COUNT)
        return 0;
    if (*(uintptr_t*)(vid + STEAM_VID_SERVICE_OFFSET))
        return 0;

    unsigned int flags = *(unsigned int*)(vid + STEAM_VID_RESOURCE_FLAGS_OFFSET);
    return !(flags & STEAM_VID_SPECIAL_FACTORY_FLAG);
}

/* Copies a stock descriptor into `destination` with counters, creations, scripts and
   damage cleared, so the copy is purely visual. */
static void sanitize(unsigned char* destination, uintptr_t source)
{
    memcpy(destination, (void*)source, STEAM_VID_SIZE);
    memset(destination + STEAM_VID_COUNTERS_OFFSET, 0, STEAM_VID_COUNTERS_SIZE);
    memset(destination + STEAM_VID_CREATIONS_OFFSET, 0, STEAM_ANIMATION_COUNT * sizeof(uintptr_t));

    /* -1 means "no script". */
    for (unsigned int i = 0; i < STEAM_ANIMATION_COUNT; ++i)
        *(int*)(destination + STEAM_VID_ANIMATION_SCRIPTS_OFFSET + i * sizeof(int)) = -1;
    *(int*)(destination + STEAM_VID_DELETE_SCRIPT_OFFSET) = -1;
    *(int*)(destination + STEAM_VID_DAMAGE_SCRIPT_OFFSET) = -1;
    *(int*)(destination + STEAM_VID_COLLISION_SCRIPT_OFFSET) = -1;
    *(int*)(destination + STEAM_VID_DAMAGE_OFFSET) = 0;
    *(int*)(destination + STEAM_VID_CREATE_SCRIPT_OFFSET) = -1;
}

/* Copies the stock death creation links into a private copy (host mode only). */
static void preserve_death(unsigned char* destination, uintptr_t source)
{
    for (unsigned int a = STEAM_ANIM_DEATH; a <= STEAM_ANIM_GIB_DEATH; ++a)
        *creation_link((uintptr_t)destination, a) = *creation_link(source, a);
}

static int clone_form(UnitCombat* fixture, uintptr_t game, uintptr_t source, unsigned int expected,
                      uintptr_t* output);

/* Caller in SEH. True when source is the loaded stock form `expected` of a
   supported class with a behavior. */
static int form_source_admissible(uintptr_t game, uintptr_t source, unsigned int expected)
{
    if (!source || expected >= STEAM_STOCK_VID_LIMIT)
        return 0;
    if (stock_vid(game, expected) != source)
        return 0;

    unsigned int class_id = *(unsigned int*)(source + STEAM_VID_CLASS_OFFSET);
    if (!supported(source, expected, class_id))
        return 0;
    switch (class_id)
    {
    case STEAM_UNIT_CLASS:
    case STEAM_CANNON_CLASS:
    case STEAM_TERRAIN_CLASS:
    case STEAM_PUPPET_PART_CLASS:
        break;
    default:
        return 0;
    }
    return *(uintptr_t*)(source + STEAM_VID_BEHAVIOR_OFFSET) != 0;
}

/* Clones the linked part of form i (a part without a link of its own) and
   adds its cost to *cost. */
static int clone_linked_part(UnitCombat* fixture, uintptr_t game, uintptr_t source, unsigned int i,
                             unsigned int* cost)
{
    uintptr_t linked = *(uintptr_t*)(source + STEAM_VID_LINKED_OFFSET);
    if (!linked)
        return 1;

    /* Only a non-part form may link a part, and that part links nothing further. */
    if (fixture->graph.classes[i] == STEAM_PUPPET_PART_CLASS)
        return 0;
    if (*(unsigned int*)(linked + STEAM_VID_CLASS_OFFSET) != STEAM_PUPPET_PART_CLASS)
        return 0;
    if (*(uintptr_t*)(linked + STEAM_VID_LINKED_OFFSET))
        return 0;

    uintptr_t private_link;
    unsigned int linked_index = *(unsigned int*)(linked + STEAM_VID_INDEX_OFFSET);
    if (!clone_form(fixture, game, linked, linked_index, &private_link))
        return 0;
    *(uintptr_t*)(fixture->graph.vids[i] + STEAM_VID_LINKED_OFFSET) = private_link;

    int child_index = private_form(fixture, private_link);
    if (child_index < 0 || fixture->graph.cost[child_index] > COMBAT_ENTITY_LIMIT - *cost)
        return 0;
    *cost += fixture->graph.cost[child_index];
    return 1;
}

/* Gives UNIT form i a private, stationary copy of its behavior. */
static int copy_form_behavior(UnitCombat* fixture, uintptr_t source, unsigned int i)
{
    uintptr_t behavior = *(uintptr_t*)(source + STEAM_VID_BEHAVIOR_OFFSET);
    if (!behavior)
        return 0;
    memcpy(fixture->graph.behaviors[i], (void*)behavior, STEAM_BEHAVIOR_SIZE);
    *(unsigned int*)(fixture->graph.behaviors[i] + STEAM_BEHAVIOR_UNIT_FLAGS_OFFSET) = 0;
    *(uintptr_t*)(fixture->graph.vids[i] + STEAM_VID_BEHAVIOR_OFFSET) =
        (uintptr_t)fixture->graph.behaviors[i];
    return 1;
}

/* Host: clones what visual form i creates on each animation. Only the medium
   armor shell may create its living successor UNIT. */
static int clone_host_creations(UnitCombat* fixture, uintptr_t game, uintptr_t source,
                                unsigned int expected, unsigned int i, unsigned int* cost)
{
    uintptr_t d = (uintptr_t)fixture->graph.vids[i];
    for (unsigned int a = 0; a < STEAM_ANIMATION_COUNT; ++a)
    {
        uintptr_t target = *creation_link(source, a);
        if (!target)
            continue;

        unsigned int n = creation_count(source, a);
        if (!n || n > COMBAT_CREATION_LIMIT)
            return 0;

        /* A creation may be a live UNIT only for the medium armor shell's death. */
        unsigned int target_index = *(unsigned int*)(target + STEAM_VID_INDEX_OFFSET);
        unsigned int target_class = *(unsigned int*)(target + STEAM_VID_CLASS_OFFSET);
        int shell_successor = expected == MONSTER_VID_MONSTER2_ARMOR_SHELL &&
                              a == STEAM_ANIM_DEATH && target_index == MONSTER_VID_MONSTER2 &&
                              n == 1u;
        if (target_class == STEAM_UNIT_CLASS && !shell_successor)
            return 0;

        uintptr_t private_target;
        if (!clone_form(fixture, game, target, target_index, &private_target))
            return 0;
        int child_index = private_form(fixture, private_target);
        if (child_index < 0)
            return 0;
        if (fixture->graph.cost[child_index] > (COMBAT_ENTITY_LIMIT - *cost) / n)
            return 0;

        *cost += n * fixture->graph.cost[child_index];
        *creation_link(d, a) = private_target;
    }
    return 1;
}

/* Only the verified armor chain admits a live UNIT successor. Other visual
   graph nodes remain harmless, with bounded recursion and multiplicity. */
static int clone_form(UnitCombat* fixture, uintptr_t game, uintptr_t source, unsigned int expected,
                      uintptr_t* output)
{
    if (!form_source_admissible(game, source, expected))
        return 0;
    unsigned int class_id = *(unsigned int*)(source + STEAM_VID_CLASS_OFFSET);
    for (unsigned int i = 0; i < fixture->graph.count; ++i)
    {
        if (fixture->graph.sources[i] == source)
        {
            if (fixture->graph.colors[i] != FORM_DONE)
                return 0;
            *output = (uintptr_t)fixture->graph.vids[i];
            return 1;
        }
    }
    if (fixture->graph.count >= COMBAT_FORM_LIMIT)
        return 0;

    unsigned int i = fixture->graph.count++;
    fixture->graph.sources[i] = source;
    fixture->graph.classes[i] = class_id;
    fixture->graph.colors[i] = FORM_IN_PROGRESS;
    unsigned char* d = fixture->graph.vids[i];
    sanitize(d, source);
    unsigned int cost = 1u;
    if (class_id != STEAM_UNIT_CLASS && !fixture->predictive)
    {
        *(unsigned int*)(d + STEAM_VID_HEALTH_OFFSET) = 0;
        *(unsigned int*)(d + STEAM_VID_COLLISION_MASK_OFFSET) = 0;
    }
    if (!clone_linked_part(fixture, game, source, i, &cost))
        return 0;

    if (class_id == STEAM_UNIT_CLASS)
    {
        if (!copy_form_behavior(fixture, source, i))
            return 0;
        /* Successor and frozen forms receive original resistance/health but no
           scripts, attacks or births. Their ordinary final death stays native. */
        if (fixture->host)
            preserve_death(d, source);
    }
    else if (class_id != STEAM_PUPPET_PART_CLASS && fixture->host)
    {
        if (!clone_host_creations(fixture, game, source, expected, i, &cost))
            return 0;
    }
    fixture->graph.cost[i] = cost;
    fixture->graph.colors[i] = FORM_DONE;
    *output = (uintptr_t)d;
    return 1;
}

/* Clones the armor shell form of `index` and links it as the private root's death creation. */
static int armor_graph(UnitCombat* fixture, uintptr_t game, unsigned int index, uintptr_t source)
{
    if (!mp_monster_armored(index))
        return 1;

    /* Medium armor drops a CANNON shell; large armor turns into the plain tank UNIT. */
    int medium = index == MONSTER_VID_MONSTER2_ARMORED;
    unsigned int expected = medium ? MONSTER_VID_MONSTER2_ARMOR_SHELL : MONSTER_VID_TANK;
    unsigned int expected_class = medium ? STEAM_CANNON_CLASS : STEAM_UNIT_CLASS;

    uintptr_t target = *creation_link(source, STEAM_ANIM_DEATH);
    if (!target || creation_count(source, STEAM_ANIM_DEATH) != 1u)
        return 0;
    if (*(unsigned int*)(target + STEAM_VID_INDEX_OFFSET) != expected)
        return 0;
    if (*(unsigned int*)(target + STEAM_VID_CLASS_OFFSET) != expected_class)
        return 0;

    uintptr_t private_target;
    if (!clone_form(fixture, game, target, expected, &private_target))
        return 0;
    *creation_link((uintptr_t)fixture->root_vid, STEAM_ANIM_DEATH) = private_target;
    return 1;
}

/* How spawn_type prepares the private descriptors. */
enum SpawnMode
{
    SPAWN_FIXTURE = 0,   /* Inert offline fixture. */
    SPAWN_HOST = 1,      /* Host gallery: native death links survive. */
    SPAWN_PREDICTIVE = 2 /* Client copy: native incoming damage survives. */
};

/* True when a ready fixture may spawn stock VID `index` at pose. */
static int spawn_request_valid(const UnitCombat* fixture, const Snapshot* local, unsigned int index,
                               const UnitPuppetPose* pose)
{
    if (!fixture || fixture->state != COMBAT_FIXTURE_READY || fixture->game)
        return 0;
    if (!local || !local->game || !local->in_level || local->health <= 0)
        return 0;
    if (index >= STEAM_STOCK_VID_LIMIT || !pose)
        return 0;
    if (!_finite(pose->x) || !_finite(pose->y) || !_finite(pose->z))
        return 0;
    return pose->direction <= STEAM_DIRECTION_MAX;
}

/* Caller in SEH. Finds the stock UNIT `index` and its optional part, and
   saves copies of both so later samples can prove they stayed unchanged. */
static int capture_spawn_sources(UnitCombat* fixture, uintptr_t game, unsigned int index)
{
    uintptr_t source = stock_vid(game, index);
    if (!supported(source, index, STEAM_UNIT_CLASS))
        return 0;

    uintptr_t child = *(uintptr_t*)(source + STEAM_VID_LINKED_OFFSET);
    unsigned int part_index = 0;
    if (child)
    {
        part_index = *(unsigned int*)(child + STEAM_VID_INDEX_OFFSET);
        if (part_index >= STEAM_STOCK_VID_LIMIT)
            return 0;
        if (!supported(child, part_index, STEAM_PUPPET_PART_CLASS))
            return 0;
        if (*(uintptr_t*)(child + STEAM_VID_LINKED_OFFSET))
            return 0;
    }

    memcpy(fixture->sources.root_before, (void*)source, STEAM_VID_SIZE);
    if (child)
        memcpy(fixture->sources.part_before, (void*)child, STEAM_VID_SIZE);
    fixture->sources.root = source;
    fixture->sources.part = child;
    fixture->sources.root_index = index;
    fixture->sources.part_index = part_index;
    return 1;
}

/* Host or predictive copy: a stationary root behavior and, on the host, the
   native death creation links. Returns 0 when the graph is not supported. */
static int prepare_live_root(UnitCombat* fixture, uintptr_t game, unsigned int index)
{
    uintptr_t source = fixture->sources.root;
    uintptr_t child = fixture->sources.part;
    uintptr_t behavior = *(uintptr_t*)(source + STEAM_VID_BEHAVIOR_OFFSET);
    /* Armor successors must be stationary from constructor entry,
       before a coordinator has an opportunity to attach their hooks. */
    if (fixture->predictive || mp_monster_armored(index))
    {
        if (!behavior)
            return 0;
        memcpy(fixture->root_behavior, (void*)behavior, STEAM_BEHAVIOR_SIZE);
        *(unsigned int*)(fixture->root_behavior + STEAM_BEHAVIOR_UNIT_FLAGS_OFFSET) = 0;
        *(uintptr_t*)(fixture->root_vid + STEAM_VID_BEHAVIOR_OFFSET) =
            (uintptr_t)fixture->root_behavior;
    }
    if (!fixture->host)
        return 1;

    /* Native incoming damage uses the original health/resistance fields.
       Only death creation links survive; scripts and outgoing attacks
       stay inert before factory entry. Native successors are map owned. */
    preserve_death(fixture->root_vid, source);
    if (child)
        preserve_death(fixture->child_vid, child);
    return armor_graph(fixture, game, index, source);
}

/* Caller in SEH. Fills the private root and part descriptors for a spawn. */
static int prepare_spawn(UnitCombat* fixture, uintptr_t game, unsigned int index,
                         enum SpawnMode mode)
{
    if (!capture_spawn_sources(fixture, game, index))
        return 0;
    uintptr_t child = fixture->sources.part;
    fixture->host = mode == SPAWN_HOST;
    fixture->predictive = mode == SPAWN_PREDICTIVE;
    fixture->graph.count = 0;
    fixture->form.current_vid = index;
    fixture->form.revision = 1u;
    fixture->form.transitions = 0;
    fixture->form.transitioning = 0;
    sanitize(fixture->root_vid, fixture->sources.root);
    if (child)
        sanitize(fixture->child_vid, child);

    uintptr_t* root_link = (uintptr_t*)(fixture->root_vid + STEAM_VID_LINKED_OFFSET);
    *root_link = child ? (uintptr_t)fixture->child_vid : 0;
    if (fixture->host || fixture->predictive)
        return prepare_live_root(fixture, game, index);
    return 1;
}

/* Caller in SEH. True when the created root (and part) are registered with
   the expected vtables and linked to each other. */
static int created_entities_valid(const UnitCombatSample* s)
{
    if (*(uintptr_t*)s->root != shared.base + STEAM_UNIT_VTABLE_RVA)
        return 0;
    if (*(uintptr_t*)(s->root + STEAM_ENTITY_CHILD_OFFSET) != s->child)
        return 0;
    if (!s->child)
        return 1;
    if (*(uintptr_t*)s->child != shared.base + STEAM_SPRITE_VTABLE_RVA)
        return 0;
    return *(uintptr_t*)(s->child + STEAM_ENTITY_PARENT_OFFSET) == s->root;
}

/* Caller in SEH, fixture already owned. Records the map baseline, creates
   the entity through the native factory and verifies it. Returns 1 or -1. */
static enum UnitResult create_fixture_entity(UnitCombat* fixture, uintptr_t game,
                                             const UnitPuppetPose* pose)
{
    uintptr_t child = fixture->sources.part;
    fixture->baseline.count = 0;
    fixture->baseline.capturing = 1;
    UnitCombatSample before = unit_combat_sample(fixture, game, 0);
    fixture->baseline.capturing = 0;
    if (before.fault || before.private_entities)
    {
        fixture->state = COMBAT_FIXTURE_FAULT;
        return UNIT_FAULT;
    }

    uintptr_t created = (uintptr_t)shared.create((void*)game, NULL, fixture->root_vid, pose->x,
                                                 pose->y, pose->z, pose->direction, NULL);
    fixture->root = created;
    /* Find identity through the map lists before dereferencing the result.
       A failed factory may still have created a linked private part. */
    UnitCombatSample s = unit_combat_sample(fixture, game, 1);
    unsigned int expected_entities = child ? 2u : 1u;
    int has_part = s.child != 0;
    int wants_part = child != 0;
    int created_as_expected = created && s.root == created && has_part == wants_part &&
                              s.private_entities == expected_entities;
    if (!created_as_expected || !s.sources_unchanged || s.fault)
    {
        fixture->state = COMBAT_FIXTURE_FAULT;
        return UNIT_FAULT;
    }
    fixture->child = s.child;
    if (!created_entities_valid(&s))
    {
        fixture->state = COMBAT_FIXTURE_FAULT;
        return UNIT_FAULT;
    }
    shared.animation((void*)s.root, NULL, STEAM_ANIM_IDLE);
    if (s.child)
        shared.animation((void*)s.child, NULL, STEAM_ANIM_IDLE);
    return UNIT_OK;
}

/* Common spawn body: validates the request, prepares the private forms, creates the
   entity and verifies it. */
static enum UnitResult spawn_type(UnitCombat* fixture, const Snapshot* local, unsigned int index,
                                  const UnitPuppetPose* pose, enum SpawnMode mode)
{
    if (!spawn_request_valid(fixture, local, index, pose))
        return UNIT_REFUSED;
    __try
    {
        if (*(uintptr_t*)local->game != shared.base + STEAM_GAME_VTABLE_RVA)
            return UNIT_REFUSED;
        if (!prepare_spawn(fixture, local->game, index, mode))
            return UNIT_REFUSED;
        fixture->game = local->game;
        fixture->state = COMBAT_FIXTURE_OWNED;
        return create_fixture_entity(fixture, local->game, pose);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        fixture->state = fixture->game ? COMBAT_FIXTURE_FAULT : COMBAT_FIXTURE_READY;
        return UNIT_FAULT;
    }
}

/* Spawns stock monster `index` at `pose` as a private copy; `host` keeps native death links. */
enum UnitResult unit_combat_spawn_type(UnitCombat* fixture, const Snapshot* local,
                                       unsigned int index, const UnitPuppetPose* pose, int host)
{
    return spawn_type(fixture, local, index, pose, host ? SPAWN_HOST : SPAWN_FIXTURE);
}

/* Spawns a copy that predicts local hits cosmetically and is otherwise inert. */
enum UnitResult unit_combat_spawn_predictive(UnitCombat* fixture, const Snapshot* local,
                                             unsigned int index, const UnitPuppetPose* pose)
{
    return spawn_type(fixture, local, index, pose, SPAWN_PREDICTIVE);
}

/* Spawns the default test monster (VID50) 80 units from the local player. */
enum UnitResult unit_combat_spawn(UnitCombat* fixture, const Snapshot* local)
{
    if (!local)
        return UNIT_REFUSED;
    __try
    {
        uintptr_t source = stock_vid(local->game, MONSTER_VID_MONSTER1);
        uintptr_t child = 0;
        if (source)
            child = *(uintptr_t*)(source + STEAM_VID_LINKED_OFFSET);
        if (!supported(child, MONSTER_VID_MONSTER1_PART, STEAM_PUPPET_PART_CLASS))
            return UNIT_REFUSED;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return UNIT_REFUSED;
    }
    UnitPuppetPose pose = {0};
    pose.x = local->x + COMBAT_TEST_SPAWN_OFFSET;
    pose.y = local->y;
    pose.z = local->z;
    pose.direction = local->direction;
    return unit_combat_spawn_type(fixture, local, MONSTER_VID_MONSTER1, &pose, 0);
}

/* Records a foreign map entity in the spawn baseline, or counts it as
   unexpected when it was not there before the spawn. Returns 0 when full. */
static int track_foreign_entity(UnitCombat* fixture, uintptr_t e, uintptr_t vid,
                                UnitCombatSample* s)
{
    if (fixture->baseline.capturing)
    {
        if (fixture->baseline.count >= COMBAT_BASELINE_LIMIT)
            return 0;
        fixture->baseline.entries[fixture->baseline.count].entity = e;
        fixture->baseline.entries[fixture->baseline.count++].vid = vid;
        return 1;
    }
    for (unsigned int j = 0; j < fixture->baseline.count; ++j)
    {
        if (fixture->baseline.entries[j].entity == e && fixture->baseline.entries[j].vid == vid)
            return 1;
    }
    ++s->unexpected_entities;
    return 1;
}

/* Caller in SEH. Counts an entity created from one of the cloned forms.
   A UNIT form becomes the root. Returns 0 on a fault. */
static int count_form_entity(const UnitCombat* fixture, uintptr_t e, int form, UnitCombatSample* s)
{
    ++s->private_entities;
    unsigned int class_id = fixture->graph.classes[form];
    uintptr_t vt = shared.base + entity_class_vtable_rva(class_id);
    if (*(uintptr_t*)e != vt || s->private_entities > COMBAT_ENTITY_LIMIT)
        return 0;
    if (class_id != STEAM_UNIT_CLASS)
        return 1;

    /* A second UNIT may replace a root only while that root still has the spawn form. */
    int root_from_spawn =
        s->root && *(uintptr_t*)(s->root + STEAM_ENTITY_VID_OFFSET) == (uintptr_t)fixture->root_vid;
    if (s->root && !root_from_spawn && s->root != e)
        return 0;
    s->root = e;
    return 1;
}

/* Caller in SEH. Sorts one map entity into the sample. Returns 0 on a fault. */
static int count_entity(UnitCombat* fixture, uintptr_t e, UnitCombatSample* s)
{
    uintptr_t vid = *(uintptr_t*)(e + STEAM_ENTITY_VID_OFFSET);
    if (vid == (uintptr_t)fixture->root_vid)
    {
        ++s->private_entities;
        s->root = e;
        return 1;
    }
    if (vid == (uintptr_t)fixture->child_vid)
    {
        ++s->private_entities;
        s->child = e;
        return 1;
    }
    int form = private_form(fixture, vid);
    if (form >= 0)
        return count_form_entity(fixture, e, form, s);

    ++s->other_entities;
    if (owned_descriptor(vid))
        return 1;
    return track_foreign_entity(fixture, e, vid, s);
}

/* Caller in SEH. True when child is registered in the map as the private
   part of root, with the part descriptor its current form links to. A list
   fault also returns 0. */
static int linked_part_valid(uintptr_t game, uintptr_t root, uintptr_t child, uintptr_t expected)
{
    for (unsigned int cat = 0; cat < STEAM_TICK_ENTITY_LIST_COUNT; ++cat)
    {
        uintptr_t* items;
        unsigned int n;
        if (!world_list_read(game, cat, &items, &n))
            return 0;
        for (unsigned int j = 0; j < n; ++j)
        {
            if (items[j] != child)
                continue;
            if (*(uintptr_t*)(child + STEAM_ENTITY_VID_OFFSET) != expected)
                return 0;
            return is_part_of(child, root);
        }
    }
    return 0;
}

/* Caller in SEH. Follows the root's current form (a native form change
   counts as a new revision) and its linked part. Returns 0 on a fault. */
static int follow_root_form(UnitCombat* fixture, uintptr_t game, UnitCombatSample* s)
{
    uintptr_t vid = *(uintptr_t*)(s->root + STEAM_ENTITY_VID_OFFSET);
    unsigned int current = *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET);
    unsigned int animation = *(unsigned int*)(s->root + STEAM_ENTITY_ANIM_OFFSET);
    int pending = (fixture->host || fixture->predictive) && mp_monster_armored(current) &&
                  animation == STEAM_ANIM_DEATH;
    if (current != fixture->form.current_vid || pending != fixture->form.transitioning)
    {
        ++fixture->form.revision;
        ++fixture->form.transitions;
    }
    fixture->form.current_vid = current;
    fixture->form.transitioning = pending;
    fixture->root = s->root;

    uintptr_t child = *(uintptr_t*)(s->root + STEAM_ENTITY_CHILD_OFFSET);
    if (child)
    {
        /* Verify current private linked identity through map membership
           before exposing it. An old armor part is not this successor. */
        uintptr_t expected = *(uintptr_t*)(vid + STEAM_VID_LINKED_OFFSET);
        if (!linked_part_valid(game, s->root, child, expected))
            return 0;
    }
    s->child = child;
    fixture->child = child;
    return 1;
}

/* Caller in SEH. True when the borrowed stock descriptors still hold the
   bytes saved at spawn time. */
static int sources_unchanged(const UnitCombat* fixture, uintptr_t game)
{
    if (memcmp((void*)fixture->sources.root, fixture->sources.root_before, STEAM_VID_SIZE))
        return 0;
    if (!fixture->sources.part)
        return 1;
    if (stock_vid(game, fixture->sources.part_index) != fixture->sources.part)
        return 0;
    return !memcmp((void*)fixture->sources.part, fixture->sources.part_before, STEAM_VID_SIZE);
}

/* Caller in SEH. Counts every map entity into s. Returns 0 on a fault. */
static int count_map_entities(UnitCombat* fixture, uintptr_t game, UnitCombatSample* s)
{
    for (unsigned int cat = 0; cat < STEAM_TICK_ENTITY_LIST_COUNT; ++cat)
    {
        uintptr_t* entries;
        unsigned int count;
        if (!world_list_read(game, cat, &entries, &count))
            return 0;
        for (unsigned int i = 0; i < count; ++i)
        {
            if (entries[i] && !count_entity(fixture, entries[i], s))
                return 0;
        }
    }
    return 1;
}

/* Counts the fixture's entities in the map; `compare_sources` also checks that the
   borrowed stock descriptors are unchanged. */
UnitCombatSample unit_combat_sample(UnitCombat* fixture, uintptr_t game, int compare_sources)
{
    UnitCombatSample s = {0};
    s.sources_unchanged = !compare_sources;
    if (!fixture)
    {
        s.fault = 1;
        return s;
    }
    __try
    {
        int world_valid = fixture->state != COMBAT_FIXTURE_OFF && game &&
                          *(uintptr_t*)game == shared.base + STEAM_GAME_VTABLE_RVA;
        if (!world_valid || !count_map_entities(fixture, game, &s))
        {
            s.fault = 1;
            return s;
        }
        if (s.root && !follow_root_form(fixture, game, &s))
        {
            s.fault = 1;
            return s;
        }

        s.current_vid = fixture->form.current_vid;
        s.form_revision = fixture->form.revision;
        s.transitions = fixture->form.transitions;
        s.transitioning = fixture->form.transitioning;

        int same_spawn_world =
            game == fixture->game && fixture->sources.root &&
            stock_vid(game, fixture->sources.root_index) == fixture->sources.root;
        if (compare_sources && same_spawn_world)
            s.sources_unchanged = sources_unchanged(fixture, game);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        s.fault = 1;
    }
    return s;
}

/* Caller in SEH. Finds the host or predictive fixture in world game that owns
   entity (its root, or any entity with one of its descriptors). */
static UnitCombat* find_live_fixture(uintptr_t game, uintptr_t entity)
{
    for (unsigned int i = 0; i < UNIT_COMBAT_CAPACITY; ++i)
    {
        UnitCombat* fixture = &fixtures[i];
        if (fixture->game != game || (!fixture->host && !fixture->predictive))
            continue;
        uintptr_t identity = *(uintptr_t*)(entity + STEAM_ENTITY_VID_OFFSET);
        if (fixture->root == entity || identity == (uintptr_t)fixture->root_vid)
            return fixture;
        if (private_form(fixture, identity) >= 0)
            return fixture;
    }
    return NULL;
}

/* Caller in SEH, just before native CHANGE_VID to the frozen form. Clones the
   frozen form of the current origin. Returns 1 prepared, 0 not ours, -1 fault. */
static enum UnitResult prepare_frozen_form(UnitCombat* fixture, uintptr_t game,
                                           unsigned int requested_vid, uintptr_t* private_vid)
{
    if (!mp_monster_origin(fixture->form.current_vid))
        return UNIT_REFUSED;
    if (requested_vid != fixture->form.current_vid + MONSTER_VID_FROZEN_DELTA)
        return UNIT_REFUSED;

    uintptr_t source = stock_vid(game, requested_vid);
    if (!supported(source, requested_vid, STEAM_UNIT_CLASS))
        return UNIT_FAULT;
    if (!clone_form(fixture, game, source, requested_vid, private_vid))
        return UNIT_FAULT;
    return UNIT_OK;
}

/* Caller in SEH. Reports the settled form of entity. Returns 1 or -1 fault. */
static enum UnitResult describe_settled_form(UnitCombat* fixture, uintptr_t game, uintptr_t entity,
                                             uintptr_t* private_vid, unsigned int* current_vid,
                                             unsigned int* revision, int* transitioning)
{
    UnitCombatSample sample = unit_combat_sample(fixture, game, 0);
    if (sample.fault)
        return UNIT_FAULT;
    /* A destructor can run after a successor has already entered
       the map; retain the previous armor transition for that call. */
    uintptr_t descriptor = *(uintptr_t*)(entity + STEAM_ENTITY_VID_OFFSET);
    unsigned int index = *(unsigned int*)(descriptor + STEAM_VID_INDEX_OFFSET);
    *private_vid = descriptor;
    *current_vid = index;
    *revision = sample.form_revision;
    unsigned int animation = *(unsigned int*)(entity + STEAM_ENTITY_ANIM_OFFSET);
    *transitioning = mp_monster_armored(index) && animation == STEAM_ANIM_DEATH;
    return UNIT_OK;
}

/* Form observer for the puppet hooks: prepares a private descriptor before CHANGE_VID,
   or describes the settled form. UNIT_REFUSED means the entity is not ours. */
enum UnitResult unit_combat_observe_entity(uintptr_t game, uintptr_t entity, uintptr_t* private_vid,
                                           unsigned int* current_vid, unsigned int* revision,
                                           int* transitioning, unsigned int requested_vid)
{
    enum UnitResult result = UNIT_REFUSED;
    __try
    {
        UnitCombat* fixture = find_live_fixture(game, entity);
        if (!fixture)
            result = UNIT_REFUSED;
        else if (requested_vid)
            result = prepare_frozen_form(fixture, game, requested_vid, private_vid);
        else
            result = describe_settled_form(fixture, game, entity, private_vid, current_vid,
                                           revision, transitioning);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        result = UNIT_FAULT;
    }
    return result;
}

/* Native class of one of the fixture's private descriptors, 0 if unknown. */
static unsigned int private_class(const UnitCombat* fixture, uintptr_t vid)
{
    if (vid == (uintptr_t)fixture->root_vid)
        return STEAM_UNIT_CLASS;
    if (vid == (uintptr_t)fixture->child_vid)
        return STEAM_PUPPET_PART_CLASS;
    int i = private_form(fixture, vid);
    return i >= 0 ? fixture->graph.classes[i] : 0u;
}

/* Caller in SEH. True when `entity` is in a map list, has one of our private descriptors
   and the vtable of that descriptor's class. */
static int private_registered(const UnitCombat* fixture, uintptr_t game, uintptr_t entity)
{
    for (unsigned int cat = 0; cat < STEAM_TICK_ENTITY_LIST_COUNT; ++cat)
    {
        uintptr_t* entries;
        unsigned int n;
        if (!world_list_read(game, cat, &entries, &n))
            return 0;
        for (unsigned int i = 0; i < n; ++i)
        {
            if (entries[i] != entity)
                continue;
            unsigned int c =
                private_class(fixture, *(uintptr_t*)(entity + STEAM_ENTITY_VID_OFFSET));
            uintptr_t expected = shared.base + entity_class_vtable_rva(c);
            return c && *(uintptr_t*)entity == expected;
        }
    }
    return 0;
}

/* Caller in SEH. True when private entity e is registered and linked to its
   part and parent exactly as the private descriptors say. */
static int private_links_valid(const UnitCombat* fixture, uintptr_t game, uintptr_t e)
{
    if (!private_registered(fixture, game, e))
        return 0;
    uintptr_t child = *(uintptr_t*)(e + STEAM_ENTITY_CHILD_OFFSET);
    if (child)
    {
        if (!private_registered(fixture, game, child))
            return 0;
        if (*(uintptr_t*)(child + STEAM_ENTITY_PARENT_OFFSET) != e)
            return 0;
        if (*(uintptr_t*)(child + STEAM_ENTITY_CHILD_OFFSET))
            return 0;
    }

    uintptr_t parent = *(uintptr_t*)(e + STEAM_ENTITY_PARENT_OFFSET);
    if (!parent)
        return 1;
    if (!private_registered(fixture, game, parent))
        return 0;
    return *(uintptr_t*)(parent + STEAM_ENTITY_CHILD_OFFSET) == e;
}

/* Caller in SEH. Finds one of our entities without a parent (a part goes
   with its parent); *found stays 0 when none is left. Returns 0 on a fault. */
static int find_form_root(const UnitCombat* fixture, uintptr_t game, uintptr_t* found)
{
    for (unsigned int cat = 0; cat < STEAM_TICK_ENTITY_LIST_COUNT; ++cat)
    {
        uintptr_t* entries;
        unsigned int n;
        if (!world_list_read(game, cat, &entries, &n))
            return 0;
        for (unsigned int j = 0; j < n; ++j)
        {
            uintptr_t e = entries[j];
            if (!e || !private_class(fixture, *(uintptr_t*)(e + STEAM_ENTITY_VID_OFFSET)))
                continue;
            if (!private_links_valid(fixture, game, e))
                return 0;
            if (!*(uintptr_t*)(e + STEAM_ENTITY_PARENT_OFFSET))
            {
                *found = e;
                return 1;
            }
        }
    }
    return 1;
}

/* Switches off creation links, then deletes the fixture's root entities one by one. Returns 1
   only when the map holds no private entity afterwards. */
static int remove_forms(UnitCombat* fixture, uintptr_t game)
{
    /* Disable creation before the first deleting destructor. Linked children
       retain their native recursive ownership, all other graph roots are found
       again by private descriptor identity after each deletion. */
    const size_t creations_size = STEAM_ANIMATION_COUNT * sizeof(uintptr_t);
    memset(fixture->root_vid + STEAM_VID_CREATIONS_OFFSET, 0, creations_size);
    memset(fixture->child_vid + STEAM_VID_CREATIONS_OFFSET, 0, creations_size);
    for (unsigned int i = 0; i < fixture->graph.count; ++i)
        memset(fixture->graph.vids[i] + STEAM_VID_CREATIONS_OFFSET, 0, creations_size);

    for (unsigned int attempt = 0; attempt < COMBAT_ENTITY_LIMIT; ++attempt)
    {
        uintptr_t found = 0;
        if (!find_form_root(fixture, game, &found))
            return 0;
        if (!found)
            break;

        /* A UNIT uses the hooked-through destructor; other classes their vtable's first slot. */
        unsigned int c = private_class(fixture, *(uintptr_t*)(found + STEAM_ENTITY_VID_OFFSET));
        ActorDestroy destroy = shared.destroy;
        if (c != STEAM_UNIT_CLASS)
            destroy = *(ActorDestroy*)(shared.base + entity_class_vtable_rva(c));
        if (!destroy)
            return 0;
        destroy((void*)found, NULL, 1);
    }

    UnitCombatSample after = unit_combat_sample(fixture, game, 0);
    return !after.fault && !after.private_entities;
}

/* The fixture owns no entity in any world any more; it may spawn again. */
static void mark_fixture_ready(UnitCombat* fixture)
{
    fixture->root = 0;
    fixture->child = 0;
    fixture->game = 0;
    fixture->state = COMBAT_FIXTURE_READY;
}

/* Caller in SEH. Deletes the root (with its part), or a part left alone by a
   failed factory. Returns 0 when the entities are not the expected shape. */
/* Caller in SEH. True when the sample holds just a UNIT root and, optionally, its part. */
static int root_with_part(const UnitCombatSample* s)
{
    if (!s->root || s->private_entities > 2u)
        return 0;
    if (*(uintptr_t*)s->root != shared.base + STEAM_UNIT_VTABLE_RVA)
        return 0;
    if (*(uintptr_t*)(s->root + STEAM_ENTITY_CHILD_OFFSET) != s->child)
        return 0;
    return !s->child || is_part_of(s->child, s->root);
}

static int destroy_spawned_entities(const UnitCombatSample* s)
{
    if (root_with_part(s))
    {
        shared.destroy((void*)s->root, NULL, 1);
        return 1;
    }

    /* Otherwise only a lone part (no root, no parent) is removable. */
    if (!s->child || s->root || s->private_entities != 1u)
        return 0;
    if (!is_part_of(s->child, 0))
        return 0;
    /* A factory failure can leave only its private part registered.
       Its deleting destructor is still validated by unit_puppet_bind. */
    ActorDestroy destroy_part =
        (ActorDestroy)((uintptr_t*)(shared.base + STEAM_SPRITE_VTABLE_RVA))[0];
    destroy_part((void*)s->child, NULL, 1);
    return 1;
}

/* One guarded removal attempt. A fault keeps ownership, so stop stays refused. */
enum UnitResult unit_combat_remove(UnitCombat* fixture, uintptr_t game)
{
    if (!fixture)
        return UNIT_REFUSED;
    if (fixture->state == COMBAT_FIXTURE_OFF || !fixture->game)
        return UNIT_OK;
    UnitCombatSample s = unit_combat_sample(fixture, game, 0);
    if (s.fault)
        return UNIT_REFUSED;
    if (!s.root && !s.child && !s.private_entities)
    {
        mark_fixture_ready(fixture);
        return UNIT_OK;
    }
    /* A native fault is never retried in this world. */
    if (fixture->state == COMBAT_FIXTURE_ABANDONED)
        return UNIT_REFUSED;
    fixture->state = COMBAT_FIXTURE_ABANDONED;
    __try
    {
        if (fixture->graph.count)
        {
            if (!remove_forms(fixture, game))
                return UNIT_REFUSED;
            mark_fixture_ready(fixture);
            return UNIT_OK;
        }
        if (!destroy_spawned_entities(&s))
            return UNIT_REFUSED;
        s = unit_combat_sample(fixture, game, 0);
        if (s.fault || s.root || s.child || s.private_entities)
            return UNIT_REFUSED;
        mark_fixture_ready(fixture);
        return UNIT_OK;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return UNIT_REFUSED;
    }
}

/* Releases the module; refuses while any fixture still owns a world. */
enum UnitResult unit_combat_stop(void)
{
    for (unsigned int i = 0; i < UNIT_COMBAT_CAPACITY; ++i)
    {
        if (fixtures[i].game)
            return UNIT_REFUSED;
    }
    for (unsigned int i = 0; i < UNIT_COMBAT_CAPACITY; ++i)
        fixtures[i].state = COMBAT_FIXTURE_OFF;
    shared.initialized = 0;
    return UNIT_OK;
}
