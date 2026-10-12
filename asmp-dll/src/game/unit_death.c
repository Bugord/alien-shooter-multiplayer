#include <string.h>
#include <math.h>
#include "unit_death.h"
#include "../../../common/src/monster_vids.h"
#include "entity_class.h"
#include "world_list.h"

#define DEATH_VID_COUNT    9u
#define DEATH_GRAPH_LIMIT  32u
#define DEATH_ENTITY_LIMIT 128u
enum DeathState
{
    DEATH_OFF,
    DEATH_READY,
    DEATH_OWNED,
    DEATH_FAULT,
    DEATH_ABANDONED
};

static const unsigned int indices[DEATH_VID_COUNT] = {51, 52, 916, 910, 110, 151, 152, 120, 108};
static const unsigned int legacy_classes[DEATH_VID_COUNT] = {5, 8, 8, 5, 8, 5, 8, 8, 9};
/* Exact observed DAG. Admit no new edge, including another approved VID. */
static const struct
{
    unsigned int from, animation, to, count;
} edges[] = {{0, 14, 2, 1}, {0, 15, 1, 1}, {1, 15, 2, 1}, {2, 14, 7, 8}, {3, 14, 4, 2},
             {3, 15, 5, 8}, {4, 14, 7, 8}, {5, 12, 7, 1}, {5, 15, 6, 1}, {6, 14, 7, 1}};
/* One death or armor-debris presentation: private VID copies of the visual
   graph and the native effects created from them. Public functions receive the
   slot explicitly. */
struct UnitDeath
{
    enum DeathState state;
    uintptr_t base, game;
    ActorCreate create;
    ActorDestroy cannon_destroy, terrain_destroy, part_destroy;
    unsigned char vids[DEATH_GRAPH_LIMIT][STEAM_VID_SIZE];
    unsigned int count, classes[DEATH_GRAPH_LIMIT], linked[DEATH_GRAPH_LIMIT];
    unsigned int corpse, gib;
};

static UnitDeath slots[UNIT_DEATH_SLOT_COUNT];
static int initialized;

UnitDeath* unit_death_slot(unsigned int index)
{
    if (index >= UNIT_DEATH_SLOT_COUNT)
        return NULL;
    return &slots[index];
}

static int private_index(const UnitDeath* death, uintptr_t vid)
{
    for (unsigned int i = 0; i < death->count; ++i)
        if (vid == (uintptr_t)death->vids[i])
            return (int)i;
    return -1;
}

static uintptr_t vtable(const UnitDeath* death, unsigned int i)
{
    return death->base + entity_class_vtable_rva(death->classes[i]);
}

enum UnitResult unit_death_init(uintptr_t base, ActorCreate create)
{
    static const unsigned char cannon[] = {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0xE8, 0x55};
    static const unsigned char terrain[] = {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0xE8, 0x15};
    if (initialized || !base || !create)
        return UNIT_REFUSED;
    __try
    {
        if (memcmp((void*)(base + STEAM_CANNON_DESTROY_RVA), cannon, sizeof(cannon)) ||
            memcmp((void*)(base + STEAM_TERRAIN_DESTROY_RVA), terrain, sizeof(terrain)) ||
            *(uintptr_t*)(base + STEAM_CANNON_VTABLE_RVA) != base + STEAM_CANNON_DESTROY_RVA ||
            *(uintptr_t*)(base + STEAM_TERRAIN_VTABLE_RVA) != base + STEAM_TERRAIN_DESTROY_RVA)
            return UNIT_REFUSED;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return UNIT_REFUSED;
    }
    UnitDeathEngine api = {create, (ActorDestroy)(base + STEAM_CANNON_DESTROY_RVA),
                           (ActorDestroy)(base + STEAM_TERRAIN_DESTROY_RVA),
                           (ActorDestroy)(base + STEAM_SPRITE_DESTROY_RVA)};
    return unit_death_initialize(base, &api);
}

