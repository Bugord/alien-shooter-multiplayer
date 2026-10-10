#include <stdio.h>
#include <string.h>
#include "../../asmp-dll/src/game/unit_death.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
#define NODE_COUNT 9u
#define ENTITY_COUNT 160u
static const unsigned int indices[NODE_COUNT] = {51,52,916,910,110,151,152,120,108};
static const unsigned int classes[NODE_COUNT] = {5,8,8,5,8,5,8,8,9};
static const unsigned int categories[NODE_COUNT] = {6,1,2,15,2,6,1,2,4};
/* Saved native graph in analysis/data/rea-monster-ai/death-vids-live.json:
   normal corpse 51 -> 916/52, alternate 910 -> 110/151 -> 152, mark120.
   The 108 linked art is cloned with every creation suppressed. */
typedef struct Edge { unsigned int from, animation, to, count; } Edge;
static const Edge edges[] = {{0,14,2,1}, {0,15,1,1}, {1,15,2,1}, {2,14,7,8},
    {3,14,4,2}, {3,15,5,8}, {4,14,7,8}, {5,12,7,1}, {5,15,6,1}, {6,14,7,1}};
static unsigned char game[0x2000], sources[NODE_COUNT][STEAM_VID_SIZE];
static unsigned char before[NODE_COUNT][STEAM_VID_SIZE], entities[ENTITY_COUNT][0x94];
static unsigned char unrelated[0x94], foreign_vid[STEAM_VID_SIZE];
static unsigned char unit_sources[2][STEAM_VID_SIZE];
static uintptr_t lists[STEAM_TICK_ENTITY_LIST_COUNT][ENTITY_COUNT], image_base;
static unsigned char* private_vids[NODE_COUNT];
static unsigned int allocated, factories, destroys, destroyed_entities, parts_destroyed, foreign_destroyed;
static unsigned int selected_index;
static int violation, destroy_fault, factory_fault, orphan_only;
static unsigned char form_sources[6][STEAM_VID_SIZE];
static unsigned char form_before[6][STEAM_VID_SIZE];
static unsigned char* form_private_root;

