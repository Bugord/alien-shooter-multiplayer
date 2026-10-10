#include <stdio.h>
#include <string.h>
#include "../../asmp-dll/src/game/unit_combat.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
enum FactoryMode { FACTORY_NORMAL, FACTORY_WRONG_RESULT, FACTORY_ORPHAN_PART };
static unsigned char game[0x2000], root[0x94], part[0x94], unrelated[0x94], extra[0x94];
static unsigned char source[STEAM_VID_SIZE], part_source[STEAM_VID_SIZE];
static unsigned char original_source[STEAM_VID_SIZE], original_part[STEAM_VID_SIZE];
static uintptr_t entries[STEAM_TICK_ENTITY_LIST_COUNT][64], image_base;
static unsigned char gallery_root[2][0x94], gallery_part[0x94], gallery_sources[2][STEAM_VID_SIZE];
static unsigned int gallery_created, gallery_destroyed;
static int gallery_error;
static unsigned int factory_calls, destroy_calls, roots_destroyed, parts_destroyed, animation_calls;
static int factory_violation, destroy_fault;
static enum FactoryMode factory_mode;
static unsigned char form_sources[7][STEAM_VID_SIZE], form_behavior[STEAM_BEHAVIOR_SIZE];
static unsigned char form_entities[64][0x94];
static unsigned int form_allocated, form_removed;
static int form_error;

static uintptr_t list(unsigned int category) {
    return (uintptr_t)game + STEAM_WORLD_LIST_OFFSET + category * STEAM_LIST_SIZE;
}
static void add_entity(unsigned int category, unsigned char* entity) {
    unsigned int* count = (unsigned int*)(list(category) + 4);
    entries[category][(*count)++] = (uintptr_t)entity;
}
static void unregister_entity(uintptr_t entity) {
    for (unsigned int category = 0; category < STEAM_TICK_ENTITY_LIST_COUNT; ++category) {
        unsigned int* count = (unsigned int*)(list(category) + 4);
        for (unsigned int i = 0; i < *count; ++i) {
            if (entries[category][i] == entity) {
                entries[category][i] = entries[category][--*count];
                return;
            }
        }
    }
}
static void clear_world(void) {
    for (unsigned int i = 0; i < STEAM_TICK_ENTITY_LIST_COUNT; ++i)
        *(unsigned int*)(list(i) + 4) = 0;
}
/* Test the safety boundary at the factory call, before a native constructor
   could execute birth scripts, create a projectile, or touch source counters. */