enum UnitResult unit_death_initialize(uintptr_t base, const UnitDeathEngine* api)
{
    if (initialized || !base || !api || !api->create || !api->cannon_destroy ||
        !api->terrain_destroy || !api->part_destroy)
        return UNIT_REFUSED;
    memset(slots, 0, sizeof(slots));
    for (unsigned int i = 0; i < UNIT_DEATH_SLOT_COUNT; ++i)
    {
        slots[i].base = base;
        slots[i].create = api->create;
        slots[i].cannon_destroy = api->cannon_destroy;
        slots[i].terrain_destroy = api->terrain_destroy;
        slots[i].part_destroy = api->part_destroy;
        slots[i].state = DEATH_READY;
    }
    initialized = 1;
    return UNIT_OK;
}

#define DEATH_EDGE_COUNT (sizeof(edges) / sizeof(edges[0]))

/* Caller in SEH. True when a stock VID can be copied: a plain native factory
   entry without a service object or special factory flag. */
static int plain_factory_vid(uintptr_t source)
{
    return *(unsigned int*)(source + STEAM_VID_CATEGORY_OFFSET) < STEAM_TICK_ENTITY_LIST_COUNT &&
           !*(uintptr_t*)(source + STEAM_VID_SERVICE_OFFSET) &&
           !(*(unsigned int*)(source + STEAM_VID_RESOURCE_FLAGS_OFFSET) &
             STEAM_VID_SPECIAL_FACTORY_FLAG);
}

/* Makes a private VID copy purely visual: no counters, scripts, collision,
   health or damage. */
static void make_inert(unsigned char* d)
{
    memset(d + STEAM_VID_COUNTERS_OFFSET, 0, STEAM_VID_COUNTERS_SIZE);
    *(int*)(d + STEAM_VID_DELETE_SCRIPT_OFFSET) = -1;
    *(int*)(d + STEAM_VID_DAMAGE_SCRIPT_OFFSET) = -1;
    *(int*)(d + STEAM_VID_COLLISION_SCRIPT_OFFSET) = -1;
    *(unsigned int*)(d + STEAM_VID_COLLISION_MASK_OFFSET) = 0;
    *(unsigned int*)(d + STEAM_VID_HEALTH_OFFSET) = 0;
    *(int*)(d + STEAM_VID_DAMAGE_OFFSET) = 0;
}

/* Caller in SEH. Copies the legacy VID50 graph and checks it is exactly the
   observed one: classes, the linked part and every creation edge. */
static int copy_legacy_sources(UnitDeath* death, uintptr_t game, uintptr_t* sources)
{
    for (unsigned int i = 0; i < DEATH_VID_COUNT; ++i)
    {
        uintptr_t s = *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + indices[i] * 4u);
        if (!s || *(unsigned int*)(s + STEAM_VID_INDEX_OFFSET) != indices[i] ||
            *(unsigned int*)(s + STEAM_VID_CLASS_OFFSET) != legacy_classes[i] ||
            !plain_factory_vid(s))
            return 0;
        sources[i] = s;
        memcpy(death->vids[i], (void*)s, STEAM_VID_SIZE);
    }
    if (*(uintptr_t*)(sources[0] + STEAM_VID_LINKED_OFFSET) != sources[8])
        return 0;
    for (unsigned int e = 0; e < DEATH_EDGE_COUNT; ++e)
    {
        uintptr_t from = sources[edges[e].from];
        unsigned int offset = edges[e].animation * 4u;
        if (*(uintptr_t*)(from + STEAM_VID_CREATIONS_OFFSET + offset) != sources[edges[e].to] ||
            *(unsigned int*)(from + STEAM_VID_CREATION_COUNT_OFFSET + offset) != edges[e].count)
            return 0;
    }
    return 1;
}

/* Points one legacy copy's link and creations at the private copies.
   Returns 0 for any link or edge outside the observed graph. */
