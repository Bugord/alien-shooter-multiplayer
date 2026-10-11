#include <string.h>
#include <math.h>
#include "unit_death.h"

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
typedef struct DeathSlot
{
    enum DeathState state;
    uintptr_t base, game;
    ActorCreate create;
    ActorDestroy cannon_destroy, terrain_destroy, part_destroy;
    unsigned char vids[DEATH_GRAPH_LIMIT][STEAM_VID_SIZE];
    unsigned int count, classes[DEATH_GRAPH_LIMIT], linked[DEATH_GRAPH_LIMIT];
    unsigned int corpse, gib;
} DeathSlot;
static DeathSlot slots[UNIT_DEATH_SLOT_COUNT];
static unsigned int selected;
static int initialized;
#define death slots[selected]
int unit_death_select(unsigned int slot)
{
    if (slot >= UNIT_DEATH_SLOT_COUNT)
        return 0;
    selected = slot;
    return 1;
}
static int private_index(uintptr_t vid)
{
    for (unsigned int i = 0; i < death.count; ++i)
        if (vid == (uintptr_t)death.vids[i])
            return (int)i;
    return -1;
}
static uintptr_t vtable(unsigned int i)
{
    return death.base + (death.classes[i] == 5   ? STEAM_CANNON_VTABLE_RVA
                         : death.classes[i] == 8 ? STEAM_TERRAIN_VTABLE_RVA
                                                 : STEAM_SPRITE_VTABLE_RVA);
}
int unit_death_init(uintptr_t base, ActorCreate create)
{
    static const unsigned char cannon[] = {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0xE8, 0x55};
    static const unsigned char terrain[] = {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0xE8, 0x15};
    if (initialized || !base || !create)
        return 0;
    __try
    {
        if (memcmp((void*)(base + STEAM_CANNON_DESTROY_RVA), cannon, sizeof(cannon)) ||
            memcmp((void*)(base + STEAM_TERRAIN_DESTROY_RVA), terrain, sizeof(terrain)) ||
            *(uintptr_t*)(base + STEAM_CANNON_VTABLE_RVA) != base + STEAM_CANNON_DESTROY_RVA ||
            *(uintptr_t*)(base + STEAM_TERRAIN_VTABLE_RVA) != base + STEAM_TERRAIN_DESTROY_RVA)
            return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
    UnitDeathEngine api = {create, (ActorDestroy)(base + STEAM_CANNON_DESTROY_RVA),
                           (ActorDestroy)(base + STEAM_TERRAIN_DESTROY_RVA),
                           (ActorDestroy)(base + STEAM_SPRITE_DESTROY_RVA)};
    return unit_death_initialize(base, &api);
}
int unit_death_initialize(uintptr_t base, const UnitDeathEngine* api)
{
    if (initialized || !base || !api || !api->create || !api->cannon_destroy ||
        !api->terrain_destroy || !api->part_destroy)
        return 0;
    memset(slots, 0, sizeof(slots));
    selected = 0;
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
    return 1;
}
/* Caller in SEH. Resolve every link through the loaded table before cloning.
   Only the observed visual graph is admitted, with bounded creation counts. */
static int prepare(uintptr_t game)
{
    uintptr_t sources[DEATH_VID_COUNT];
    death.count = DEATH_VID_COUNT;
    death.corpse = 0;
    death.gib = 5;
    for (unsigned int i = 0; i < DEATH_VID_COUNT; ++i)
    {
        death.classes[i] = legacy_classes[i];
        death.linked[i] = DEATH_GRAPH_LIMIT;
    }
    death.linked[0] = 8;
    for (unsigned int i = 0; i < DEATH_VID_COUNT; ++i)
    {
        uintptr_t s = *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + indices[i] * 4u);
        if (!s || *(unsigned int*)(s + STEAM_VID_INDEX_OFFSET) != indices[i] ||
            *(unsigned int*)(s + STEAM_VID_CLASS_OFFSET) != legacy_classes[i] ||
            *(unsigned int*)(s + STEAM_VID_CATEGORY_OFFSET) >= STEAM_TICK_ENTITY_LIST_COUNT ||
            *(uintptr_t*)(s + STEAM_VID_SERVICE_OFFSET) ||
            (*(unsigned int*)(s + STEAM_VID_RESOURCE_FLAGS_OFFSET) &
             STEAM_VID_SPECIAL_FACTORY_FLAG))
            return 0;
        sources[i] = s;
        memcpy(death.vids[i], (void*)s, STEAM_VID_SIZE);
    }
    if (*(uintptr_t*)(sources[0] + STEAM_VID_LINKED_OFFSET) != sources[8])
        return 0;
    for (unsigned int e = 0; e < sizeof(edges) / sizeof(edges[0]); ++e)
        if (*(uintptr_t*)(sources[edges[e].from] + STEAM_VID_CREATIONS_OFFSET +
                          edges[e].animation * 4u) != sources[edges[e].to] ||
            *(unsigned int*)(sources[edges[e].from] + STEAM_VID_CREATION_COUNT_OFFSET +
                             edges[e].animation * 4u) != edges[e].count)
            return 0;
    for (unsigned int i = 0; i < DEATH_VID_COUNT; ++i)
    {
        unsigned char* d = death.vids[i];
        memset(d + STEAM_VID_COUNTERS_OFFSET, 0, STEAM_VID_COUNTERS_SIZE);
        *(int*)(d + STEAM_VID_DELETE_SCRIPT_OFFSET) = -1;
        *(int*)(d + STEAM_VID_DAMAGE_SCRIPT_OFFSET) = -1;
        *(int*)(d + STEAM_VID_COLLISION_SCRIPT_OFFSET) = -1;
        *(unsigned int*)(d + STEAM_VID_COLLISION_MASK_OFFSET) = 0;
        *(unsigned int*)(d + STEAM_VID_HEALTH_OFFSET) = 0;
        *(int*)(d + STEAM_VID_DAMAGE_OFFSET) = 0;
        uintptr_t link = *(uintptr_t*)(d + STEAM_VID_LINKED_OFFSET);
        if (link && !(i == 0 && link == sources[8]))
            return 0;
        *(uintptr_t*)(d + STEAM_VID_LINKED_OFFSET) = link ? (uintptr_t)death.vids[8] : 0;
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
            for (edge = 0; edge < sizeof(edges) / sizeof(edges[0]); ++edge)
                if (edges[edge].from == i && edges[edge].animation == a &&
                    target == sources[edges[edge].to] && count == (int)edges[edge].count)
                    break;
            if (edge >= sizeof(edges) / sizeof(edges[0]))
                return 0;
            *(uintptr_t*)(d + STEAM_VID_CREATIONS_OFFSET + a * 4u) =
                (uintptr_t)death.vids[edges[edge].to];
        }
    }
    return 1;
}
/* Stock-descriptor admission policy, not a claim about native table capacity.
   Dynamic map-owned descriptors (1024+) are never shallow-cloned here. */