static uintptr_t list(unsigned int category) { return (uintptr_t)game + STEAM_WORLD_LIST_OFFSET + category * STEAM_LIST_SIZE; }
static void add(unsigned int category, uintptr_t entity) {
    unsigned int* count = (unsigned int*)(list(category) + 4);
    if (*count >= ENTITY_COUNT) { violation = 1; return; }
    lists[category][(*count)++] = entity;
}
static void unregister_entity(uintptr_t entity) {
    for (unsigned int c = 0; c < STEAM_TICK_ENTITY_LIST_COUNT; ++c) {
        unsigned int* count = (unsigned int*)(list(c) + 4);
        for (unsigned int i = 0; i < *count; ++i) if (lists[c][i] == entity) {
            lists[c][i] = lists[c][--*count]; return;
        }
    }
}
static void empty_world(void) {
    for (unsigned int c = 0; c < STEAM_TICK_ENTITY_LIST_COUNT; ++c) *(unsigned int*)(list(c) + 4) = 0;
}
static int index_of(const unsigned char* vid) {
    unsigned int index = *(const unsigned int*)(vid + STEAM_VID_INDEX_OFFSET);
    for (unsigned int i = 0; i < NODE_COUNT; ++i) if (indices[i] == index) return (int)i;
    return -1;
}
static uintptr_t expected_vtable(unsigned int i) {
    return image_base + (classes[i] == 5 ? STEAM_CANNON_VTABLE_RVA :
        classes[i] == 8 ? STEAM_TERRAIN_VTABLE_RVA : STEAM_SPRITE_VTABLE_RVA);
}
static int sources_preserved(void) { return !memcmp(sources, before, sizeof(sources)); }
static int validate_clone(unsigned char* vid) {
    int position = index_of(vid);
    if (position < 0) return 0;
    unsigned int i = (unsigned int)position;
    if (vid == sources[i]) return 0;
    if (private_vids[i]) return private_vids[i] == vid;
    private_vids[i] = vid;
    if (*(unsigned int*)(vid + STEAM_VID_CLASS_OFFSET) != classes[i] ||
        *(unsigned int*)(vid + STEAM_VID_COLLISION_MASK_OFFSET) || *(unsigned int*)(vid + STEAM_VID_HEALTH_OFFSET) ||
        *(int*)(vid + STEAM_VID_DAMAGE_OFFSET) || *(int*)(vid + STEAM_VID_DELETE_SCRIPT_OFFSET) != -1 ||
        *(int*)(vid + STEAM_VID_DAMAGE_SCRIPT_OFFSET) != -1 || *(int*)(vid + STEAM_VID_COLLISION_SCRIPT_OFFSET) != -1 ||
        *(uintptr_t*)(vid + STEAM_VID_SERVICE_OFFSET)) return 0;
    for (unsigned int b = 0; b < STEAM_VID_COUNTERS_SIZE; ++b) if (vid[STEAM_VID_COUNTERS_OFFSET + b]) return 0;
    /* Animation frame counts and borrowed behavior remain unchanged. */
    if (memcmp(vid + 0x74u, sources[i] + 0x74u, 0xBCu - 0x74u) ||
        *(uintptr_t*)(vid + 0x45Cu) != *(uintptr_t*)(sources[i] + 0x45Cu)) return 0;
    uintptr_t linked = *(uintptr_t*)(vid + STEAM_VID_LINKED_OFFSET);
    if (i == 0u) { if (!linked || !validate_clone((unsigned char*)linked) || index_of((unsigned char*)linked) != 8) return 0; }
    else if (linked) return 0;
    for (unsigned int a = 0; a < STEAM_ANIMATION_COUNT; ++a) {
        if (*(int*)(vid + STEAM_VID_ANIMATION_SCRIPTS_OFFSET + a * 4u) != -1) return 0;
        uintptr_t target = *(uintptr_t*)(vid + STEAM_VID_CREATIONS_OFFSET + a * 4u);
        const Edge* expected = NULL;
        for (unsigned int j = 0; j < sizeof(edges) / sizeof(edges[0]); ++j)
            if (edges[j].from == i && edges[j].animation == a) expected = &edges[j];
        if (!expected) { if (target) return 0; continue; }
        if (!target || index_of((unsigned char*)target) != (int)expected->to ||
            *(unsigned int*)(vid + STEAM_VID_CREATION_COUNT_OFFSET + a * 4u) != expected->count ||
            !validate_clone((unsigned char*)target)) return 0;
    }
    return 1;
}
static uintptr_t spawn_descriptor(unsigned char* vid, uintptr_t parent, unsigned int depth) {
    if (allocated >= ENTITY_COUNT || depth > NODE_COUNT) { violation = 1; return 0; }
    int position = index_of(vid); if (position < 0) { violation = 1; return 0; }
    unsigned int i = (unsigned int)position;
    unsigned char* e = entities[allocated++];
    *(uintptr_t*)e = expected_vtable(i); *(uintptr_t*)(e + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)vid;
    *(uintptr_t*)(e + STEAM_ENTITY_PARENT_OFFSET) = parent;
    add(categories[i], (uintptr_t)e);
    ++*(unsigned int*)(vid + STEAM_VID_COUNTERS_OFFSET); /* Native private bookkeeping. */
    unsigned char* child = *(unsigned char**)(vid + STEAM_VID_LINKED_OFFSET);
    if (child) *(uintptr_t*)(e + STEAM_ENTITY_CHILD_OFFSET) = spawn_descriptor(child, (uintptr_t)e, depth + 1u);
    unsigned char* birth = *(unsigned char**)(vid + STEAM_VID_CREATIONS_OFFSET + 14u * 4u);
    unsigned int count = *(unsigned int*)(vid + STEAM_VID_CREATION_COUNT_OFFSET + 14u * 4u);
    if (birth) for (unsigned int j = 0; j < count; ++j) spawn_descriptor(birth, 0, depth + 1u);
    return (uintptr_t)e;
}
static void* __fastcall create(void* world, void* unused, void* descriptor,
    float x, float y, float z, int direction, void* owner) {
    (void)unused; ++factories;
    memset(private_vids, 0, sizeof(private_vids));
    if (world != game || owner || x != 180.5f || y != 200.25f || z != 3.0f || direction != 192 ||
        !sources_preserved() || !validate_clone(descriptor)) violation = 1;
    selected_index = *(unsigned int*)((unsigned char*)descriptor + STEAM_VID_INDEX_OFFSET);
    if (factory_fault) RaiseException(0xE1234567u, 0, 0, NULL);
    if (orphan_only) { spawn_descriptor(private_vids[8], 0, 0); return NULL; }
    return (void*)spawn_descriptor(descriptor, 0, 0);
}
static void destroy_recursive(uintptr_t entity) {
    if (entity == (uintptr_t)unrelated) { ++foreign_destroyed; unregister_entity(entity); return; }
    ++destroyed_entities;
    unsigned char* vid = *(unsigned char**)(entity + STEAM_ENTITY_VID_OFFSET);
    if (index_of(vid) == 8) ++parts_destroyed;
    uintptr_t child = *(uintptr_t*)(entity + STEAM_ENTITY_CHILD_OFFSET);
    if (child) destroy_recursive(child);
    unregister_entity(entity);
}
static void* __fastcall destroy(void* entity, void* unused, int release) {
    (void)unused; ++destroys;
    if (release != 1) violation = 1;
    for (unsigned int i = 0; i < NODE_COUNT; ++i) if (private_vids[i])
        for (unsigned int a = 0; a < STEAM_ANIMATION_COUNT; ++a)
            if (*(uintptr_t*)(private_vids[i] + STEAM_VID_CREATIONS_OFFSET + a * 4u)) violation = 1;
    if (destroy_fault) RaiseException(0xE1234567u, 0, 0, NULL);
    destroy_recursive((uintptr_t)entity); return entity;
}
static void* __fastcall destroy_cannon(void* entity, void* unused, int release) {
    int i = index_of(*(unsigned char**)((uintptr_t)entity + STEAM_ENTITY_VID_OFFSET));
    if (i < 0 || classes[i] != 5u) violation = 1;
    return destroy(entity, unused, release);
}
static void* __fastcall destroy_terrain(void* entity, void* unused, int release) {
    int i = index_of(*(unsigned char**)((uintptr_t)entity + STEAM_ENTITY_VID_OFFSET));
    if (i < 0 || classes[i] != 8u) violation = 1;
    return destroy(entity, unused, release);
}
static void* __fastcall destroy_part(void* entity, void* unused, int release) {
    int i = index_of(*(unsigned char**)((uintptr_t)entity + STEAM_ENTITY_VID_OFFSET));
    if (i < 0 || classes[i] != 9u) violation = 1;
    return destroy(entity, unused, release);
}
static UnitPuppetPose prepare(void) {
    memset(game, 0, sizeof(game)); memset(entities, 0, sizeof(entities)); memset(lists, 0, sizeof(lists));
    memset(private_vids, 0, sizeof(private_vids)); memset(unrelated, 0, sizeof(unrelated));
    memset(sources, 0xA5, sizeof(sources)); memset(foreign_vid, 0, sizeof(foreign_vid));
    *(uintptr_t*)game = image_base + STEAM_GAME_VTABLE_RVA;
    for (unsigned int c = 0; c < STEAM_TICK_ENTITY_LIST_COUNT; ++c) {
        *(unsigned int*)(list(c) + 8) = ENTITY_COUNT; *(uintptr_t**)(list(c) + 12) = lists[c];
    }
    for (unsigned int i = 0; i < NODE_COUNT; ++i) {
        unsigned char* d = sources[i];
        *(unsigned int*)(d + STEAM_VID_INDEX_OFFSET) = indices[i];
        *(unsigned int*)(d + STEAM_VID_CLASS_OFFSET) = classes[i];
        *(unsigned int*)(d + STEAM_VID_CATEGORY_OFFSET) = categories[i];
        *(uintptr_t*)(d + STEAM_VID_LINKED_OFFSET) = *(uintptr_t*)(d + STEAM_VID_SERVICE_OFFSET) = 0;
        *(unsigned int*)(d + STEAM_VID_RESOURCE_FLAGS_OFFSET) = 0;
        memset(d + STEAM_VID_CREATIONS_OFFSET, 0, STEAM_ANIMATION_COUNT * 4u);
        memset(d + STEAM_VID_CREATION_COUNT_OFFSET, 0, STEAM_ANIMATION_COUNT * 4u);
        *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + indices[i] * 4u) = (uintptr_t)d;
    }
    *(uintptr_t*)(sources[0] + STEAM_VID_LINKED_OFFSET) = (uintptr_t)sources[8];
    for (unsigned int i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i) {
        const Edge* edge = &edges[i];
        *(uintptr_t*)(sources[edge->from] + STEAM_VID_CREATIONS_OFFSET + edge->animation * 4u) = (uintptr_t)sources[edge->to];
        *(unsigned int*)(sources[edge->from] + STEAM_VID_CREATION_COUNT_OFFSET + edge->animation * 4u) = edge->count;
    }
    /* Stock linked art's unknown effects must be stripped, never followed. */
    *(uintptr_t*)(sources[8] + STEAM_VID_CREATIONS_OFFSET + STEAM_ANIM_ATTACK * 4u) = (uintptr_t)foreign_vid;
    memcpy(before, sources, sizeof(sources));
    memset(unit_sources, 0, sizeof(unit_sources));
    for (unsigned int u = 0; u < 2u; ++u) {
        unsigned int index = u ? 60u : 50u;
        unsigned char* d = unit_sources[u];
        *(unsigned int*)(d + STEAM_VID_INDEX_OFFSET) = index;
        *(unsigned int*)(d + STEAM_VID_CLASS_OFFSET) = STEAM_UNIT_CLASS;
        *(unsigned int*)(d + STEAM_VID_CATEGORY_OFFSET) = 6;
        *(uintptr_t*)(d + STEAM_VID_CREATIONS_OFFSET + 15u * 4u) = (uintptr_t)sources[u ? 3 : 0];
        *(uintptr_t*)(d + STEAM_VID_CREATIONS_OFFSET + 16u * 4u) = (uintptr_t)sources[3];
        *(unsigned int*)(d + STEAM_VID_CREATION_COUNT_OFFSET + 15u * 4u) = 1;
        *(unsigned int*)(d + STEAM_VID_CREATION_COUNT_OFFSET + 16u * 4u) = 1;
        *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + index * 4u) = (uintptr_t)d;
    }
    *(uintptr_t*)(unrelated + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)foreign_vid;
    add(3u, (uintptr_t)unrelated);
    *(unsigned int*)(list(17u) + 4) = *(unsigned int*)(list(17u) + 8) = 1;
    *(uintptr_t*)(list(17u) + 12) = 1; /* Never scan script lists. */
    allocated = factories = destroys = destroyed_entities = parts_destroyed = foreign_destroyed = selected_index = 0;
    violation = destroy_fault = factory_fault = orphan_only = 0;
    UnitPuppetPose pose = {180.5f, 200.25f, 3.0f, 0, 192, 0}; return pose;
}
static uintptr_t form_spawn(unsigned char* vid, unsigned int depth) {
    if (depth > 8u || allocated >= ENTITY_COUNT) { violation = 1; return 0; }
    unsigned int index = *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET);
    unsigned int c = *(unsigned int*)(vid + STEAM_VID_CLASS_OFFSET);
    if ((uintptr_t)vid == *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + index*4u) ||
        (c != 5u && c != 8u && c != 9u) || *(int*)(vid + STEAM_VID_DAMAGE_OFFSET) ||
        *(unsigned int*)(vid + STEAM_VID_COLLISION_MASK_OFFSET) || *(unsigned int*)(vid + STEAM_VID_HEALTH_OFFSET)) violation = 1;
    for (unsigned int a = 0; a < STEAM_ANIMATION_COUNT; ++a) {
        if (*(int*)(vid + STEAM_VID_ANIMATION_SCRIPTS_OFFSET + a*4u) != -1) violation = 1;
        uintptr_t target = *(uintptr_t*)(vid + STEAM_VID_CREATIONS_OFFSET + a*4u);
        if (target && *(unsigned int*)(target + STEAM_VID_CLASS_OFFSET) == STEAM_UNIT_CLASS) violation = 1;
    }
    unsigned char* e = entities[allocated++]; memset(e,0,0x94);
    *(uintptr_t*)e = image_base + (c == 5u ? STEAM_CANNON_VTABLE_RVA : c == 8u ? STEAM_TERRAIN_VTABLE_RVA : STEAM_SPRITE_VTABLE_RVA);
    *(uintptr_t*)(e + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)vid;
    add(c == 5u ? 6u : c == 8u ? 2u : 4u,(uintptr_t)e);
    uintptr_t birth = *(uintptr_t*)(vid + STEAM_VID_CREATIONS_OFFSET + 14u*4u);
    unsigned int count = *(unsigned int*)(vid + STEAM_VID_CREATION_COUNT_OFFSET + 14u*4u);
    if (birth) for (unsigned int i = 0; i < count; ++i) form_spawn((unsigned char*)birth,depth+1u);
    return (uintptr_t)e;
}
static void* __fastcall form_create(void* world, void* unused, void* descriptor,
    float x,float y,float z,int direction,void* owner) {
    (void)unused; (void)x; (void)y; (void)z; (void)direction;
    if (world != game || owner) violation = 1;
    ++factories; form_private_root = descriptor;
    return (void*)form_spawn(descriptor,0);
}
static void* __fastcall form_destroy(void* e,void* unused,int release) {
    (void)unused;
    if (release != 1 || e == unrelated) violation = 1;
    unsigned char* vid = *(unsigned char**)((uintptr_t)e + STEAM_ENTITY_VID_OFFSET);
    for (unsigned int a = 0; a < STEAM_ANIMATION_COUNT; ++a)
        if (*(uintptr_t*)(vid + STEAM_VID_CREATIONS_OFFSET + a*4u)) violation = 1;
    ++destroyed_entities; unregister_entity((uintptr_t)e); return e;
}
static UnitPuppetPose prepare_forms(int armor) {
    UnitPuppetPose pose = prepare(); memset(form_sources,0,sizeof(form_sources));
    const unsigned int ids[6] = {armor ? 70u : 69u, armor ? 771u : 149u,
        armor ? 763u : 147u, armor ? 764u : 148u,60u,90u};
    for (unsigned int i = 0; i < 6u; ++i) {
        unsigned char* d = form_sources[i];
        *(unsigned int*)(d + STEAM_VID_INDEX_OFFSET) = ids[i];
        *(unsigned int*)(d + STEAM_VID_CLASS_OFFSET) = i == 0u || i >= 4u ? 2u :
            i == 1u ? (armor ? 5u : 9u) : i == 2u ? 5u : 8u;
        *(unsigned int*)(d + STEAM_VID_CATEGORY_OFFSET) = 6u;
        *(uintptr_t*)(d + STEAM_VID_BEHAVIOR_OFFSET) = 0x12345678u;
        *(int*)(d + STEAM_VID_DAMAGE_OFFSET) = 42;
        *(int*)(d + STEAM_VID_HEALTH_OFFSET) = 100;
        *(unsigned int*)(d + STEAM_VID_COLLISION_MASK_OFFSET) = 0xFFFFFFFFu;
        *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + ids[i]*4u) = (uintptr_t)d;
    }
    *(uintptr_t*)(form_sources[0] + STEAM_VID_CREATIONS_OFFSET + 15u*4u) = (uintptr_t)form_sources[1];
    *(unsigned int*)(form_sources[0] + STEAM_VID_CREATION_COUNT_OFFSET + 15u*4u) = armor ? 1u : 2u;
    *(uintptr_t*)(form_sources[1] + STEAM_VID_CREATIONS_OFFSET + 14u*4u) = (uintptr_t)form_sources[2];
    *(unsigned int*)(form_sources[1] + STEAM_VID_CREATION_COUNT_OFFSET + 14u*4u) = armor ? 14u : 12u;
    *(uintptr_t*)(form_sources[2] + STEAM_VID_CREATIONS_OFFSET + 15u*4u) = (uintptr_t)form_sources[3];
    *(unsigned int*)(form_sources[2] + STEAM_VID_CREATION_COUNT_OFFSET + 15u*4u) = 1u;
    if (armor) {
        *(uintptr_t*)(form_sources[1] + STEAM_VID_CREATIONS_OFFSET + 15u*4u) = (uintptr_t)form_sources[4];
        *(unsigned int*)(form_sources[1] + STEAM_VID_CREATION_COUNT_OFFSET + 15u*4u) = 1u;
    }
    memcpy(form_before,form_sources,sizeof(form_sources)); form_private_root = NULL; return pose;
}
int main(void) {
    image_base = (uintptr_t)VirtualAlloc(NULL, 0x140000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); CHECK(image_base);
    UnitDeathEngine api = {create, destroy_cannon, destroy_terrain, destroy_part}, invalid = api;
    CHECK(!unit_death_initialize(0, &api) && !unit_death_initialize(image_base, NULL));
    invalid.part_destroy = NULL; CHECK(!unit_death_initialize(image_base, &invalid));
    for (unsigned int variant = 15; variant <= 16; ++variant) {
        UnitPuppetPose pose = prepare(); CHECK(unit_death_initialize(image_base, &api));
        CHECK(!unit_death_initialize(image_base, &api));
        CHECK(!unit_death_play((uintptr_t)game, NULL, variant));
        CHECK(!unit_death_play((uintptr_t)game, &pose, 14));
        pose.direction = 256; CHECK(!unit_death_play((uintptr_t)game, &pose, variant)); pose.direction = 192;
        CHECK(unit_death_play((uintptr_t)game, &pose, variant) == 1 && !violation && factories == 1);
        CHECK(selected_index == (variant == 15 ? 51u : 910u));
        UnitDeathSample s = unit_death_sample((uintptr_t)game);
        CHECK(!s.fault && s.entities == (variant == 15 ? 11u : 19u) && s.corpses == (variant == 15 ? 1u : 0u));
        CHECK(!unit_death_play((uintptr_t)game, &pose, variant) && factories == 1 && !unit_death_stop());
        /* Later native successors are independent allocations, found by VID,
           without saving factory return pointers or synchronizing their frames. */
        if (variant == 15) spawn_descriptor(private_vids[1], 0, 0);
        else for (unsigned int i = 0; i < 8u; ++i) spawn_descriptor(private_vids[5], 0, 0);
        s = unit_death_sample((uintptr_t)game);
        CHECK(!s.fault && s.entities == (variant == 15 ? 12u : 27u) && s.gibs == (variant == 15 ? 0u : 8u));
        unsigned int owned = s.entities;
        CHECK(unit_death_remove((uintptr_t)game) && destroyed_entities == owned && !violation && !foreign_destroyed);
        CHECK(parts_destroyed == (variant == 15 ? 1u : 0u) && !unit_death_sample((uintptr_t)game).entities);
        CHECK(sources_preserved() && unit_death_remove((uintptr_t)game) && unit_death_stop());
    }
    for (unsigned int bad = 0; bad < 20u; ++bad) {
        UnitPuppetPose pose = prepare();
        if (bad == 0u) *(unsigned int*)(sources[1] + STEAM_VID_CLASS_OFFSET) = 2;
        if (bad == 1u) *(unsigned int*)(sources[2] + STEAM_VID_INDEX_OFFSET) = 999;
        if (bad == 2u) *(uintptr_t*)(sources[0] + STEAM_VID_LINKED_OFFSET) = (uintptr_t)foreign_vid;
        if (bad == 3u) *(uintptr_t*)(sources[3] + STEAM_VID_SERVICE_OFFSET) = 1;
        if (bad == 4u) *(unsigned int*)(sources[4] + STEAM_VID_RESOURCE_FLAGS_OFFSET) = STEAM_VID_SPECIAL_FACTORY_FLAG;
        if (bad == 5u) *(unsigned int*)(sources[5] + STEAM_VID_CATEGORY_OFFSET) = STEAM_TICK_ENTITY_LIST_COUNT;
        if (bad == 6u) *(uintptr_t*)(sources[0] + STEAM_VID_CREATIONS_OFFSET + 14u * 4u) = (uintptr_t)foreign_vid;
        if (bad == 7u) *(uintptr_t*)(sources[0] + STEAM_VID_CREATIONS_OFFSET + 8u * 4u) = (uintptr_t)sources[2];
        if (bad == 8u) *(int*)(sources[0] + STEAM_VID_CREATION_COUNT_OFFSET + 14u * 4u) = 0;
        if (bad == 9u) *(int*)(sources[0] + STEAM_VID_CREATION_COUNT_OFFSET + 14u * 4u) = 17;
        if (bad == 10u) *(int*)(sources[0] + STEAM_VID_CREATION_COUNT_OFFSET + 14u * 4u) = -17;
        if (bad == 11u) *(uintptr_t*)(sources[0] + STEAM_VID_CREATIONS_OFFSET + 14u * 4u) = (uintptr_t)sources[0];
        if (bad == 12u) *(uintptr_t*)(sources[0] + STEAM_VID_CREATIONS_OFFSET + 14u * 4u) = (uintptr_t)sources[8];
        if (bad == 13u) *(uintptr_t*)(sources[0] + STEAM_VID_CREATIONS_OFFSET + 14u * 4u) = (uintptr_t)sources[3];
        if (bad == 14u) *(uintptr_t*)(sources[2] + STEAM_VID_CREATIONS_OFFSET + 14u * 4u) = (uintptr_t)sources[0];
        if (bad == 15u) *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 916u * 4u) = 0;
        if (bad == 16u) *(int*)(sources[0] + STEAM_VID_CREATION_COUNT_OFFSET + 14u * 4u) = -1;
        if (bad == 17u) *(int*)(sources[0] + STEAM_VID_CREATION_COUNT_OFFSET + 14u * 4u) = 16;
        if (bad == 18u) *(uintptr_t*)(sources[0] + STEAM_VID_CREATIONS_OFFSET + 14u * 4u) = 0;
        if (bad == 19u) *(uintptr_t*)(sources[0] + STEAM_VID_LINKED_OFFSET) = 0;
        CHECK(unit_death_initialize(image_base, &api));
        CHECK(!unit_death_play((uintptr_t)game, &pose, 15) && !factories && unit_death_stop());
    }
    /* An access fault while inspecting the source graph owns no native entity.
       Cleanup must be truthful, and a repaired source must permit the same
       initialized module to retry rather than remaining trapped in FAULT. */
    UnitPuppetPose pose = prepare();
    CHECK(unit_death_initialize(image_base, &api));
    *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 51u * 4u) = 1;
    CHECK(unit_death_play((uintptr_t)game, &pose, 15) == -1 && !factories && !destroys);
    CHECK(unit_death_remove((uintptr_t)game) && !unit_death_sample((uintptr_t)game).entities);
    *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 51u * 4u) = (uintptr_t)sources[0];
    CHECK(unit_death_play((uintptr_t)game, &pose, 15) == 1 && factories == 1 && !violation);
    CHECK(unit_death_remove((uintptr_t)game) && sources_preserved() && unit_death_stop());
    pose = prepare(); orphan_only = 1;
    CHECK(unit_death_initialize(image_base, &api) && unit_death_play((uintptr_t)game, &pose, 15) == -1);
    CHECK(unit_death_sample((uintptr_t)game).entities == 1 && !unit_death_stop());
    CHECK(unit_death_remove((uintptr_t)game) && parts_destroyed == 1 && !foreign_destroyed && !violation && unit_death_stop());
    pose = prepare(); factory_fault = 1;
    CHECK(unit_death_initialize(image_base, &api) && unit_death_play((uintptr_t)game, &pose, 15) == -1);
    CHECK(!unit_death_stop() && unit_death_remove((uintptr_t)game) && !destroys && unit_death_stop());
    pose = prepare(); CHECK(unit_death_initialize(image_base, &api) && unit_death_play((uintptr_t)game, &pose, 15) == 1);
    destroy_fault = 1;
    CHECK(!unit_death_remove((uintptr_t)game) && destroys == 1 && !unit_death_stop());
    CHECK(!unit_death_remove((uintptr_t)game) && destroys == 1);
    empty_world(); CHECK(unit_death_remove((uintptr_t)game) && destroys == 1 && unit_death_stop());
    pose = prepare(); CHECK(unit_death_initialize(image_base, &api) && unit_death_play((uintptr_t)game, &pose, 15) == 1);
    *(uintptr_t*)(entities[0] + STEAM_ENTITY_CHILD_OFFSET) = (uintptr_t)unrelated;
    CHECK(!unit_death_remove((uintptr_t)game) && !foreign_destroyed && !unit_death_stop());
    empty_world(); CHECK(unit_death_remove((uintptr_t)game) && unit_death_stop());
    pose = prepare(); CHECK(unit_death_initialize(image_base, &api) && unit_death_play((uintptr_t)game, &pose, 15) == 1);
    /* Corrupt class identity cannot select a destructor by stale VID alone. */
    *(uintptr_t*)entities[1] = image_base + STEAM_CANNON_VTABLE_RVA;
    CHECK(unit_death_sample((uintptr_t)game).fault && !unit_death_remove((uintptr_t)game) && !destroys && !unit_death_stop());
    empty_world(); CHECK(unit_death_remove((uintptr_t)game) && unit_death_stop());
    pose = prepare(); CHECK(unit_death_initialize(image_base, &api) && unit_death_play((uintptr_t)game, &pose, 15) == 1);
    while (allocated <= 128u) spawn_descriptor(private_vids[7], 0, 0);
    CHECK(unit_death_sample((uintptr_t)game).fault && !unit_death_remove((uintptr_t)game) && !destroys && !unit_death_stop());
    empty_world(); CHECK(unit_death_remove((uintptr_t)game) && unit_death_stop());
    /* Simultaneous presentations have independent pinned identities. The
       selected source UNIT determines the graph; type60 deliberately points
       at different visual art, proving no substitution with fixed VID51. */
    pose = prepare(); CHECK(unit_death_initialize(image_base, &api));
    CHECK(!unit_death_select(UNIT_DEATH_SLOT_COUNT));
    CHECK(unit_death_select(0) && unit_death_play_type((uintptr_t)game, &pose, 15, 50) == 1);
    unsigned char* first_private[NODE_COUNT]; memcpy(first_private, private_vids, sizeof(first_private));
    CHECK(selected_index == 51 && unit_death_sample((uintptr_t)game).entities == 11);
    CHECK(unit_death_select(63) && unit_death_play_type((uintptr_t)game, &pose, 15, 60) == 1);
    unsigned char* second_private[NODE_COUNT]; memcpy(second_private, private_vids, sizeof(second_private));
    CHECK(selected_index == 910 && unit_death_sample((uintptr_t)game).entities == 19);
    CHECK(first_private[7] && second_private[7] && first_private[7] != second_private[7]);
    CHECK(!unit_death_stop() && factories == 2);
    CHECK(unit_death_select(0) && unit_death_sample((uintptr_t)game).entities == 11);
    memcpy(private_vids, first_private, sizeof(private_vids));
    CHECK(unit_death_remove((uintptr_t)game) && !violation && !foreign_destroyed);
    CHECK(!unit_death_stop());
    CHECK(unit_death_select(63) && unit_death_sample((uintptr_t)game).entities == 19);
    memcpy(private_vids, second_private, sizeof(private_vids));
    CHECK(unit_death_remove((uintptr_t)game) && !violation && !foreign_destroyed && unit_death_stop());
    /* Every unsupported graph must fail before native allocation. A class2
       successor is a live-monster transformation, never a visual death. */
    for (unsigned int bad = 0; bad < 9u; ++bad) {
        pose = prepare();
        if (bad == 0) *(uintptr_t*)(unit_sources[0] + STEAM_VID_CREATIONS_OFFSET + 15u * 4u) = 0;
        if (bad == 1) *(unsigned int*)(unit_sources[0] + STEAM_VID_CREATION_COUNT_OFFSET + 15u * 4u) = 2;
        if (bad == 2) *(uintptr_t*)(sources[1] + STEAM_VID_CREATIONS_OFFSET + 15u * 4u) = (uintptr_t)sources[0];
        if (bad == 3) *(unsigned int*)(sources[1] + STEAM_VID_CLASS_OFFSET) = STEAM_UNIT_CLASS;
        if (bad == 4) *(unsigned int*)(sources[0] + STEAM_VID_CREATION_COUNT_OFFSET + 14u * 4u) = 17;
        if (bad == 5) *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 52u * 4u) = 0;
        if (bad == 6) *(uintptr_t*)(sources[8] + STEAM_VID_LINKED_OFFSET) = (uintptr_t)sources[0];
        if (bad == 7) *(unsigned int*)(sources[0] + STEAM_VID_CREATION_COUNT_OFFSET + 15u * 4u) = 16;
        if (bad == 8) *(uintptr_t*)(sources[0] + STEAM_VID_BEHAVIOR_OFFSET) = 0;
        CHECK(unit_death_initialize(image_base, &api));
        CHECK(!unit_death_play_type((uintptr_t)game, &pose, 15, 50) && !factories && unit_death_stop());
    }
    /* Ice149 is a standalone class9 factory root: its birth147x12 must remain,
       whereas linked sprite art still has every creation suppressed. */
    UnitDeathEngine form_api = {form_create,form_destroy,form_destroy,form_destroy};
    pose = prepare_forms(0); CHECK(unit_death_initialize(image_base,&form_api));
    CHECK(unit_death_play_type((uintptr_t)game,&pose,STEAM_ANIM_DEATH,69u) == 1);
    CHECK(factories == 2u && allocated == 26u && !violation);
    CHECK(*(unsigned int*)(form_private_root + STEAM_VID_INDEX_OFFSET) == 149u);
    CHECK(*(uintptr_t*)(form_private_root + STEAM_VID_CREATIONS_OFFSET + 14u*4u));
    CHECK(*(unsigned int*)(form_private_root + STEAM_VID_CREATION_COUNT_OFFSET + 14u*4u) == 12u);
    CHECK(unit_death_sample((uintptr_t)game).entities == 26u);
    CHECK(unit_death_remove((uintptr_t)game) && destroyed_entities == 26u && !violation);
    CHECK(!memcmp(form_sources,form_before,sizeof(form_sources)) && !foreign_destroyed && unit_death_stop());
    /* Both armor sizes use private shell visuals with no live successor.
       Independent banks may own identical art without sharing private VIDs. */
    pose = prepare_forms(1); CHECK(unit_death_initialize(image_base,&form_api));
    CHECK(!unit_death_play_type((uintptr_t)game,&pose,STEAM_ANIM_DEATH,70u) && !factories);
    CHECK(unit_death_play_armor((uintptr_t)game,&pose,70u) == 1);
    CHECK(factories == 1u && allocated == 15u && !violation);
    CHECK(!*(uintptr_t*)(form_private_root + STEAM_VID_CREATIONS_OFFSET + 15u*4u));
    CHECK(unit_death_sample((uintptr_t)game).entities == 15u);
    CHECK(!memcmp(form_sources,form_before,sizeof(form_sources)));
    *(unsigned int*)(form_sources[4] + STEAM_VID_INDEX_OFFSET) = 80u;
    *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 80u*4u) = (uintptr_t)form_sources[4];
    *(uintptr_t*)(form_sources[5] + STEAM_VID_CREATIONS_OFFSET + 15u*4u) = (uintptr_t)form_sources[4];
    *(unsigned int*)(form_sources[5] + STEAM_VID_CREATION_COUNT_OFFSET + 15u*4u) = 1u;
    memcpy(form_before,form_sources,sizeof(form_sources));
    unsigned char* medium_private_root = form_private_root;
    CHECK(unit_death_select(UNIT_DEATH_SLOT_COUNT-1u));
    *(unsigned int*)(form_sources[5] + STEAM_VID_CREATION_COUNT_OFFSET + 15u*4u) = 2u;
    CHECK(!unit_death_play_armor((uintptr_t)game,&pose,90u) && factories == 1u);
    *(unsigned int*)(form_sources[5] + STEAM_VID_CREATION_COUNT_OFFSET + 15u*4u) = 1u;
    CHECK(unit_death_play_armor((uintptr_t)game,&pose,90u) == 1 && factories == 2u);
    CHECK(form_private_root != medium_private_root && allocated == 30u && !violation);
    CHECK(!*(uintptr_t*)(form_private_root + STEAM_VID_CREATIONS_OFFSET + 15u*4u));
    CHECK(unit_death_sample((uintptr_t)game).entities == 15u && !unit_death_stop());
    CHECK(!memcmp(form_sources,form_before,sizeof(form_sources)));
    CHECK(unit_death_select(0) && unit_death_remove((uintptr_t)game));
    CHECK(destroyed_entities == 15u && !violation && !unit_death_stop());
    CHECK(unit_death_select(UNIT_DEATH_SLOT_COUNT-1u));
    CHECK(unit_death_sample((uintptr_t)game).entities == 15u && unit_death_remove((uintptr_t)game));
    CHECK(destroyed_entities == 30u && !violation);
    *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 70u*4u) = 0;
    CHECK(!unit_death_play_armor((uintptr_t)game,&pose,90u) && factories == 2u);
    CHECK(unit_death_stop());
    CHECK(VirtualFree((void*)image_base, 0, MEM_RELEASE));
    puts("UNIT death graph sanitization, successors, variants and guarded cleanup checks passed."); return 0;
}