static int relink_legacy_copy(UnitDeath* death, unsigned int i, const uintptr_t* sources)
{
    unsigned char* d = death->vids[i];
    make_inert(d);
    uintptr_t link = *(uintptr_t*)(d + STEAM_VID_LINKED_OFFSET);
    if (link && !(i == 0 && link == sources[8]))
        return 0;
    *(uintptr_t*)(d + STEAM_VID_LINKED_OFFSET) = link ? (uintptr_t)death->vids[8] : 0;
    if (i == 8u)
        memset(d + STEAM_VID_CREATIONS_OFFSET, 0, STEAM_ANIMATION_COUNT * 4u);

    for (unsigned int a = 0; a < STEAM_ANIMATION_COUNT; ++a)
    {
        *(int*)(d + STEAM_VID_ANIMATION_SCRIPTS_OFFSET + a * 4u) = -1;
        uintptr_t target = *(uintptr_t*)(d + STEAM_VID_CREATIONS_OFFSET + a * 4u);
        if (!target)
            continue;
        int count = *(int*)(d + STEAM_VID_CREATION_COUNT_OFFSET + a * 4u);
        unsigned int edge;
        for (edge = 0; edge < DEATH_EDGE_COUNT; ++edge)
        {
            if (edges[edge].from == i && edges[edge].animation == a &&
                target == sources[edges[edge].to] && count == (int)edges[edge].count)
                break;
        }
        if (edge >= DEATH_EDGE_COUNT)
            return 0;
        *(uintptr_t*)(d + STEAM_VID_CREATIONS_OFFSET + a * 4u) =
            (uintptr_t)death->vids[edges[edge].to];
    }
    return 1;
}

/* Caller in SEH. Resolve every link through the loaded table before cloning.
   Only the observed visual graph is admitted, with bounded creation counts. */
static int prepare(UnitDeath* death, uintptr_t game)
{
    uintptr_t sources[DEATH_VID_COUNT];
    death->count = DEATH_VID_COUNT;
    death->corpse = 0;
    death->gib = 5;
    for (unsigned int i = 0; i < DEATH_VID_COUNT; ++i)
    {
        death->classes[i] = legacy_classes[i];
        death->linked[i] = DEATH_GRAPH_LIMIT;
    }
    death->linked[0] = 8;
    if (!copy_legacy_sources(death, game, sources))
        return 0;
    for (unsigned int i = 0; i < DEATH_VID_COUNT; ++i)
    {
        if (!relink_legacy_copy(death, i, sources))
            return 0;
    }
    return 1;
}

#define DEATH_CREATION_LIMIT 16u
typedef struct DeathGraph
{
    uintptr_t sources[DEATH_GRAPH_LIMIT];
    unsigned int colors[DEATH_GRAPH_LIMIT], cost[DEATH_GRAPH_LIMIT];
    int armor;
} DeathGraph;

static int graph_visit(UnitDeath* death, uintptr_t game, uintptr_t source, DeathGraph* graph,
                       unsigned int* result, int linked_art);

/* Caller in SEH. True when a graph node is a stock visual VID (cannon, terrain
   or part class) with a behavior, loaded at its own table index. */
static int visual_node_admissible(uintptr_t game, uintptr_t source)
{
    unsigned int index = *(unsigned int*)(source + STEAM_VID_INDEX_OFFSET);
    unsigned int class_id = *(unsigned int*)(source + STEAM_VID_CLASS_OFFSET);
    if (index >= STEAM_STOCK_VID_LIMIT ||
        *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + index * 4u) != source)
        return 0;
    if (class_id != STEAM_CANNON_CLASS && class_id != STEAM_TERRAIN_CLASS &&
        class_id != STEAM_PUPPET_PART_CLASS)
        return 0;
    return *(uintptr_t*)(source + STEAM_VID_BEHAVIOR_OFFSET) && plain_factory_vid(source);
}

static void clear_creation(unsigned char* d, unsigned int animation)
{
    *(uintptr_t*)(d + STEAM_VID_CREATIONS_OFFSET + animation * 4u) = 0;
    *(unsigned int*)(d + STEAM_VID_CREATION_COUNT_OFFSET + animation * 4u) = 0;
}

/* Visits the linked part of node i (a class-9 SPRITE without a link of its
   own) and adds its cost to *total. */
static int visit_linked_part(UnitDeath* death, uintptr_t game, uintptr_t source, DeathGraph* graph,
                             unsigned int i, unsigned int* total)
{
    uintptr_t linked = *(uintptr_t*)(source + STEAM_VID_LINKED_OFFSET);
    if (!linked)
        return 1;
    unsigned int child;
    if (death->classes[i] == 9u || *(unsigned int*)(linked + STEAM_VID_CLASS_OFFSET) != 9u ||
        *(uintptr_t*)(linked + STEAM_VID_LINKED_OFFSET) ||
        !graph_visit(death, game, linked, graph, &child, 1))
        return 0;
    death->linked[i] = child;
    *(uintptr_t*)(death->vids[i] + STEAM_VID_LINKED_OFFSET) = (uintptr_t)death->vids[child];
    *total += graph->cost[child];
    return 1;
}