#define DEATH_STOCK_VID_LIMIT 1024u
#define DEATH_CREATION_LIMIT  16u
typedef struct DeathGraph
{
    uintptr_t sources[DEATH_GRAPH_LIMIT];
    unsigned int colors[DEATH_GRAPH_LIMIT], cost[DEATH_GRAPH_LIMIT];
    int armor;
} DeathGraph;
static int graph_visit(uintptr_t game, uintptr_t source, DeathGraph* graph, unsigned int* result,
                       int linked_art)
{
    if (!source)
        return 0;
    unsigned int i;
    for (i = 0; i < death.count; ++i)
        if (graph->sources[i] == source)
            break;
    if (i < death.count)
    {
        if (graph->colors[i] != 2u)
            return 0; /* A back edge is a cycle. */
        *result = i;
        return 1;
    }
    if (death.count >= DEATH_GRAPH_LIMIT)
        return 0;
    unsigned int index = *(unsigned int*)(source + STEAM_VID_INDEX_OFFSET);
    unsigned int class_id = *(unsigned int*)(source + STEAM_VID_CLASS_OFFSET);
    if (index >= DEATH_STOCK_VID_LIMIT ||
        *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + index * 4u) != source ||
        (class_id != 5u && class_id != 8u && class_id != 9u) ||
        !*(uintptr_t*)(source + STEAM_VID_BEHAVIOR_OFFSET) ||
        *(unsigned int*)(source + STEAM_VID_CATEGORY_OFFSET) >= STEAM_TICK_ENTITY_LIST_COUNT ||
        *(uintptr_t*)(source + STEAM_VID_SERVICE_OFFSET) ||
        (*(unsigned int*)(source + STEAM_VID_RESOURCE_FLAGS_OFFSET) &
         STEAM_VID_SPECIAL_FACTORY_FLAG))
        return 0;
    i = death.count++;
    graph->sources[i] = source;
    graph->colors[i] = 1;
    death.classes[i] = class_id;
    death.linked[i] = DEATH_GRAPH_LIMIT;
    unsigned char* d = death.vids[i];
    memcpy(d, (void*)source, STEAM_VID_SIZE);
    memset(d + STEAM_VID_COUNTERS_OFFSET, 0, STEAM_VID_COUNTERS_SIZE);
    *(int*)(d + STEAM_VID_DELETE_SCRIPT_OFFSET) = -1;
    *(int*)(d + STEAM_VID_DAMAGE_SCRIPT_OFFSET) = -1;
    *(int*)(d + STEAM_VID_COLLISION_SCRIPT_OFFSET) = -1;
    *(unsigned int*)(d + STEAM_VID_COLLISION_MASK_OFFSET) = 0;
    *(unsigned int*)(d + STEAM_VID_HEALTH_OFFSET) = 0;
    *(int*)(d + STEAM_VID_DAMAGE_OFFSET) = 0;
    unsigned int total = 1;
    uintptr_t linked = *(uintptr_t*)(source + STEAM_VID_LINKED_OFFSET);
    if (linked)
    {
        unsigned int child;
        if (class_id == 9u || *(unsigned int*)(linked + STEAM_VID_CLASS_OFFSET) != 9u ||
            *(uintptr_t*)(linked + STEAM_VID_LINKED_OFFSET) ||
            !graph_visit(game, linked, graph, &child, 1))
            return 0;
        death.linked[i] = child;
        *(uintptr_t*)(d + STEAM_VID_LINKED_OFFSET) = (uintptr_t)death.vids[child];
        total += graph->cost[child];
    }
    for (unsigned int a = 0; a < STEAM_ANIMATION_COUNT; ++a)
    {
        *(int*)(d + STEAM_VID_ANIMATION_SCRIPTS_OFFSET + a * 4u) = -1;
        uintptr_t target = *(uintptr_t*)(source + STEAM_VID_CREATIONS_OFFSET + a * 4u);
        /* A linked SPRITE is borrowed art only, just like scoped VID108. */
        if (linked_art)
        {
            *(uintptr_t*)(d + STEAM_VID_CREATIONS_OFFSET + a * 4u) = 0;
            *(unsigned int*)(d + STEAM_VID_CREATION_COUNT_OFFSET + a * 4u) = 0;
            continue;
        }
        if (!target)
            continue;
        if (graph->armor && *(unsigned int*)(target + STEAM_VID_CLASS_OFFSET) == STEAM_UNIT_CLASS)
        {
            unsigned int successor = *(unsigned int*)(target + STEAM_VID_INDEX_OFFSET);
            if ((successor != 60u && successor != 80u) ||
                *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + successor * 4u) != target)
                return 0;
            *(uintptr_t*)(d + STEAM_VID_CREATIONS_OFFSET + a * 4u) = 0;
            *(unsigned int*)(d + STEAM_VID_CREATION_COUNT_OFFSET + a * 4u) = 0;
            continue;
        }
        unsigned int count = *(unsigned int*)(source + STEAM_VID_CREATION_COUNT_OFFSET + a * 4u),
                     child;
        if (!count || count > DEATH_CREATION_LIMIT ||
            !graph_visit(game, target, graph, &child, 0) ||
            graph->cost[child] > (DEATH_ENTITY_LIMIT - total) / count)
            return 0;
        total += count * graph->cost[child];
        *(uintptr_t*)(d + STEAM_VID_CREATIONS_OFFSET + a * 4u) = (uintptr_t)death.vids[child];
    }
    graph->cost[i] = total;
    graph->colors[i] = 2;
    *result = i;
    return 1;
}
static int prepare_type(uintptr_t game, unsigned int source_vid, unsigned int animation, int armor,
                        unsigned int* multiplicity)
{
    if (source_vid >= DEATH_STOCK_VID_LIMIT)
        return 0;
    uintptr_t source = *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + source_vid * 4u);
    if (!source || *(unsigned int*)(source + STEAM_VID_INDEX_OFFSET) != source_vid ||
        *(unsigned int*)(source + STEAM_VID_CLASS_OFFSET) != STEAM_UNIT_CLASS ||
        *(unsigned int*)(source + STEAM_VID_CATEGORY_OFFSET) >= STEAM_TICK_ENTITY_LIST_COUNT ||
        *(uintptr_t*)(source + STEAM_VID_SERVICE_OFFSET) ||
        (*(unsigned int*)(source + STEAM_VID_RESOURCE_FLAGS_OFFSET) &
         STEAM_VID_SPECIAL_FACTORY_FLAG))
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
    death.count = 0;
    death.corpse = animation == STEAM_ANIM_DEATH ? 0u : DEATH_GRAPH_LIMIT;
    death.gib = DEATH_GRAPH_LIMIT;
    if (armor && source_vid == 90u && root &&
        *(unsigned int*)(root + STEAM_VID_INDEX_OFFSET) == 80u &&
        *(unsigned int*)(root + STEAM_VID_CLASS_OFFSET) == STEAM_UNIT_CLASS &&
        *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 80u * 4u) == root)
    {
        /* Custom large armor feedback: borrow the medium shell's visual graph.
           Private copies omit its living successor; native90->80 stays intact. */
        return prepare_type(game, 70u, animation, armor, multiplicity);
    }
    return graph_visit(game, root, &graph, &root_index, 0) && root_index == 0u &&
           graph.cost[root_index] <= DEATH_ENTITY_LIMIT / *multiplicity;
}
/* Caller in SEH. Check identity in a map list before following a child pointer. */
static int find_private(uintptr_t game, uintptr_t wanted, unsigned int index)
{
    for (unsigned int c = 0; c < STEAM_TICK_ENTITY_LIST_COUNT; ++c)
    {
        uintptr_t list = game + STEAM_WORLD_LIST_OFFSET + c * STEAM_LIST_SIZE;
        unsigned int n = *(unsigned int*)(list + 4), cap = *(unsigned int*)(list + 8);
        uintptr_t* entries = *(uintptr_t**)(list + 12);
        if (n > cap || n > 65536u || (n && !entries))
            return 0;
        for (unsigned int j = 0; j < n; ++j)
            if (entries[j] == wanted)
                return *(uintptr_t*)wanted == vtable(index) &&
                       *(uintptr_t*)(wanted + STEAM_ENTITY_VID_OFFSET) ==
                           (uintptr_t)death.vids[index];
    }
    return 0;
}
UnitDeathSample unit_death_sample(uintptr_t game)
{
    UnitDeathSample s = {0};
    if (death.state == DEATH_OFF || !death.game)
        return s;
    __try
    {
        if (!game || *(uintptr_t*)game != death.base + STEAM_GAME_VTABLE_RVA)
        {
            s.fault = 1;
            return s;
        }
        for (unsigned int c = 0; c < STEAM_TICK_ENTITY_LIST_COUNT; ++c)
        {
            uintptr_t list = game + STEAM_WORLD_LIST_OFFSET + c * STEAM_LIST_SIZE;
            unsigned int n = *(unsigned int*)(list + 4), cap = *(unsigned int*)(list + 8);
            uintptr_t* entries = *(uintptr_t**)(list + 12);
            if (n > cap || n > 65536u || (n && !entries))
            {
                s.fault = 1;
                return s;
            }
            for (unsigned int j = 0; j < n; ++j)
            {
                uintptr_t e = entries[j];
                if (!e)
                    continue;
                int i = private_index(*(uintptr_t*)(e + STEAM_ENTITY_VID_OFFSET));
                if (i < 0)
                    continue;
                if (*(uintptr_t*)e != vtable((unsigned int)i) || ++s.entities > DEATH_ENTITY_LIMIT)
                {
                    s.fault = 1;
                    return s;
                }
                uintptr_t child = *(uintptr_t*)(e + STEAM_ENTITY_CHILD_OFFSET);
                uintptr_t parent = *(uintptr_t*)(e + STEAM_ENTITY_PARENT_OFFSET);
                unsigned int link = death.linked[i];
                if (child && (link >= death.count || !find_private(game, child, link) ||
                              *(uintptr_t*)(child + STEAM_ENTITY_PARENT_OFFSET) != e ||
                              *(uintptr_t*)(child + STEAM_ENTITY_CHILD_OFFSET)))
                {
                    s.fault = 1;
                    return s;
                }
                if (parent)
                {
                    unsigned int parent_index;
                    for (parent_index = 0; parent_index < death.count; ++parent_index)
                        if (death.linked[parent_index] == (unsigned int)i &&
                            find_private(game, parent, parent_index))
                            break;
                    if (parent_index == death.count ||
                        *(uintptr_t*)(parent + STEAM_ENTITY_CHILD_OFFSET) != e)
                    {
                        s.fault = 1;
                        return s;
                    }
                }
                if ((unsigned int)i == death.corpse)
                    ++s.corpses;
                if ((unsigned int)i == death.gib ||
                    (death.gib == DEATH_GRAPH_LIMIT && death.classes[i] == 5u && i != 0))
                    ++s.gibs;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        s.fault = 1;
    }
    return s;
}
int unit_death_play(uintptr_t game, const UnitPuppetPose* p, unsigned int animation)
{
    if (death.state != DEATH_READY || death.game || !p ||
        (animation != STEAM_ANIM_DEATH && animation != STEAM_ANIM_GIB_DEATH) || !isfinite(p->x) ||
        !isfinite(p->y) || !isfinite(p->z) || p->direction > 255u)
        return 0;
    __try
    {
        if (!game || *(uintptr_t*)game != death.base + STEAM_GAME_VTABLE_RVA || !prepare(game))
            return 0;
        death.game = game;
        death.state = DEATH_OWNED;
        void* e = death.create((void*)game, NULL, death.vids[animation == STEAM_ANIM_DEATH ? 0 : 3],
                               p->x, p->y, p->z, p->direction, NULL);
        UnitDeathSample s = unit_death_sample(game);
        unsigned int root_index = animation == STEAM_ANIM_DEATH ? 0u : 3u;
        if (!e || s.fault || !s.entities || !find_private(game, (uintptr_t)e, root_index))
        {
            death.state = DEATH_FAULT;
            return -1;
        }
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        death.state = death.game ? DEATH_FAULT : DEATH_READY;
        return -1;
    }
}
static int play_type(uintptr_t game, const UnitPuppetPose* p, unsigned int animation,
                     unsigned int source_vid, int armor)
{
    if (death.state != DEATH_READY || death.game || !p ||
        (animation != STEAM_ANIM_DEATH && animation != STEAM_ANIM_GIB_DEATH) || !isfinite(p->x) ||
        !isfinite(p->y) || !isfinite(p->z) || p->direction > 255u)
        return 0;
    __try
    {
        unsigned int multiplicity;
        if (!game || *(uintptr_t*)game != death.base + STEAM_GAME_VTABLE_RVA ||
            !prepare_type(game, source_vid, animation, armor, &multiplicity))
            return 0;
        if (!death.count)
            return 1;
        death.game = game;
        death.state = DEATH_OWNED;
        void* e = NULL;
        for (unsigned int i = 0; i < multiplicity; ++i)
        {
            e = death.create((void*)game, NULL, death.vids[0], p->x, p->y, p->z, p->direction,
                             NULL);
            if (!e || !find_private(game, (uintptr_t)e, 0u))
            {
                death.state = DEATH_FAULT;
                return -1;
            }
        }
        UnitDeathSample s = unit_death_sample(game);
        if (!e || s.fault || !s.entities || !find_private(game, (uintptr_t)e, 0u))
        {
            death.state = DEATH_FAULT;
            return -1;
        }
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        death.state = death.game ? DEATH_FAULT : DEATH_READY;
        return -1;
    }
}
int unit_death_play_type(uintptr_t game, const UnitPuppetPose* p, unsigned int animation,
                         unsigned int source_vid)
{
    return play_type(game, p, animation, source_vid, 0);
}
int unit_death_play_armor(uintptr_t game, const UnitPuppetPose* p, unsigned int source_vid)
{
    if (source_vid != 70u && source_vid != 90u)
        return 0;
    return play_type(game, p, STEAM_ANIM_DEATH, source_vid, 1);
}
int unit_death_remove(uintptr_t game)
{
    if (death.state == DEATH_OFF || !death.game)
        return 1;
    UnitDeathSample s = unit_death_sample(game);
    if (s.fault)
        return 0;
    if (!s.entities)
    {
        death.game = 0;
        death.state = DEATH_READY;
        return 1;
    }
    if (death.state == DEATH_ABANDONED)
        return 0;
    death.state = DEATH_ABANDONED;
    __try
    {
        /* No update can create successors during game-thread forced removal. */
        for (unsigned int i = 0; i < death.count; ++i)
            memset(death.vids[i] + STEAM_VID_CREATIONS_OFFSET, 0, STEAM_ANIMATION_COUNT * 4u);
        for (unsigned int attempt = 0; attempt < DEATH_ENTITY_LIMIT; ++attempt)
        {
            uintptr_t found = 0;
            int index = -1;
            for (unsigned int c = 0; c < STEAM_TICK_ENTITY_LIST_COUNT && !found; ++c)
            {
                uintptr_t list = game + STEAM_WORLD_LIST_OFFSET + c * STEAM_LIST_SIZE;
                unsigned int n = *(unsigned int*)(list + 4);
                uintptr_t* entries = *(uintptr_t**)(list + 12);
                for (unsigned int j = 0; j < n; ++j)
                {
                    uintptr_t e = entries[j];
                    if (!e)
                        continue;
                    int i = private_index(*(uintptr_t*)(e + STEAM_ENTITY_VID_OFFSET));
                    if (i >= 0 && !*(uintptr_t*)(e + STEAM_ENTITY_PARENT_OFFSET))
                    {
                        found = e;
                        index = i;
                        break;
                    }
                }
            }
            if (!found)
                break;
            ActorDestroy destroy = death.classes[index] == 5   ? death.cannon_destroy
                                   : death.classes[index] == 8 ? death.terrain_destroy
                                                               : death.part_destroy;
            destroy((void*)found, NULL, 1);
        }
        s = unit_death_sample(game);
        if (s.fault || s.entities)
            return 0;
        death.game = 0;
        death.state = DEATH_READY;
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}
int unit_death_stop(void)
{
    for (unsigned int i = 0; i < UNIT_DEATH_SLOT_COUNT; ++i)
        if (slots[i].game)
            return 0;
    for (unsigned int i = 0; i < UNIT_DEATH_SLOT_COUNT; ++i)
        slots[i].state = DEATH_OFF;
    initialized = 0;
    selected = 0;
    return 1;
}