static int inert_descriptor(const unsigned char* vid) {
    for (unsigned int i = 0; i < STEAM_ANIMATION_COUNT; ++i) {
        if (*(const uintptr_t*)(vid + STEAM_VID_CREATIONS_OFFSET + i * 4u) ||
            *(const int*)(vid + STEAM_VID_ANIMATION_SCRIPTS_OFFSET + i * 4u) != -1) return 0;
    }
    for (unsigned int i = 0; i < STEAM_VID_COUNTERS_SIZE; ++i)
        if (vid[STEAM_VID_COUNTERS_OFFSET + i]) return 0;
    return *(const int*)(vid + STEAM_VID_DAMAGE_OFFSET) == 0 &&
        *(const int*)(vid + STEAM_VID_DELETE_SCRIPT_OFFSET) == -1 &&
        *(const int*)(vid + STEAM_VID_DAMAGE_SCRIPT_OFFSET) == -1 &&
        *(const int*)(vid + STEAM_VID_COLLISION_SCRIPT_OFFSET) == -1;
}
static void* __fastcall gallery_create(void* world, void* unused, void* descriptor,
    float x, float y, float z, int direction, void* owner) {
    (void)unused;
    unsigned int slot = gallery_created++;
    if (slot >= 2u) { gallery_error = 1; return NULL; }
    unsigned char* v = descriptor;
    unsigned char* linked = *(unsigned char**)(v + STEAM_VID_LINKED_OFFSET);
    if (world != game || owner || x != (float)(300u + slot * 100u) || y != 400.0f || z || direction != 80)
        gallery_error = 1;
    if (slot == 0u) {
        if (linked || !inert_descriptor(v)) gallery_error = 1;
    } else {
        if (!linked || *(uintptr_t*)(v + STEAM_VID_CREATIONS_OFFSET + STEAM_ANIM_DEATH * 4u) != 0x1111u ||
            *(uintptr_t*)(v + STEAM_VID_CREATIONS_OFFSET + STEAM_ANIM_GIB_DEATH * 4u) != 0x2222u ||
            *(int*)(v + STEAM_VID_DAMAGE_OFFSET) ||
            *(int*)(v + STEAM_VID_ANIMATION_SCRIPTS_OFFSET + STEAM_ANIM_DEATH * 4u) != -1 ||
            *(uintptr_t*)(v + STEAM_VID_CREATIONS_OFFSET + 14u * 4u)) gallery_error = 1;
    }
    unsigned char* entity = gallery_root[slot]; memset(entity, 0, 0x94);
    *(uintptr_t*)entity = image_base + STEAM_UNIT_VTABLE_RVA;
    *(uintptr_t*)(entity + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)v;
    if (linked) {
        memset(gallery_part, 0, sizeof(gallery_part));
        *(uintptr_t*)gallery_part = image_base + STEAM_SPRITE_VTABLE_RVA;
        *(uintptr_t*)(gallery_part + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)linked;
        *(uintptr_t*)(gallery_part + STEAM_ENTITY_PARENT_OFFSET) = (uintptr_t)entity;
        *(uintptr_t*)(entity + STEAM_ENTITY_CHILD_OFFSET) = (uintptr_t)gallery_part;
        add_entity(4u, gallery_part);
    }
    add_entity(5u, entity); return entity;
}
static void* __fastcall gallery_destroy(void* entity, void* unused, int release) {
    (void)unused; if (release != 1) gallery_error = 1;
    ++gallery_destroyed;
    uintptr_t linked = *(uintptr_t*)((unsigned char*)entity + STEAM_ENTITY_CHILD_OFFSET);
    if (linked) unregister_entity(linked);
    unregister_entity((uintptr_t)entity); return entity;
}
static void* __fastcall create(void* world, void* unused, void* descriptor,
    float x, float y, float z, int direction, void* owner) {
    (void)unused;
    ++factory_calls;
    unsigned char* private_root = descriptor;
    unsigned char* private_part = *(unsigned char**)(private_root + STEAM_VID_LINKED_OFFSET);
    if (world != game || owner || private_root == source || private_part == part_source ||
        !private_part || !inert_descriptor(private_root) || !inert_descriptor(private_part) ||
        *(unsigned int*)(private_root + STEAM_VID_INDEX_OFFSET) != 50u ||
        *(unsigned int*)(private_part + STEAM_VID_INDEX_OFFSET) != 108u ||
        *(uintptr_t*)(private_part + STEAM_VID_LINKED_OFFSET) ||
        memcmp(source, original_source, sizeof(source)) || memcmp(part_source, original_part, sizeof(part_source)) ||
        x != 180.0f || y != 200.0f || z != 0.0f || direction != 64)
        factory_violation = 1;
    /* Art and geometry are borrowed unchanged, including opaque pointers. */
    if (memcmp(private_root + 0x74u, source + 0x74u, 0xBCu - 0x74u) ||
        memcmp(private_part + 0x74u, part_source + 0x74u, 0xBCu - 0x74u) ||
        *(uintptr_t*)(private_root + 0x45Cu) != *(uintptr_t*)(source + 0x45Cu))
        factory_violation = 1;
    *(uintptr_t*)root = image_base + STEAM_UNIT_VTABLE_RVA;
    *(uintptr_t*)part = image_base + STEAM_SPRITE_VTABLE_RVA;
    *(uintptr_t*)(root + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)private_root;
    *(uintptr_t*)(part + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)private_part;
    *(uintptr_t*)(root + STEAM_ENTITY_CHILD_OFFSET) = (uintptr_t)part;
    *(uintptr_t*)(part + STEAM_ENTITY_PARENT_OFFSET) = (uintptr_t)root;
    *(unsigned int*)(root + STEAM_ENTITY_ANIM_OFFSET) = 14u;
    *(unsigned int*)(part + STEAM_ENTITY_ANIM_OFFSET) = 14u;
    ++*(unsigned int*)(private_part + STEAM_VID_COUNTERS_OFFSET);
    add_entity(4u, part);
    if (factory_mode == FACTORY_ORPHAN_PART) {
        *(uintptr_t*)(part + STEAM_ENTITY_PARENT_OFFSET) = 0;
        return NULL;
    }
    ++*(unsigned int*)(private_root + STEAM_VID_COUNTERS_OFFSET);
    add_entity(5u, root);
    return factory_mode == FACTORY_WRONG_RESULT ? unrelated : root;
}
static void __fastcall animation(void* entity, void* unused, unsigned int value) {
    (void)unused;
    ++animation_calls;
    *(unsigned int*)((unsigned char*)entity + STEAM_ENTITY_ANIM_OFFSET) = value;
    /* Native ChangeAnimation propagates applicable animations to linked art. */
    uintptr_t linked = *(uintptr_t*)((unsigned char*)entity + STEAM_ENTITY_CHILD_OFFSET);
    if (linked) *(unsigned int*)(linked + STEAM_ENTITY_ANIM_OFFSET) = value;
}
static void* __fastcall destroy(void* entity, void* unused, int release) {
    (void)unused;
    ++destroy_calls;
    if (release != 1) factory_violation = 1;
    if (destroy_fault) RaiseException(0xE1234567u, 0, 0, NULL);
    if (entity == root) {
        /* Native root deletion recursively destroys its linked part. */
        ++roots_destroyed;
        if (*(uintptr_t*)(root + STEAM_ENTITY_CHILD_OFFSET)) {
            ++parts_destroyed; unregister_entity((uintptr_t)part);
            *(uintptr_t*)(root + STEAM_ENTITY_CHILD_OFFSET) = 0;
        }
        unregister_entity((uintptr_t)root);
    } else if (entity == part) {
        ++parts_destroyed; unregister_entity((uintptr_t)part);
    } else factory_violation = 1;
    return entity;
}
static Snapshot prepare(void) {
    memset(game, 0, sizeof(game)); memset(root, 0, sizeof(root)); memset(part, 0, sizeof(part));
    memset(unrelated, 0, sizeof(unrelated)); memset(entries, 0, sizeof(entries));
    memset(source, 0xA5, sizeof(source)); memset(part_source, 0x5A, sizeof(part_source));
    *(uintptr_t*)game = image_base + STEAM_GAME_VTABLE_RVA;
    for (unsigned int i = 0; i < STEAM_TICK_ENTITY_LIST_COUNT; ++i) {
        *(unsigned int*)(list(i) + 8) = 4;
        *(uintptr_t**)(list(i) + 12) = entries[i];
    }
    unsigned char* descriptors[] = {source, part_source};
    for (unsigned int i = 0; i < 2u; ++i) {
        *(unsigned int*)(descriptors[i] + STEAM_VID_INDEX_OFFSET) = i ? 108u : 50u;
        *(unsigned int*)(descriptors[i] + STEAM_VID_CLASS_OFFSET) = i ? STEAM_PUPPET_PART_CLASS : STEAM_UNIT_CLASS;
        *(unsigned int*)(descriptors[i] + STEAM_VID_CATEGORY_OFFSET) = i ? 4u : 5u;
        *(unsigned int*)(descriptors[i] + STEAM_VID_RESOURCE_FLAGS_OFFSET) = 0;
        *(uintptr_t*)(descriptors[i] + STEAM_VID_SERVICE_OFFSET) = 0;
    }
    *(uintptr_t*)(source + STEAM_VID_LINKED_OFFSET) = (uintptr_t)part_source;
    *(uintptr_t*)(part_source + STEAM_VID_LINKED_OFFSET) = 0;
    *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 50u * 4u) = (uintptr_t)source;
    *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 108u * 4u) = (uintptr_t)part_source;
    *(uintptr_t*)(unrelated + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)source;
    add_entity(3u, unrelated);
    /* Later script lists contain non-entity data and must never be scanned. */
    *(unsigned int*)(list(17u) + 4) = *(unsigned int*)(list(17u) + 8) = 1;
    *(uintptr_t*)(list(17u) + 12) = 1;
    memcpy(original_source, source, sizeof(source)); memcpy(original_part, part_source, sizeof(part_source));
    factory_calls = destroy_calls = roots_destroyed = parts_destroyed = animation_calls = 0;
    factory_violation = destroy_fault = 0; factory_mode = FACTORY_NORMAL;
    Snapshot local = {0}; local.game = (uintptr_t)game; local.in_level = 1; local.health = 100;
    local.x = 100.0f; local.y = 200.0f; local.z = 0.0f; local.direction = 64u; return local;
}
static uintptr_t form_spawn(unsigned char* vid, unsigned int depth) {
    if (depth > 8u || form_allocated >= 64u) { form_error = 1; return 0; }
    unsigned int index = *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET);
    unsigned int c = *(unsigned int*)(vid + STEAM_VID_CLASS_OFFSET);
    uintptr_t stock = *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + index * 4u);
    if ((uintptr_t)vid == stock || *(int*)(vid + STEAM_VID_DAMAGE_OFFSET) ||
        *(int*)(vid + STEAM_VID_CREATE_SCRIPT_OFFSET) != -1 ||
        *(int*)(vid + STEAM_VID_DELETE_SCRIPT_OFFSET) != -1) form_error = 1;
    for (unsigned int a = 0; a < STEAM_ANIMATION_COUNT; ++a)
        if (*(int*)(vid + STEAM_VID_ANIMATION_SCRIPTS_OFFSET + a * 4u) != -1) form_error = 1;
    if (c == STEAM_UNIT_CLASS) {
        unsigned char* behavior = *(unsigned char**)(vid + STEAM_VID_BEHAVIOR_OFFSET);
        if (!behavior || behavior == form_behavior || *(unsigned int*)(behavior + STEAM_BEHAVIOR_UNIT_FLAGS_OFFSET)) form_error = 1;
    }
    unsigned char* e = form_entities[form_allocated++]; memset(e, 0, 0x94);
    *(uintptr_t*)e = image_base + (c == 2u ? STEAM_UNIT_VTABLE_RVA : c == 5u ? STEAM_CANNON_VTABLE_RVA : STEAM_TERRAIN_VTABLE_RVA);
    *(uintptr_t*)(e + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)vid;
    *(int*)(e + STEAM_ENTITY_HEALTH_OFFSET) = 100;
    add_entity(c == 2u ? 5u : c == 5u ? 7u : 8u, e);
    uintptr_t birth = *(uintptr_t*)(vid + STEAM_VID_CREATIONS_OFFSET + 14u * 4u);
    unsigned int n = *(unsigned int*)(vid + STEAM_VID_CREATION_COUNT_OFFSET + 14u * 4u);
    if (birth) for (unsigned int i = 0; i < n; ++i) form_spawn((unsigned char*)birth, depth + 1u);
    return (uintptr_t)e;
}
static void* __fastcall form_create(void* world, void* unused, void* vid, float x, float y, float z, int direction, void* owner) {
    (void)unused; (void)x; (void)y; (void)z; (void)direction;
    if (world != game || owner) form_error = 1;
    return (void*)form_spawn(vid, 0);
}
static void* __fastcall form_destroy(void* entity, void* unused, int release) {
    (void)unused;
    unsigned char* vid = *(unsigned char**)((uintptr_t)entity + STEAM_ENTITY_VID_OFFSET);
    unsigned int index = *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET);
    if (release != 1 || (uintptr_t)vid == *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + index * 4u)) form_error = 1;
    for (unsigned int a = 0; a < STEAM_ANIMATION_COUNT; ++a)
        if (*(uintptr_t*)(vid + STEAM_VID_CREATIONS_OFFSET + a * 4u)) form_error = 1;
    ++form_removed; unregister_entity((uintptr_t)entity); return entity;
}
static Snapshot prepare_forms(void) {
    Snapshot local = prepare(); clear_world();
    for (unsigned int i = 0; i < STEAM_TICK_ENTITY_LIST_COUNT; ++i) *(unsigned int*)(list(i) + 8) = 64u;
    memset(form_sources, 0, sizeof(form_sources)); memset(form_behavior, 0x5A, sizeof(form_behavior));
    static const unsigned int ids[7] = {70u,771u,763u,764u,60u,69u,90u};
    for (unsigned int i = 0; i < 7u; ++i) {
        unsigned char* d = form_sources[i];
        *(unsigned int*)(d + STEAM_VID_INDEX_OFFSET) = ids[i];
        *(unsigned int*)(d + STEAM_VID_CLASS_OFFSET) = i == 1u || i == 2u ? 5u : i == 3u ? 8u : 2u;
        *(unsigned int*)(d + STEAM_VID_CATEGORY_OFFSET) = 5u;
        *(uintptr_t*)(d + STEAM_VID_BEHAVIOR_OFFSET) = (uintptr_t)form_behavior;
        *(int*)(d + STEAM_VID_HEALTH_OFFSET) = 100;
        *(int*)(d + STEAM_VID_DAMAGE_OFFSET) = 42;
        *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + ids[i] * 4u) = (uintptr_t)d;
    }
    *(uintptr_t*)(form_sources[0] + STEAM_VID_CREATIONS_OFFSET + 15u * 4u) = (uintptr_t)form_sources[1];
    *(unsigned int*)(form_sources[0] + STEAM_VID_CREATION_COUNT_OFFSET + 15u * 4u) = 1u;
    *(uintptr_t*)(form_sources[1] + STEAM_VID_CREATIONS_OFFSET + 14u * 4u) = (uintptr_t)form_sources[2];
    *(unsigned int*)(form_sources[1] + STEAM_VID_CREATION_COUNT_OFFSET + 14u * 4u) = 14u;
    *(uintptr_t*)(form_sources[1] + STEAM_VID_CREATIONS_OFFSET + 15u * 4u) = (uintptr_t)form_sources[4];
    *(unsigned int*)(form_sources[1] + STEAM_VID_CREATION_COUNT_OFFSET + 15u * 4u) = 1u;
    *(uintptr_t*)(form_sources[2] + STEAM_VID_CREATIONS_OFFSET + 15u * 4u) = (uintptr_t)form_sources[3];
    *(unsigned int*)(form_sources[2] + STEAM_VID_CREATION_COUNT_OFFSET + 15u * 4u) = 1u;
    form_allocated = form_removed = 0; form_error = 0;
    *(ActorDestroy*)(image_base + STEAM_CANNON_VTABLE_RVA) = form_destroy;
    *(ActorDestroy*)(image_base + STEAM_TERRAIN_VTABLE_RVA) = form_destroy;
    return local;
}
int main(void) {
    image_base = (uintptr_t)VirtualAlloc(NULL, 0x140000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(image_base);
    *(ActorDestroy*)(image_base + STEAM_SPRITE_VTABLE_RVA) = destroy;
    ActorCreate bound = create;
    CHECK(!unit_combat_bind(image_base, &bound) && !bound);
    const unsigned char signature[] = {0x55,0x8B,0xEC,0x6A,0xFF};
    const unsigned char cannon[] = {0x55,0x8B,0xEC,0x56,0x8B,0xF1,0xE8,0x55};
    const unsigned char terrain[] = {0x55,0x8B,0xEC,0x56,0x8B,0xF1,0xE8,0x15};
    memcpy((void*)(image_base + STEAM_ACTOR_CREATE_RVA), signature, sizeof(signature));
    memcpy((void*)(image_base + STEAM_CANNON_DESTROY_RVA), cannon, sizeof(cannon));
    memcpy((void*)(image_base + STEAM_TERRAIN_DESTROY_RVA), terrain, sizeof(terrain));
    *(uintptr_t*)(image_base + STEAM_CANNON_VTABLE_RVA) = image_base + STEAM_CANNON_DESTROY_RVA;
    *(uintptr_t*)(image_base + STEAM_TERRAIN_VTABLE_RVA) = image_base + STEAM_TERRAIN_DESTROY_RVA;
    *(uintptr_t*)(image_base + STEAM_GAME_VTABLE_RVA + 8u * sizeof(uintptr_t)) = image_base + STEAM_ACTOR_CREATE_RVA;
    CHECK(unit_combat_bind(image_base, &bound) && bound == (ActorCreate)(image_base + STEAM_ACTOR_CREATE_RVA));
    *(uintptr_t*)(image_base + STEAM_CANNON_VTABLE_RVA) = 0;
    CHECK(!unit_combat_bind(image_base, &bound) && !bound);
    *(uintptr_t*)(image_base + STEAM_CANNON_VTABLE_RVA) = image_base + STEAM_CANNON_DESTROY_RVA;
    *(uintptr_t*)(image_base + STEAM_GAME_VTABLE_RVA + 8u * sizeof(uintptr_t)) = 0;
    CHECK(!unit_combat_bind(image_base, &bound) && !bound);

    Snapshot local = prepare();
    CHECK(!unit_combat_init(0, create, destroy, animation));
    CHECK(!unit_combat_init(image_base, NULL, destroy, animation));
    CHECK(unit_combat_init(image_base, create, destroy, animation));
    CHECK(!unit_combat_init(image_base, create, destroy, animation));
    CHECK(!unit_combat_spawn(NULL));
    local.health = 0; CHECK(!unit_combat_spawn(&local)); local.health = 100;
    CHECK(factory_calls == 0 && unit_combat_spawn(&local) == 1 && factory_calls == 1 && !factory_violation);
    UnitCombatSample s = unit_combat_sample((uintptr_t)game, 1);
    CHECK(!s.fault && s.sources_unchanged && s.private_entities == 2u && s.other_entities == 1u);
    CHECK(s.root == (uintptr_t)root && s.child == (uintptr_t)part && animation_calls);
    CHECK(!s.unexpected_entities);
    /* Counting alone would miss a new object replacing an expired ambient one. */
    unregister_entity((uintptr_t)unrelated);
    CHECK(!unit_combat_sample((uintptr_t)game, 1).unexpected_entities);
    memset(extra, 0, sizeof(extra)); *(uintptr_t*)(extra + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)source;
    add_entity(3u, extra);
    s = unit_combat_sample((uintptr_t)game, 1);
    CHECK(s.other_entities == 1u && s.unexpected_entities == 1u);
    unregister_entity((uintptr_t)extra); add_entity(3u, unrelated);
    CHECK(*(unsigned int*)(root + STEAM_ENTITY_ANIM_OFFSET) == 0u);
    CHECK(!unit_combat_stop() && !unit_combat_spawn(&local) && factory_calls == 1);
    /* A table replacement must not count a cached source pointer as current. */
    *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 50u * 4u) = (uintptr_t)part_source;
    CHECK(!unit_combat_sample((uintptr_t)game, 1).sources_unchanged);
    *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 50u * 4u) = (uintptr_t)source;
    CHECK(unit_combat_remove((uintptr_t)game) && roots_destroyed == 1 && parts_destroyed == 1 && !factory_violation);
    CHECK(!memcmp(source, original_source, sizeof(source)) && !memcmp(part_source, original_part, sizeof(part_source)));
    CHECK(unit_combat_remove((uintptr_t)game) && destroy_calls == 1 && unit_combat_stop());

    /* Reject descriptors outside this fixture before any native allocation. */
    for (unsigned int kind = 0; kind < 8u; ++kind) {
        local = prepare();
        if (kind == 0u) *(unsigned int*)(source + STEAM_VID_CLASS_OFFSET) = 4u;
        if (kind == 1u) *(unsigned int*)(part_source + STEAM_VID_INDEX_OFFSET) = 109u;
        if (kind == 2u) *(uintptr_t*)(source + STEAM_VID_SERVICE_OFFSET) = 1u;
        if (kind == 3u) *(uintptr_t*)(part_source + STEAM_VID_SERVICE_OFFSET) = 1u;
        if (kind == 4u) *(unsigned int*)(source + STEAM_VID_RESOURCE_FLAGS_OFFSET) = STEAM_VID_SPECIAL_FACTORY_FLAG;
        if (kind == 5u) *(unsigned int*)(part_source + STEAM_VID_RESOURCE_FLAGS_OFFSET) = STEAM_VID_SPECIAL_FACTORY_FLAG;
        if (kind == 6u) *(uintptr_t*)(part_source + STEAM_VID_LINKED_OFFSET) = (uintptr_t)source;
        if (kind == 7u) *(unsigned int*)(source + STEAM_VID_CATEGORY_OFFSET) = STEAM_TICK_ENTITY_LIST_COUNT;
        CHECK(unit_combat_init(image_base, create, destroy, animation));
        CHECK(!unit_combat_spawn(&local) && !factory_calls && unit_combat_stop());
    }
    local = prepare(); factory_mode = FACTORY_WRONG_RESULT;
    CHECK(unit_combat_init(image_base, create, destroy, animation));
    CHECK(unit_combat_spawn(&local) == -1 && !unit_combat_stop());
    CHECK(unit_combat_remove((uintptr_t)game) && roots_destroyed == 1 && parts_destroyed == 1);
    CHECK(unit_combat_stop() && !factory_violation);

    local = prepare(); factory_mode = FACTORY_ORPHAN_PART;
    CHECK(unit_combat_init(image_base, create, destroy, animation));
    CHECK(unit_combat_spawn(&local) == -1 && !unit_combat_stop());
    CHECK(unit_combat_remove((uintptr_t)game) && !roots_destroyed && parts_destroyed == 1);
    CHECK(unit_combat_stop() && !factory_violation);

    for (unsigned int topology = 0; topology < 2u; ++topology) {
        local = prepare();
        CHECK(unit_combat_init(image_base, create, destroy, animation) && unit_combat_spawn(&local) == 1);
        if (topology) *(uintptr_t*)part = image_base + STEAM_UNIT_VTABLE_RVA;
        else *(uintptr_t*)(part + STEAM_ENTITY_CHILD_OFFSET) = (uintptr_t)unrelated;
        CHECK(!unit_combat_remove((uintptr_t)game) && !destroy_calls && !unit_combat_stop());
        clear_world();
        CHECK(unit_combat_remove((uintptr_t)game) && !destroy_calls && unit_combat_stop());
    }

    local = prepare();
    CHECK(unit_combat_init(image_base, create, destroy, animation) && unit_combat_spawn(&local) == 1);
    /* The cached address now belongs to an unrelated entity. Only the still
       registered private part may be passed to a deleting destructor. */
    *(uintptr_t*)(root + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)source;
    *(uintptr_t*)(root + STEAM_ENTITY_CHILD_OFFSET) = 0;
    *(uintptr_t*)(part + STEAM_ENTITY_PARENT_OFFSET) = 0;
    CHECK(unit_combat_remove((uintptr_t)game) && !roots_destroyed && parts_destroyed == 1);
    CHECK(unit_combat_stop() && !factory_violation);

    local = prepare();
    CHECK(unit_combat_init(image_base, create, destroy, animation) && unit_combat_spawn(&local) == 1);
    /* A corrupt world list must not cause cached-pointer native calls. */
    *(unsigned int*)(list(5u) + 4) = 5u;
    CHECK(unit_combat_sample((uintptr_t)game, 0).fault);
    CHECK(!unit_combat_remove((uintptr_t)game) && !destroy_calls && !unit_combat_stop());
    *(unsigned int*)(list(5u) + 4) = 1u;
    CHECK(unit_combat_remove((uintptr_t)game) && unit_combat_stop());

    local = prepare();
    CHECK(unit_combat_init(image_base, create, destroy, animation) && unit_combat_spawn(&local) == 1);
    destroy_fault = 1;
    CHECK(!unit_combat_remove((uintptr_t)game) && destroy_calls == 1 && !unit_combat_stop());
    CHECK(!unit_combat_remove((uintptr_t)game) && destroy_calls == 1 && !unit_combat_stop());
    CHECK(!unit_combat_init(image_base, create, destroy, animation));
    /* World teardown, rather than another native deletion attempt, releases
       the abandoned fixture once map ownership of both private VIDs is gone. */
    clear_world();
    CHECK(unit_combat_remove((uintptr_t)game) && destroy_calls == 1 && unit_combat_stop());
    local = prepare();
    CHECK(unit_combat_init(image_base, create, destroy, animation) && unit_combat_stop());
    /* Two different compositions coexist with one shared factory binding.
       A host clone preserves death CRE while all pre-death creation is inert. */
    local = prepare();
    memcpy(gallery_sources[0], source, STEAM_VID_SIZE);
    memcpy(gallery_sources[1], source, STEAM_VID_SIZE);
    *(unsigned int*)(gallery_sources[0] + STEAM_VID_INDEX_OFFSET) = 40u;
    *(unsigned int*)(gallery_sources[1] + STEAM_VID_INDEX_OFFSET) = 80u;
    *(uintptr_t*)(gallery_sources[0] + STEAM_VID_LINKED_OFFSET) = 0;
    *(uintptr_t*)(gallery_sources[1] + STEAM_VID_CREATIONS_OFFSET + STEAM_ANIM_DEATH * 4u) = 0x1111u;
    *(uintptr_t*)(gallery_sources[1] + STEAM_VID_CREATIONS_OFFSET + STEAM_ANIM_GIB_DEATH * 4u) = 0x2222u;
    *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 40u * 4u) = (uintptr_t)gallery_sources[0];
    *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 80u * 4u) = (uintptr_t)gallery_sources[1];
    gallery_created = gallery_destroyed = 0; gallery_error = 0;
    CHECK(unit_combat_init(image_base, gallery_create, gallery_destroy, animation));
    UnitPuppetPose gallery_pose = {300.0f, 400.0f, 0, 0, 80u, 0};
    CHECK(unit_combat_spawn_type(&local, 40u, &gallery_pose, 0) == 1);
    CHECK(unit_combat_sample((uintptr_t)game, 1).private_entities == 1u);
    CHECK(unit_combat_select(1)); gallery_pose.x = 400.0f;
    CHECK(unit_combat_spawn_type(&local, 80u, &gallery_pose, 1) == 1);
    CHECK(unit_combat_sample((uintptr_t)game, 1).private_entities == 2u);
    CHECK(!unit_combat_sample((uintptr_t)game, 1).unexpected_entities);
    CHECK(!unit_combat_select(UNIT_COMBAT_CAPACITY));
    CHECK(unit_combat_sample((uintptr_t)game, 1).root == (uintptr_t)gallery_root[1]);
    CHECK(unit_combat_select(0));
    CHECK(!unit_combat_sample((uintptr_t)game, 1).unexpected_entities);
    CHECK(unit_combat_sample((uintptr_t)game, 1).sources_unchanged);
    CHECK(!unit_combat_stop() && unit_combat_remove((uintptr_t)game));
    CHECK(!unit_combat_stop()); /* Slot1 is still owned. */
    CHECK(unit_combat_select(1) && unit_combat_remove((uintptr_t)game));
    CHECK(unit_combat_stop() && unit_combat_select(0));
    CHECK(gallery_created == 2u && gallery_destroyed == 2u && !gallery_error);
    /* Armor creates a private intermediate, debris and living successor.
       The successor is associated by private VID, never proximity to stock60. */
    local = prepare_forms();
    CHECK(unit_combat_init(image_base, form_create, form_destroy, animation));
    UnitPuppetPose form_pose = {100.0f,200.0f,0,0,0,0};
    CHECK(unit_combat_spawn_type(&local,70u,&form_pose,1) == 1 && !form_error);
    uintptr_t armored = unit_combat_sample((uintptr_t)game,1).root;
    unsigned char* armor_vid = *(unsigned char**)(armored + STEAM_ENTITY_VID_OFFSET);
    *(unsigned int*)(armored + STEAM_ENTITY_ANIM_OFFSET) = STEAM_ANIM_DEATH;
    s = unit_combat_sample((uintptr_t)game,1);
    CHECK(s.transitioning && s.current_vid == 70u && s.form_revision == 2u);
    unsigned char* middle_vid = *(unsigned char**)(armor_vid + STEAM_VID_CREATIONS_OFFSET + 15u*4u);
    uintptr_t middle = form_spawn(middle_vid,0);
    unregister_entity(armored);
    s = unit_combat_sample((uintptr_t)game,1);
    CHECK(!s.fault && !s.root && s.transitioning && s.private_entities == 15u);
    unsigned char* successor_vid = *(unsigned char**)(middle_vid + STEAM_VID_CREATIONS_OFFSET + 15u*4u);
    uintptr_t successor = form_spawn(successor_vid,0); unregister_entity(middle);
    memset(unrelated,0,sizeof(unrelated));
    *(uintptr_t*)unrelated = image_base + STEAM_UNIT_VTABLE_RVA;
    *(uintptr_t*)(unrelated + STEAM_ENTITY_VID_OFFSET) = (uintptr_t)form_sources[4];
    add_entity(5u,unrelated);
    s = unit_combat_sample((uintptr_t)game,1);
    CHECK(!s.fault && s.root == successor && s.current_vid == 60u && !s.transitioning && s.form_revision == 3u);
    uintptr_t frozen = 0; unsigned int current = 0, revision = 0; int pending = 0;
    CHECK(unit_combat_observe_entity((uintptr_t)game,successor,&frozen,&current,&revision,&pending,69u) == 1);
    CHECK(frozen && frozen != (uintptr_t)form_sources[5]);
    CHECK(*(int*)(frozen + STEAM_VID_DAMAGE_OFFSET) == 0);
    *(uintptr_t*)(successor + STEAM_ENTITY_VID_OFFSET) = frozen;
    CHECK(unit_combat_observe_entity((uintptr_t)game,successor,&frozen,&current,&revision,&pending,0) == 1);
    CHECK(current == 69u && revision == 4u && !pending);
    s = unit_combat_sample((uintptr_t)game,1);
    CHECK(s.root == successor && s.current_vid == 69u && s.sources_unchanged && s.transitions == 3u);
    CHECK(unit_combat_remove((uintptr_t)game) && form_removed == 15u && !form_error);
    CHECK(*(unsigned int*)(list(5u)+4) == 1u && entries[5u][0] == (uintptr_t)unrelated);
    CHECK(unit_combat_stop());
    /* A final gib animation is terminal; it does not enter the armor chain. */
    local = prepare_forms(); CHECK(unit_combat_init(image_base,form_create,form_destroy,animation));
    CHECK(unit_combat_spawn_type(&local,70u,&form_pose,1) == 1);
    armored = unit_combat_sample((uintptr_t)game,0).root;
    *(unsigned int*)(armored + STEAM_ENTITY_ANIM_OFFSET) = STEAM_ANIM_GIB_DEATH;
    CHECK(!unit_combat_sample((uintptr_t)game,0).transitioning);
    CHECK(unit_combat_remove((uintptr_t)game) && unit_combat_stop() && !form_error);
    /* Predictive armor is immediately damageable, but its native descriptor
       cannot create an intermediate or successor while awaiting authority. */
    local = prepare_forms(); CHECK(unit_combat_init(image_base,form_create,form_destroy,animation));
    CHECK(unit_combat_spawn_predictive(&local,70u,&form_pose) == 1 && !form_error);
    armored = unit_combat_sample((uintptr_t)game,1).root;
    armor_vid = *(unsigned char**)(armored + STEAM_ENTITY_VID_OFFSET);
    CHECK(inert_descriptor(armor_vid));
    CHECK(*(int*)(armor_vid + STEAM_VID_HEALTH_OFFSET) == *(int*)(form_sources[0] + STEAM_VID_HEALTH_OFFSET));
    CHECK(*(unsigned int*)(armor_vid + STEAM_VID_COLLISION_MASK_OFFSET) ==
        *(unsigned int*)(form_sources[0] + STEAM_VID_COLLISION_MASK_OFFSET));
    *(unsigned int*)(armored + STEAM_ENTITY_ANIM_OFFSET) = STEAM_ANIM_DEATH;
    s = unit_combat_sample((uintptr_t)game,1);
    CHECK(s.transitioning && s.form_revision == 2u && s.private_entities == 1u && s.sources_unchanged);
    CHECK(unit_combat_remove((uintptr_t)game) && unit_combat_stop() && form_removed == 1u && !form_error);
    /* A predicted frozen form inherits native incoming health/resistance but
       suppresses every creation link, including the stock ice death graph. */
    local = prepare_forms();
    *(uintptr_t*)(form_sources[5] + STEAM_VID_CREATIONS_OFFSET + STEAM_ANIM_DEATH * 4u) = 0x1111u;
    CHECK(unit_combat_init(image_base,form_create,form_destroy,animation));
    CHECK(unit_combat_spawn_predictive(&local,60u,&form_pose) == 1 && !form_error);
    successor = unit_combat_sample((uintptr_t)game,1).root; frozen = 0;
    CHECK(unit_combat_observe_entity((uintptr_t)game,successor,&frozen,&current,&revision,&pending,69u) == 1);
    CHECK(frozen && frozen != (uintptr_t)form_sources[5] && inert_descriptor((unsigned char*)frozen));
    CHECK(*(int*)(frozen + STEAM_VID_HEALTH_OFFSET) == 100);
    CHECK(*(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + 69u*4u) == (uintptr_t)form_sources[5]);
    *(uintptr_t*)(successor + STEAM_ENTITY_VID_OFFSET) = frozen;
    CHECK(unit_combat_observe_entity((uintptr_t)game,successor,&frozen,&current,&revision,&pending,0) == 1);
    CHECK(current == 69u && revision == 2u && !pending);
    CHECK(unit_combat_remove((uintptr_t)game) && unit_combat_stop() && form_removed == 1u && !form_error);
    CHECK(VirtualFree((void*)image_base, 0, MEM_RELEASE));
    printf("UNIT combat private-descriptor and cleanup checks passed.\n");
    return 0;
}