/* Visits what node i creates on animation a and points the private copy at
   it. Armor graphs drop the living successor UNIT. Keeps *total bounded. */
static int visit_creation(UnitDeath* death, uintptr_t game, uintptr_t source, DeathGraph* graph,
                          unsigned int i, unsigned int a, unsigned int* total)
{
    unsigned char* d = death->vids[i];
    uintptr_t target = *(uintptr_t*)(source + STEAM_VID_CREATIONS_OFFSET + a * 4u);
    if (!target)
        return 1;
    if (graph->armor && *(unsigned int*)(target + STEAM_VID_CLASS_OFFSET) == STEAM_UNIT_CLASS)
    {
        unsigned int successor = *(unsigned int*)(target + STEAM_VID_INDEX_OFFSET);
        if ((successor != MONSTER_VID_MONSTER2 && successor != MONSTER_VID_TANK) ||
            *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + successor * 4u) != target)
            return 0;
        clear_creation(d, a);
        return 1;
    }

    unsigned int count = *(unsigned int*)(source + STEAM_VID_CREATION_COUNT_OFFSET + a * 4u);
    unsigned int child;
    if (!count || count > DEATH_CREATION_LIMIT ||
        !graph_visit(death, game, target, graph, &child, 0) ||
        graph->cost[child] > (DEATH_ENTITY_LIMIT - *total) / count)
        return 0;
    *total += count * graph->cost[child];
    *(uintptr_t*)(d + STEAM_VID_CREATIONS_OFFSET + a * 4u) = (uintptr_t)death->vids[child];
    return 1;
}

/* Caller in SEH. Depth-first copy of a death graph into private VIDs.
 *result receives the node index of source; cycles are rejected. */
static int graph_visit(UnitDeath* death, uintptr_t game, uintptr_t source, DeathGraph* graph,
                       unsigned int* result, int linked_art)
{
    if (!source)
        return 0;
    unsigned int i;
    for (i = 0; i < death->count; ++i)
    {
        if (graph->sources[i] == source)
            break;
    }
    if (i < death->count)
    {
        if (graph->colors[i] != 2u)
            return 0; /* A back edge is a cycle. */
        *result = i;
        return 1;
    }
    if (death->count >= DEATH_GRAPH_LIMIT || !visual_node_admissible(game, source))
        return 0;

    i = death->count++;
    graph->sources[i] = source;
    graph->colors[i] = 1;
    death->classes[i] = *(unsigned int*)(source + STEAM_VID_CLASS_OFFSET);
    death->linked[i] = DEATH_GRAPH_LIMIT;
    unsigned char* d = death->vids[i];
    memcpy(d, (void*)source, STEAM_VID_SIZE);
    make_inert(d);
    unsigned int total = 1;
    if (!visit_linked_part(death, game, source, graph, i, &total))
        return 0;

    for (unsigned int a = 0; a < STEAM_ANIMATION_COUNT; ++a)
    {
        *(int*)(d + STEAM_VID_ANIMATION_SCRIPTS_OFFSET + a * 4u) = -1;
        /* A linked SPRITE is borrowed art only, just like scoped VID108. */
        if (linked_art)
        {
            clear_creation(d, a);
            continue;
        }
        if (!visit_creation(death, game, source, graph, i, a, &total))
            return 0;
    }
    graph->cost[i] = total;
    graph->colors[i] = 2;
    *result = i;
    return 1;
}

static int prepare_type(UnitDeath* death, uintptr_t game, unsigned int source_vid,
                        unsigned int animation, int armor, unsigned int* multiplicity)
{
    if (source_vid >= STEAM_STOCK_VID_LIMIT)
        return 0;
    uintptr_t source = *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + source_vid * 4u);
    if (!source || *(unsigned int*)(source + STEAM_VID_INDEX_OFFSET) != source_vid ||
        *(unsigned int*)(source + STEAM_VID_CLASS_OFFSET) != STEAM_UNIT_CLASS ||
        !plain_factory_vid(source))
        return 0;
    *multiplicity = *(unsigned int*)(source + STEAM_VID_CREATION_COUNT_OFFSET + animation * 4u);
    if (!*multiplicity || *multiplicity > 2u ||
        (*multiplicity != 1u && source_vid != 69u && source_vid != 79u && source_vid != 89u &&
         source_vid != 99u))
        return 0;
    uintptr_t root = *(uintptr_t*)(source + STEAM_VID_CREATIONS_OFFSET + animation * 4u);
    DeathGraph graph = {0};
    unsigned int root_index;
    graph.armor = armor;
    death->count = 0;
    death->corpse = animation == STEAM_ANIM_DEATH ? 0u : DEATH_GRAPH_LIMIT;
    death->gib = DEATH_GRAPH_LIMIT;
    if (armor && source_vid == MONSTER_VID_TANK_ARMORED && root &&
        *(unsigned int*)(root + STEAM_VID_INDEX_OFFSET) == MONSTER_VID_TANK &&
        *(unsigned int*)(root + STEAM_VID_CLASS_OFFSET) == STEAM_UNIT_CLASS &&
        *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + MONSTER_VID_TANK * 4u) == root)
    {
        /* Custom large armor feedback: borrow the medium shell's visual graph.
           Private copies omit its living successor; native90->80 stays intact. */
        return prepare_type(death, game, MONSTER_VID_MONSTER2_ARMORED, animation, armor,
                            multiplicity);
    }
    return graph_visit(death, game, root, &graph, &root_index, 0) && root_index == 0u &&
           graph.cost[root_index] <= DEATH_ENTITY_LIMIT / *multiplicity;
}

/* Caller in SEH. Check identity in a map list before following a child pointer. */
static int find_private(const UnitDeath* death, uintptr_t game, uintptr_t wanted,
                        unsigned int index)
{
    for (unsigned int c = 0; c < STEAM_TICK_ENTITY_LIST_COUNT; ++c)
    {
        uintptr_t* entries;
        unsigned int n;
        if (!world_list_read(game, c, &entries, &n))
            return 0;
        for (unsigned int j = 0; j < n; ++j)
            if (entries[j] == wanted)
                return *(uintptr_t*)wanted == vtable(death, index) &&
                       *(uintptr_t*)(wanted + STEAM_ENTITY_VID_OFFSET) ==
                           (uintptr_t)death->vids[index];
    }
    return 0;
}

/* Caller in SEH. Checks that private entity e (node i) is linked to its part
   and parent exactly as the private graph says. */
static int private_links_valid(const UnitDeath* death, uintptr_t game, uintptr_t e, unsigned int i)
{
    uintptr_t child = *(uintptr_t*)(e + STEAM_ENTITY_CHILD_OFFSET);
    uintptr_t parent = *(uintptr_t*)(e + STEAM_ENTITY_PARENT_OFFSET);
    unsigned int link = death->linked[i];
    if (child && (link >= death->count || !find_private(death, game, child, link) ||
                  *(uintptr_t*)(child + STEAM_ENTITY_PARENT_OFFSET) != e ||
                  *(uintptr_t*)(child + STEAM_ENTITY_CHILD_OFFSET)))
        return 0;
    if (!parent)
        return 1;

    unsigned int parent_index;
    for (parent_index = 0; parent_index < death->count; ++parent_index)
    {
        if (death->linked[parent_index] == i && find_private(death, game, parent, parent_index))
            break;
    }
    return parent_index != death->count && *(uintptr_t*)(parent + STEAM_ENTITY_CHILD_OFFSET) == e;
}

/* Caller in SEH. Counts one map entity into s if it is one of ours.
   Returns 0 on a fault (wrong vtable, broken links or too many entities). */
static int count_private_entity(const UnitDeath* death, uintptr_t game, uintptr_t e,
                                UnitDeathSample* s)
{
    int i = private_index(death, *(uintptr_t*)(e + STEAM_ENTITY_VID_OFFSET));
    if (i < 0)
        return 1;
    if (*(uintptr_t*)e != vtable(death, (unsigned int)i) || ++s->entities > DEATH_ENTITY_LIMIT)
        return 0;
    if (!private_links_valid(death, game, e, (unsigned int)i))
        return 0;
    if ((unsigned int)i == death->corpse)
        ++s->corpses;
    if ((unsigned int)i == death->gib ||
        (death->gib == DEATH_GRAPH_LIMIT && death->classes[i] == STEAM_CANNON_CLASS && i != 0))
        ++s->gibs;
    return 1;
}

UnitDeathSample unit_death_sample(UnitDeath* death, uintptr_t game)
{
    UnitDeathSample s = {0};
    if (!death || death->state == DEATH_OFF || !death->game)
        return s;
    __try
    {
        if (!game || *(uintptr_t*)game != death->base + STEAM_GAME_VTABLE_RVA)
        {
            s.fault = 1;
            return s;
        }
        for (unsigned int c = 0; c < STEAM_TICK_ENTITY_LIST_COUNT; ++c)
        {
            uintptr_t* entries;
            unsigned int n;
            if (!world_list_read(game, c, &entries, &n))
            {
                s.fault = 1;
                return s;
            }
            for (unsigned int j = 0; j < n; ++j)
            {
                if (entries[j] && !count_private_entity(death, game, entries[j], &s))
                {
                    s.fault = 1;
                    return s;
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        s.fault = 1;
    }
    return s;
}

/* True when a ready, empty slot may play a death animation at pose p. */
static int play_request_valid(const UnitDeath* death, const UnitPuppetPose* p,
                              unsigned int animation)
{
    return death && death->state == DEATH_READY && !death->game && p &&
           (animation == STEAM_ANIM_DEATH || animation == STEAM_ANIM_GIB_DEATH) && isfinite(p->x) &&
           isfinite(p->y) && isfinite(p->z) && p->direction <= 255u;
}

enum UnitResult unit_death_play(UnitDeath* death, uintptr_t game, const UnitPuppetPose* p,
                                unsigned int animation)
{
    if (!play_request_valid(death, p, animation))
        return UNIT_REFUSED;
    __try
    {
        if (!game || *(uintptr_t*)game != death->base + STEAM_GAME_VTABLE_RVA ||
            !prepare(death, game))
            return UNIT_REFUSED;
        death->game = game;
        death->state = DEATH_OWNED;
        void* e =
            death->create((void*)game, NULL, death->vids[animation == STEAM_ANIM_DEATH ? 0 : 3],
                          p->x, p->y, p->z, p->direction, NULL);
        UnitDeathSample s = unit_death_sample(death, game);
        unsigned int root_index = animation == STEAM_ANIM_DEATH ? 0u : 3u;
        if (!e || s.fault || !s.entities || !find_private(death, game, (uintptr_t)e, root_index))
        {
            death->state = DEATH_FAULT;
            return UNIT_FAULT;
        }
        return UNIT_OK;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        death->state = death->game ? DEATH_FAULT : DEATH_READY;
        return UNIT_FAULT;
    }
}

static enum UnitResult play_type(UnitDeath* death, uintptr_t game, const UnitPuppetPose* p,
                                 unsigned int animation, unsigned int source_vid, int armor)
{
    if (!play_request_valid(death, p, animation))
        return UNIT_REFUSED;
    __try
    {
        unsigned int multiplicity;
        if (!game || *(uintptr_t*)game != death->base + STEAM_GAME_VTABLE_RVA ||
            !prepare_type(death, game, source_vid, animation, armor, &multiplicity))
            return UNIT_REFUSED;
        if (!death->count)
            return UNIT_OK;
        death->game = game;
        death->state = DEATH_OWNED;
        void* e = NULL;
        for (unsigned int i = 0; i < multiplicity; ++i)
        {
            e = death->create((void*)game, NULL, death->vids[0], p->x, p->y, p->z, p->direction,
                              NULL);
            if (!e || !find_private(death, game, (uintptr_t)e, 0u))
            {
                death->state = DEATH_FAULT;
                return UNIT_FAULT;
            }
        }
        UnitDeathSample s = unit_death_sample(death, game);
        if (!e || s.fault || !s.entities || !find_private(death, game, (uintptr_t)e, 0u))
        {
            death->state = DEATH_FAULT;
            return UNIT_FAULT;
        }
        return UNIT_OK;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        death->state = death->game ? DEATH_FAULT : DEATH_READY;
        return UNIT_FAULT;
    }
}

enum UnitResult unit_death_play_type(UnitDeath* death, uintptr_t game, const UnitPuppetPose* p,
                                     unsigned int animation, unsigned int source_vid)
{
    return play_type(death, game, p, animation, source_vid, 0);
}

enum UnitResult unit_death_play_armor(UnitDeath* death, uintptr_t game, const UnitPuppetPose* p,
                                      unsigned int source_vid)
{
    if (!mp_monster_armored(source_vid))
        return UNIT_REFUSED;
    return play_type(death, game, p, STEAM_ANIM_DEATH, source_vid, 1);
}

/* Caller in SEH. Finds one of our entities without a parent (a part goes with
   its parent). *found stays 0 when none is left. Returns 0 on a list fault. */
static int find_private_root(const UnitDeath* death, uintptr_t game, uintptr_t* found, int* index)
{
    for (unsigned int c = 0; c < STEAM_TICK_ENTITY_LIST_COUNT; ++c)
    {
        uintptr_t* entries;
        unsigned int n;
        if (!world_list_read(game, c, &entries, &n))
            return 0;
        for (unsigned int j = 0; j < n; ++j)
        {
            uintptr_t e = entries[j];
            if (!e)
                continue;
            int i = private_index(death, *(uintptr_t*)(e + STEAM_ENTITY_VID_OFFSET));
            if (i >= 0 && !*(uintptr_t*)(e + STEAM_ENTITY_PARENT_OFFSET))
            {
                *found = e;
                *index = i;
                return 1;
            }
        }
    }
    return 1;
}

/* Caller in SEH. Deletes one entity through the deleting destructor of its
   native class. */
static void destroy_private_entity(const UnitDeath* death, uintptr_t entity, unsigned int index)
{
    unsigned int class_id = death->classes[index];
    ActorDestroy destroy = death->part_destroy;
    if (class_id == STEAM_CANNON_CLASS)
        destroy = death->cannon_destroy;
    else if (class_id == STEAM_TERRAIN_CLASS)
        destroy = death->terrain_destroy;
    destroy((void*)entity, NULL, 1);
}

enum UnitResult unit_death_remove(UnitDeath* death, uintptr_t game)
{
    if (!death)
        return UNIT_REFUSED;
    if (death->state == DEATH_OFF || !death->game)
        return UNIT_OK;
    UnitDeathSample s = unit_death_sample(death, game);
    if (s.fault)
        return UNIT_REFUSED;
    if (!s.entities)
    {
        death->game = 0;
        death->state = DEATH_READY;
        return UNIT_OK;
    }
    if (death->state == DEATH_ABANDONED)
        return UNIT_REFUSED;
    death->state = DEATH_ABANDONED;
    __try
    {
        /* No update can create successors during game-thread forced removal. */
        for (unsigned int i = 0; i < death->count; ++i)
            memset(death->vids[i] + STEAM_VID_CREATIONS_OFFSET, 0, STEAM_ANIMATION_COUNT * 4u);
        for (unsigned int attempt = 0; attempt < DEATH_ENTITY_LIMIT; ++attempt)
        {
            uintptr_t found = 0;
            int index = -1;
            if (!find_private_root(death, game, &found, &index))
                return UNIT_REFUSED;
            if (!found)
                break;
            destroy_private_entity(death, found, (unsigned int)index);
        }
        s = unit_death_sample(death, game);
        if (s.fault || s.entities)
            return UNIT_REFUSED;
        death->game = 0;
        death->state = DEATH_READY;
        return UNIT_OK;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return UNIT_REFUSED;
    }
}

enum UnitResult unit_death_stop(void)
{
    for (unsigned int i = 0; i < UNIT_DEATH_SLOT_COUNT; ++i)
        if (slots[i].game)
            return UNIT_REFUSED;
    for (unsigned int i = 0; i < UNIT_DEATH_SLOT_COUNT; ++i)
        slots[i].state = DEATH_OFF;
    initialized = 0;
    return UNIT_OK;
}
