/* Puppet slots: borrow existing native UNIT monsters (and their class-9 part) so a
   peer's replicated pose can drive them, or so the host can watch them. Layer: game/
   (engine API). Hooks the UNIT/SPRITE vtable slots; each native callback finds its slot by
   entity identity and passes unowned entities straight to the engine. A slot never owns
   the native entity. Everything runs on the game thread, native access inside SEH. */

#include <string.h>
#include <math.h>
#include <float.h>
#include "unit_puppet.h"
#include "../../../common/src/monster_vids.h"
#include "slot_hook.h"
#include "world_list.h"

/* Largest health or damage value a caller may pass to a native action. */
#define PUPPET_HEALTH_LIMIT 100000
/* SET_HP value that makes the engine choose the gib death instead of the normal one. */
#define PUPPET_GIB_HEALTH (-PUPPET_HEALTH_LIMIT)
/* A replay sequence this far ahead or more is treated as older (wrapped). */
#define PUPPET_SEQUENCE_HALF_RANGE 0x80000000u

enum PuppetMode
{
    PUPPET_CONTROLLED,
    PUPPET_WATCH,
    PUPPET_STATIONARY
};

/* One borrowed UNIT (and optional class-9 part). The hooks find the slot that
   owns an entity with route(); public functions receive the slot explicitly. */
struct UnitPuppet
{
    enum UnitPuppetState state;
    enum PuppetMode watching;
    uintptr_t game;
    uintptr_t entity; /* Borrowed native root; never owned. */
    uintptr_t vid;    /* Descriptor the root used when last validated. */
    unsigned int category;
    uintptr_t child; /* Borrowed native part, or 0. */
    uintptr_t child_vid;
    unsigned int behavior; /* Native behavior restored on release. */
    UnitPuppetPose pose;   /* Last pose applied to a controlled entity. */
    uint32_t last_event;   /* Last replayed event sequence. */
    int combat;
    int predictive;
    unsigned int captured_attacks;
    unsigned int predicted_hits;

    /* Animation frames seen at the last sample, to count native progression. */
    struct
    {
        unsigned int last_frame;
        unsigned int last_animation;
        unsigned int last_first;
        unsigned int child_last_frame;
        unsigned int child_last_first;
        unsigned int child_last_animation;
    } frames;

    struct
    {
        unsigned int current_vid;
        unsigned int revision;
        unsigned int captured;
        int transitioning;
        uintptr_t changing_vid; /* Private form during an in-flight CHANGE_VID. */
    } form;

    struct
    {
        int dying;
        unsigned int captured;
        unsigned int animation;
        UnitPuppetPose pose;
    } death;

    /* Diagnostic counters only; nothing above depends on them. */
    struct
    {
        unsigned int decisions;
        unsigned int movements;
        unsigned int blocked;
        unsigned int frame_changes;
        unsigned int child_frame_changes;
        unsigned int attack_frame_changes;
        unsigned int replayed;
        unsigned int duplicates;
    } stats;
};

static UnitPuppet puppets[UNIT_PUPPET_CAPACITY];
static struct
{
    uintptr_t base;
    UnitPuppetEngine api;
    SlotHook action, movement, destroy, part_action, part_destroy;
    UnitFormObserver forms;
} shared;

/* Registers the callback that supplies private form descriptors (unit_combat's observer). */
void unit_puppet_set_form_observer(UnitFormObserver observer)
{
    shared.forms = observer;
}

/* Returns slot `index`, or NULL when out of range. */
UnitPuppet* unit_puppet_slot(unsigned int index)
{
    if (index >= UNIT_PUPPET_CAPACITY)
        return NULL;
    return &puppets[index];
}

/* Finds the slot that currently owns a native root (part = 0) or part entity.
   NULL means no slot owns it, and the hook passes the call to the engine. */
static UnitPuppet* route(void* entity, int part)
{
    for (unsigned int i = 0; i < UNIT_PUPPET_CAPACITY; ++i)
    {
        uintptr_t owned = part ? puppets[i].child : puppets[i].entity;
        int live = puppets[i].state == UNIT_PUPPET_ACTIVE || puppets[i].state == UNIT_PUPPET_FAULT;
        if (owned == (uintptr_t)entity && live)
            return &puppets[i];
    }
    return NULL;
}

/* Verifies the native function bytes and fills `out`; hooks nothing. */
enum UnitResult unit_puppet_bind(uintptr_t base, UnitPuppetEngine* out)
{
    static const struct
    {
        unsigned int rva;
        unsigned char bytes[8];
    } signatures[] = {{STEAM_UNIT_DESTROY_RVA, {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x90, 0x7C}},
                      {STEAM_UNIT_ACTION_RVA, {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0xC0, 0x7C}},
                      {STEAM_UNIT_UPDATE_RVA, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10, 0x56, 0x57}},
                      {STEAM_UNIT_MOVEMENT_RVA, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10, 0x53, 0x56}},
                      {STEAM_SPRITE_DESTROY_RVA, {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0xE8, 0x55}},
                      {STEAM_SPRITE_ACTION_RVA, {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0xCF, 0x7A}},
                      {STEAM_SET_ANIMATION_RVA, {0x55, 0x8B, 0xEC, 0x51, 0x53, 0x56, 0x57, 0x8B}},
                      {STEAM_ACTOR_MOVE_RVA, {0x55, 0x8B, 0xEC, 0xF3, 0x0F, 0x10, 0x4D, 0x08}},
                      {STEAM_ACTOR_ROTATE_RVA, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10, 0x8A, 0x45}}};
    memset(out, 0, sizeof(*out));
    __try
    {
        for (unsigned int i = 0; i < sizeof(signatures) / sizeof(signatures[0]); ++i)
            if (memcmp((void*)(base + signatures[i].rva), signatures[i].bytes, 8))
                return UNIT_REFUSED;
        const unsigned int rvas[] = {STEAM_UNIT_DESTROY_RVA, STEAM_UNIT_ACTION_RVA,
                                     STEAM_UNIT_UPDATE_RVA, STEAM_UNIT_MOVEMENT_RVA};
        for (unsigned int i = 0; i < 4; ++i)
            if (*(uintptr_t*)(base + STEAM_UNIT_VTABLE_RVA + i * sizeof(uintptr_t)) !=
                base + rvas[i])
                return UNIT_REFUSED;
        if (*(uintptr_t*)(base + STEAM_SPRITE_VTABLE_RVA) != base + STEAM_SPRITE_DESTROY_RVA ||
            *(uintptr_t*)(base + STEAM_SPRITE_VTABLE_RVA + 4u) != base + STEAM_SPRITE_ACTION_RVA ||
            *(uintptr_t*)(base + STEAM_SPRITE_VTABLE_RVA + 8u) != base + STEAM_UNIT_UPDATE_RVA)
            return UNIT_REFUSED;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return UNIT_REFUSED;
    }
    out->action = (ActorAction)(base + STEAM_UNIT_ACTION_RVA);
    out->destroy = (ActorDestroy)(base + STEAM_UNIT_DESTROY_RVA);
    out->movement = (UnitMovement)(base + STEAM_UNIT_MOVEMENT_RVA);
    out->animation = (UnitAnimation)(base + STEAM_SET_ANIMATION_RVA);
    out->move = (ActorMove)(base + STEAM_ACTOR_MOVE_RVA);
    out->rotate = (ActorRotate)(base + STEAM_ACTOR_ROTATE_RVA);
    out->part_action = (ActorAction)(base + STEAM_SPRITE_ACTION_RVA);
    out->part_destroy = (ActorDestroy)(base + STEAM_SPRITE_DESTROY_RVA);
    return UNIT_OK;
}

/* Caller is inside SEH. Never dereference a cached entity before finding it
   in the current map list. Destructor hook invalidates before native deletion,
   including same-address/same-VID allocation reuse in the same world. */
static int registered(UnitPuppet* puppet, uintptr_t game)
{
    if (!puppet->entity || !game || game != puppet->game)
        return 0;
    if (*(uintptr_t*)game != shared.base + STEAM_GAME_VTABLE_RVA)
        return 0;
    if (!world_list_find(game, puppet->category, puppet->entity))
        return 0;

    if (*(uintptr_t*)puppet->entity != shared.base + STEAM_UNIT_VTABLE_RVA)
        return 0;
    return *(uintptr_t*)(puppet->entity + STEAM_ENTITY_VID_OFFSET) == puppet->vid;
}

/* Forgets the borrowed pointers after the world dropped the entity; the slot goes to LOST. */
static void mark_entity_lost(UnitPuppet* puppet)
{
    puppet->entity = 0;
    puppet->child = 0;
    puppet->state = UNIT_PUPPET_LOST;
}

/* True while the slot holds an entity: ACTIVE, or FAULT so the hooks keep blocking it. */
static int holds_entity(const UnitPuppet* puppet)
{
    return puppet->state == UNIT_PUPPET_ACTIVE || puppet->state == UNIT_PUPPET_FAULT;
}

/* True while this slot controls `e`. */
static int owns_entity(const UnitPuppet* puppet, void* e)
{
    return puppet->entity == (uintptr_t)e && holds_entity(puppet);
}

/* Same as owns_entity for the slot's class-9 part. */
static int owns_part(const UnitPuppet* puppet, void* e)
{
    return puppet->child && puppet->child == (uintptr_t)e && holds_entity(puppet);
}

/* Child is dereferenced only after finding it in its current category list. */
static int child_registered(UnitPuppet* puppet)
{
    if (!puppet->child)
        return 1;
    if (!registered(puppet, puppet->game))
        return 0;
    if (*(uintptr_t*)(puppet->entity + STEAM_ENTITY_CHILD_OFFSET) != puppet->child)
        return 0;

    unsigned int category = *(unsigned int*)(puppet->child_vid + STEAM_VID_CATEGORY_OFFSET);
    if (category >= STEAM_TICK_ENTITY_LIST_COUNT)
        return 0;
    if (!world_list_find(puppet->game, category, puppet->child))
        return 0;

    uintptr_t child = puppet->child;
    if (*(uintptr_t*)child != shared.base + STEAM_SPRITE_VTABLE_RVA)
        return 0;
    if (*(uintptr_t*)(child + STEAM_ENTITY_VID_OFFSET) != puppet->child_vid)
        return 0;
    if (*(uintptr_t*)(child + STEAM_ENTITY_CHILD_OFFSET))
        return 0;
    return *(uintptr_t*)(child + STEAM_ENTITY_PARENT_OFFSET) == puppet->entity;
}

/* Chooses run or idle from the last pose, because the native AI decision is skipped. */
static void select_animation(UnitPuppet* puppet, void* entity)
{
    unsigned int animation = STEAM_ANIM_IDLE;
    if (puppet->pose.moving && puppet->pose.velocity != 0)
        animation = STEAM_ANIM_RUN;
    shared.api.animation(entity, NULL, animation);
}

/* Records the death animation and pose so the host can send them as an event. */
static void capture_death(UnitPuppet* puppet, uintptr_t e)
{
    puppet->death.dying = 1;
    ++puppet->death.captured;
    unsigned int a = *(unsigned int*)(e + STEAM_ENTITY_ANIM_OFFSET);
    puppet->death.animation = a == STEAM_ANIM_GIB_DEATH ? a : STEAM_ANIM_DEATH;
    puppet->death.pose.x = *(float*)(e + STEAM_ENTITY_X_OFFSET);
    puppet->death.pose.y = *(float*)(e + STEAM_ENTITY_Y_OFFSET);
    puppet->death.pose.z = *(float*)(e + STEAM_ENTITY_Z_OFFSET);
    puppet->death.pose.direction = *(unsigned char*)(e + STEAM_ENTITY_DIRECTION_OFFSET);
}

/* Caller in SEH. The engine's stock descriptor for table index `index`. */
static uintptr_t stock_vid(uintptr_t game, unsigned int index)
{
    return *(uintptr_t*)(game + STEAM_GAME_VID_TABLE_OFFSET + index * sizeof(uintptr_t));
}

/* Caller in SEH. True when a watched stock monster changed to the stock
   frozen form of its current origin. */
static int is_stock_frozen_form(const UnitPuppet* puppet, uintptr_t actual)
{
    unsigned int frozen = puppet->form.current_vid + MONSTER_VID_FROZEN_DELTA;
    if (!mp_monster_origin(puppet->form.current_vid) || frozen >= STEAM_STOCK_VID_LIMIT)
        return 0;
    if (!actual || actual != stock_vid(puppet->game, frozen))
        return 0;
    if (*(unsigned int*)(actual + STEAM_VID_INDEX_OFFSET) != frozen)
        return 0;
    return *(unsigned int*)(actual + STEAM_VID_CLASS_OFFSET) == STEAM_UNIT_CLASS;
}

/* Caller in SEH. Follows the entity to its new descriptor vid (stock index
   `index`) as a new form revision. Returns UNIT_FAULT for a bad category. */
static enum UnitResult adopt_form(UnitPuppet* puppet, uintptr_t entity, uintptr_t vid,
                                  unsigned int index, unsigned int revision)
{
    unsigned int category = *(unsigned int*)(vid + STEAM_VID_CATEGORY_OFFSET);
    if (category >= STEAM_TICK_ENTITY_LIST_COUNT)
    {
        puppet->state = UNIT_PUPPET_FAULT;
        return UNIT_FAULT;
    }
    puppet->vid = vid;
    puppet->form.current_vid = index;
    puppet->category = category;
    puppet->form.revision = revision;
    ++puppet->form.captured;
    puppet->death.dying = 0;
    puppet->behavior = *(unsigned int*)(entity + STEAM_UNIT_BEHAVIOR_OFFSET);
    puppet->child = *(uintptr_t*)(entity + STEAM_ENTITY_CHILD_OFFSET);
    puppet->child_vid = 0;
    if (puppet->child)
        puppet->child_vid = *(uintptr_t*)(puppet->child + STEAM_ENTITY_VID_OFFSET);
    return UNIT_OK;
}

/* Called only while the entity is live in a native action or after locating it
   in its category list. Native CHANGE_VID performs child teardown and counters;
   observe the resulting descriptor rather than editing those structures.
   Returns the observer's result: UNIT_OK when a form was prepared or adopted. */
static enum UnitResult observe_form(UnitPuppet* puppet, uintptr_t entity, unsigned int requested,
                                    uintptr_t* prepared)
{
    uintptr_t vid = 0;
    unsigned int index = puppet->form.current_vid;
    unsigned int revision = puppet->form.revision;
    int transitioning = puppet->form.transitioning;
    enum UnitResult result = UNIT_REFUSED;
    if (shared.forms)
    {
        result =
            shared.forms(puppet->game, entity, &vid, &index, &revision, &transitioning, requested);
    }
    if (result == UNIT_FAULT)
    {
        puppet->state = UNIT_PUPPET_FAULT;
        return UNIT_FAULT;
    }

    if (requested)
    {
        if (prepared)
            *prepared = result == UNIT_OK ? vid : 0;
        return result;
    }

    /* Without an observer answer, a watched stock monster may still have frozen. */
    uintptr_t actual = *(uintptr_t*)(entity + STEAM_ENTITY_VID_OFFSET);
    if (result == UNIT_REFUSED && actual != puppet->vid && puppet->watching)
    {
        if (!is_stock_frozen_form(puppet, actual))
            return UNIT_REFUSED;
        vid = actual;
        index = puppet->form.current_vid + MONSTER_VID_FROZEN_DELTA;
        revision = puppet->form.revision + 1u;
        result = UNIT_OK;
    }
    if (result != UNIT_OK)
        return result;

    puppet->form.transitioning = transitioning;
    int entity_changed_form = vid && actual == vid && vid != puppet->vid;
    if (entity_changed_form && adopt_form(puppet, entity, vid, index, revision) == UNIT_FAULT)
        return UNIT_FAULT;
    return result;
}

static int sanitized(uintptr_t vid);

/* Caller in SEH. True when `vid` is a private, sanitized descriptor that a predictive
   copy may use; a root descriptor must also have an idle behavior. */
static int prediction_descriptor(const UnitPuppet* puppet, uintptr_t vid, int root)
{
    if (!vid || !sanitized(vid))
        return 0;
    if (*(int*)(vid + STEAM_VID_CREATE_SCRIPT_OFFSET) != -1)
        return 0;

    unsigned int index = *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET);
    if (index >= STEAM_STOCK_VID_LIMIT)
        return 0;
    /* A stock descriptor is allowed only while it is our in-flight private form. */
    if (vid == stock_vid(puppet->game, index) && vid != puppet->form.changing_vid)
        return 0;

    if (root)
    {
        uintptr_t behavior = *(uintptr_t*)(vid + STEAM_VID_BEHAVIOR_OFFSET);
        if (!behavior || *(unsigned int*)(behavior + STEAM_BEHAVIOR_UNIT_FLAGS_OFFSET))
            return 0;
    }
    return 1;
}

/* Before native CHANGE_VID on our root: asks the form observer for a private
   descriptor for table index a. Returns 0 when the change must be blocked. */
static int prepare_change_vid(UnitPuppet* puppet, void* e, intptr_t a, uintptr_t* prepared)
{
    if (puppet->predictive)
    {
        /* A predictive copy may only freeze, into the frozen form of its origin. */
        if (!mp_monster_origin(puppet->form.current_vid))
            return 0;
        if (a != (intptr_t)(puppet->form.current_vid + MONSTER_VID_FROZEN_DELTA))
            return 0;
    }

    int stock_index = a > 0 && a < STEAM_STOCK_VID_LIMIT;
    if (stock_index && observe_form(puppet, (uintptr_t)e, (unsigned int)a, prepared) == UNIT_FAULT)
        return 0;

    /* Prediction must never escape through an unsupported stock descriptor. */
    if (puppet->predictive)
    {
        if (!*prepared || !prediction_descriptor(puppet, *prepared, 1))
            return 0;
    }
    return 1;
}

/* Runs native CHANGE_VID with table entry a temporarily pointing at the
   prepared private form. The entry is restored even when the constructor
   raises SEH. */
static int change_vid_to_prepared(UnitPuppet* puppet, void* e, unsigned int kind, intptr_t a,
                                  intptr_t b, intptr_t c, ActorAction original, uintptr_t prepared)
{
    int result = 0;
    uintptr_t* entry = (uintptr_t*)(puppet->game + STEAM_GAME_VID_TABLE_OFFSET +
                                    (unsigned int)a * sizeof(uintptr_t));
    uintptr_t saved = *entry;
    uintptr_t saved_changing = puppet->form.changing_vid;
    __try
    {
        *entry = prepared;
        puppet->form.changing_vid = prepared;
        result = original(e, NULL, kind, a, b, c);
    }
    __finally
    {
        *entry = saved;
        puppet->form.changing_vid = saved_changing;
    }
    return result;
}

/* True for the native attack: USE_AMMO with no ammo cost requested (a <= 0). */
static int is_attack(unsigned int kind, intptr_t a)
{
    return kind == STEAM_ACT_USE_AMMO && a <= 0;
}

/* Watch mode: runs the original action, then counts attacks and observes the form.
   Only CHANGE_VID is altered (a prepared private form replaces the stock index). */
static int observed_action(UnitPuppet* puppet, void* e, unsigned int kind, intptr_t a, intptr_t b,
                           intptr_t c, ActorAction original)
{
    int result = 0;
    /* CHANGE_VID takes a table index, not a VID pointer. A scoped substitution
       lets the native path perform linked teardown, health and VID counters on
       a sanitized private form. */
    uintptr_t prepared = 0;
    int on_root = (uintptr_t)e == puppet->entity;
    if (on_root && kind == STEAM_ACT_CHANGE_VID && !prepare_change_vid(puppet, e, a, &prepared))
        return 0;
    if (on_root && puppet->predictive && kind == STEAM_ACT_DAMAGE)
        ++puppet->predicted_hits;

    if (prepared)
        result = change_vid_to_prepared(puppet, e, kind, a, b, c, original, prepared);
    else
        result = original(e, NULL, kind, a, b, c);

    /* Verified native ExecuteCommand(46A8A0) calls USE_AMMO then animation8.
       It also does this for zero-cost melee; no pose/frame inference is used. */
    if (is_attack(kind, a) && !puppet->death.dying)
        ++puppet->captured_attacks;

    __try
    {
        if (puppet->entity)
            observe_form(puppet, puppet->entity, 0, NULL);
        /* Nested DAMAGE can select death before F145_15 changes to frozen VID.
           Final death is decided at the game-tick sample boundary/destructor. */
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        puppet->state = UNIT_PUPPET_FAULT;
    }
    return result;
}

/* Caller in SEH. Native animation completion may leave pain and resume the
   replicated movement pose, but never reset frozen/death forms. */
static void resume_predicted_pose(UnitPuppet* puppet, void* e, int part)
{
    if (puppet->death.dying || puppet->form.transitioning)
        return;
    if (mp_monster_frozen(puppet->form.current_vid))
        return;

    unsigned int anim = *(unsigned int*)(puppet->entity + STEAM_ENTITY_ANIM_OFFSET);
    switch (anim)
    {
    case STEAM_ANIM_IDLE:
    case STEAM_ANIM_RUN:
    case STEAM_ANIM_ATTACK:
    case STEAM_ANIM_PAIN:
    case STEAM_ANIM_PAIN_ALT:
        break;
    default:
        return;
    }

    if (part)
        shared.api.animation(e, NULL, STEAM_ANIM_IDLE);
    else
        select_animation(puppet, e);
}

/* Actions a predictive copy passes to the engine: incoming damage, health,
   its own form change, idle/stop and the native bookkeeping queries. */
static int prediction_allows(unsigned int kind, intptr_t a, int part)
{
    switch (kind)
    {
    case STEAM_ACT_DAMAGE:
    case STEAM_ACT_SET_HP:
    case STEAM_ACT_GET_HP:
    case STEAM_ACT_CLEAR_COMMAND:
    case STEAM_ACT_CLEAR_STACK:
    case STEAM_ACT_STOP:
    case STEAM_ACT_BOOKKEEPING_5C:
    case STEAM_ACT_BOOKKEEPING_5E:
    case STEAM_ACT_BOOKKEEPING_60:
        return 1;
    case STEAM_ACT_CHANGE_VID:
        return !part;
    case STEAM_ACT_SET_ANIMATION:
        return a == STEAM_ANIM_IDLE;
    case STEAM_ACT_SET_BEHAVIOR:
        return a == 0;
    default:
        return 0;
    }
}

/* Actions a controlled (movement-only) copy passes to the engine: animation changes,
   health queries and native bookkeeping. Everything else is blocked. */
static int controlled_allows(unsigned int kind)
{
    switch (kind)
    {
    case STEAM_ACT_SET_ANIMATION:
    case STEAM_ACT_GET_HP:
    case STEAM_ACT_BOOKKEEPING_5C:
    case STEAM_ACT_BOOKKEEPING_5E:
    case STEAM_ACT_BOOKKEEPING_60:
        return 1;
    default:
        return 0;
    }
}

/* True for the actions a stationary gallery monster skips: AI decisions and attacks. */
static int stationary_skips(unsigned int kind, intptr_t a)
{
    return kind == STEAM_ACT_NEXT_COMMAND || is_attack(kind, a);
}

/* Caller in SEH. True when the root, its part and both descriptors are still the
   private, registered objects a predictive copy expects. */
static int prediction_intact(UnitPuppet* puppet)
{
    if (!child_registered(puppet))
        return 0;
    if (!prediction_descriptor(puppet, puppet->vid, 1))
        return 0;
    if (puppet->child && !prediction_descriptor(puppet, puppet->child_vid, 0))
        return 0;
    return 1;
}

static int prediction_action(UnitPuppet* puppet, void* e, unsigned int kind, intptr_t a, intptr_t b,
                             intptr_t c, ActorAction original, int part)
{
    __try
    {
        if (puppet->state == UNIT_PUPPET_FAULT)
            return 0;
        /* Native CHANGE_VID can recurse into actions after assigning its new
           descriptor. Adopt only our explicitly in-flight private form. */
        uintptr_t entity_vid = *(uintptr_t*)((uintptr_t)e + STEAM_ENTITY_VID_OFFSET);
        if (!part && puppet->form.changing_vid && entity_vid == puppet->form.changing_vid)
            observe_form(puppet, (uintptr_t)e, 0, NULL);

        if (!registered(puppet, puppet->game))
        {
            mark_entity_lost(puppet);
            return 0;
        }
        if (!prediction_intact(puppet))
        {
            puppet->state = UNIT_PUPPET_FAULT;
            return 0;
        }

        if (kind == STEAM_ACT_NEXT_COMMAND)
        {
            ++puppet->stats.decisions;
            resume_predicted_pose(puppet, e, part);
            return 0;
        }
        if (prediction_allows(kind, a, part))
            return observed_action(puppet, e, kind, a, b, c, original);
        ++puppet->stats.blocked;
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        puppet->state = UNIT_PUPPET_FAULT;
        return 0;
    }
}

/* Action hook body for the root: routes to prediction, watch or controlled handling.
   A controlled slot lets only the movement-related actions through and counts the rest. */
static int scoped_action(UnitPuppet* puppet, void* e, unsigned int kind, intptr_t a, intptr_t b,
                         intptr_t c)
{
    if (puppet->predictive && owns_entity(puppet, e))
        return prediction_action(puppet, e, kind, a, b, c, shared.api.action, 0);
    if (puppet->watching && owns_entity(puppet, e))
    {
        if (puppet->watching == PUPPET_STATIONARY && stationary_skips(kind, a))
        {
            ++puppet->stats.decisions;
            return 0;
        }
        return observed_action(puppet, e, kind, a, b, c, shared.api.action);
    }

    if (owns_entity(puppet, e))
    {
        if (puppet->state == UNIT_PUPPET_FAULT && kind != STEAM_ACT_SET_ANIMATION)
            return 0;
        __try
        {
            if (!registered(puppet, puppet->game))
            {
                mark_entity_lost(puppet);
            }
            else if (kind == STEAM_ACT_NEXT_COMMAND)
            {
                ++puppet->stats.decisions;
                if (!puppet->death.dying)
                    select_animation(puppet, e);
                return 0;
            }
            else if (!controlled_allows(kind))
            {
                /* This movement-only experiment rejects ALL other actions,
                   including scripts assigning targets/stack, damage and HP.
                   It is not the future combat replay policy. */
                ++puppet->stats.blocked;
                return 0;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            puppet->state = UNIT_PUPPET_FAULT;
            return 0;
        }
    }
    return shared.api.action(e, NULL, kind, a, b, c);
}

/* Movement hook body: a controlled entity skips native movement; watch mode runs it. */
static void scoped_movement(UnitPuppet* puppet, void* e)
{
    if (puppet->watching == PUPPET_WATCH && owns_entity(puppet, e))
    {
        shared.api.movement(e, NULL);
        return;
    }
    if (owns_entity(puppet, e))
    {
        if (puppet->state == UNIT_PUPPET_FAULT)
            return;
        __try
        {
            if (registered(puppet, puppet->game))
            {
                ++puppet->stats.movements;
                return;
            }
            mark_entity_lost(puppet);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            puppet->state = UNIT_PUPPET_FAULT;
            return;
        }
    }
    shared.api.movement(e, NULL);
}

/* Destructor hook body: before native deletion a watched or predicted entity records its
   death or armor transition, then the slot is invalidated so no stale pointer is used. */
static void* scoped_destroy(UnitPuppet* puppet, void* e, int release)
{
    int observed = puppet->watching || puppet->predictive;
    if (observed && puppet->entity == (uintptr_t)e && !puppet->death.dying)
    {
        __try
        {
            observe_form(puppet, (uintptr_t)e, 0, NULL);

            /* An armored monster "dies" when it loses its armor; that is a form change. */
            unsigned int anim = *(unsigned int*)((uintptr_t)e + STEAM_ENTITY_ANIM_OFFSET);
            if (mp_monster_armored(puppet->form.current_vid) && anim == STEAM_ANIM_DEATH)
                puppet->form.transitioning = 1;
            if (!puppet->form.transitioning)
                capture_death(puppet, (uintptr_t)e);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            puppet->state = UNIT_PUPPET_FAULT;
        }
    }
    if (puppet->entity == (uintptr_t)e)
        mark_entity_lost(puppet);
    return shared.api.destroy(e, NULL, release);
}

static int scoped_part_action(UnitPuppet* puppet, void* e, unsigned int kind, intptr_t a,
                              intptr_t b, intptr_t c)
{
    if (puppet->predictive && owns_part(puppet, e))
        return prediction_action(puppet, e, kind, a, b, c, shared.api.part_action, 1);
    if (puppet->watching && owns_part(puppet, e))
    {
        if (puppet->watching == PUPPET_STATIONARY && stationary_skips(kind, a))
        {
            ++puppet->stats.decisions;
            return 0;
        }
        return observed_action(puppet, e, kind, a, b, c, shared.api.part_action);
    }

    if (owns_part(puppet, e))
    {
        __try
        {
            if (!child_registered(puppet))
            {
                puppet->state = UNIT_PUPPET_FAULT;
                return 0;
            }
            if (kind == STEAM_ACT_NEXT_COMMAND)
            {
                ++puppet->stats.decisions;
                if (puppet->combat && !puppet->death.dying)
                    shared.api.animation(e, NULL, STEAM_ANIM_IDLE);
                return 0;
            }
            if (!controlled_allows(kind))
            {
                ++puppet->stats.blocked;
                return 0;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            puppet->state = UNIT_PUPPET_FAULT;
            return 0;
        }
    }
    return shared.api.part_action(e, NULL, kind, a, b, c);
}

/* Destructor hook body for the part: forgets it, and faults a controlled slot. */
static void* scoped_part_destroy(UnitPuppet* puppet, void* e, int release)
{
    if (puppet->child == (uintptr_t)e)
    {
        puppet->child = 0;
        if (!puppet->death.dying && !puppet->watching && !puppet->predictive)
            puppet->state = UNIT_PUPPET_FAULT;
    }
    return shared.api.part_destroy(e, NULL, release);
}

/* Vtable hooks. Native callbacks may interleave slots, including recursive
   root/part calls, so each call looks up its own slot by entity identity.
   Entities that no slot owns go straight to the original engine function. */
static int __fastcall on_action(void* e, void* unused, unsigned int kind, intptr_t a, intptr_t b,
                                intptr_t c)
{
    (void)unused;
    UnitPuppet* puppet = route(e, 0);
    if (!puppet)
        return shared.api.action(e, NULL, kind, a, b, c);
    return scoped_action(puppet, e, kind, a, b, c);
}

static void __fastcall on_movement(void* e, void* unused)
{
    (void)unused;
    UnitPuppet* puppet = route(e, 0);
    if (!puppet)
    {
        shared.api.movement(e, NULL);
        return;
    }
    scoped_movement(puppet, e);
}

static void* __fastcall on_destroy(void* e, void* unused, int release)
{
    (void)unused;
    UnitPuppet* puppet = route(e, 0);
    if (!puppet)
        return shared.api.destroy(e, NULL, release);
    return scoped_destroy(puppet, e, release);
}

static int __fastcall on_part_action(void* e, void* unused, unsigned int kind, intptr_t a,
                                     intptr_t b, intptr_t c)
{
    (void)unused;
    UnitPuppet* puppet = route(e, 1);
    if (!puppet)
        return shared.api.part_action(e, NULL, kind, a, b, c);
    return scoped_part_action(puppet, e, kind, a, b, c);
}

static void* __fastcall on_part_destroy(void* e, void* unused, int release)
{
    (void)unused;
    UnitPuppet* puppet = route(e, 1);
    if (!puppet)
        return shared.api.part_destroy(e, NULL, release);
    return scoped_part_destroy(puppet, e, release);
}

/* Hooks the five vtable slots. Call unit_puppet_stop to roll back. */
/* True when every engine function the hooks call is present. */
static int engine_complete(const UnitPuppetEngine* api)
{
    if (!api->action || !api->destroy || !api->movement || !api->animation)
        return 0;
    if (!api->move || !api->rotate)
        return 0;
    return api->part_action && api->part_destroy;
}

/* True while any of the five vtable slots is still hooked. */
static int hooks_installed(void)
{
    return shared.action.slot || shared.movement.slot || shared.destroy.slot ||
           shared.part_action.slot || shared.part_destroy.slot;
}

/* Installs the five vtable hooks, parts first. Returns 0 at the first failure. */
static int install_hooks(uintptr_t base, const UnitPuppetEngine* api)
{
    void* volatile* vt = (void* volatile*)(base + STEAM_UNIT_VTABLE_RVA);
    void* volatile* part_vt = (void* volatile*)(base + STEAM_SPRITE_VTABLE_RVA);

    if (slot_install(&shared.part_destroy, part_vt + STEAM_ENTITY_DESTROY_SLOT,
                     (void*)api->part_destroy, (void*)on_part_destroy) != SLOT_OK)
        return 0;
    if (slot_install(&shared.part_action, part_vt + STEAM_ENTITY_ACTION_SLOT,
                     (void*)api->part_action, (void*)on_part_action) != SLOT_OK)
        return 0;
    if (slot_install(&shared.destroy, vt + STEAM_ENTITY_DESTROY_SLOT, (void*)api->destroy,
                     (void*)on_destroy) != SLOT_OK)
        return 0;
    if (slot_install(&shared.movement, vt + STEAM_UNIT_MOVEMENT_SLOT, (void*)api->movement,
                     (void*)on_movement) != SLOT_OK)
        return 0;
    return slot_install(&shared.action, vt + STEAM_ENTITY_ACTION_SLOT, (void*)api->action,
                        (void*)on_action) == SLOT_OK;
}

enum UnitResult unit_puppet_install(uintptr_t base, const UnitPuppetEngine* api)
{
    if (hooks_installed() || !base || !api || !engine_complete(api))
        return UNIT_REFUSED;

    memset(puppets, 0, sizeof(puppets));
    memset(&shared, 0, sizeof(shared));
    shared.base = base;
    shared.api = *api;
    if (!install_hooks(base, api))
    {
        unit_puppet_stop();
        return UNIT_REFUSED;
    }

    for (unsigned int i = 0; i < UNIT_PUPPET_CAPACITY; ++i)
        puppets[i].state = UNIT_PUPPET_READY;
    return UNIT_OK;
}

/* What attach looks for: any suitable UNIT except the player, or exactly
   `required` when it is nonzero, in the given mode. */
typedef struct AttachRequest
{
    uintptr_t game;
    uintptr_t player;
    uintptr_t required;
    enum PuppetMode watching;
} AttachRequest;

/* Caller in SEH. True when e (found in list cat) is a live UNIT in a
   supported animation that this request may attach to. */
static int unit_candidate(const AttachRequest* request, uintptr_t e, unsigned int cat)
{
    if (!e || e == request->player)
        return 0;
    if (request->required && e != request->required)
        return 0;
    if (*(uintptr_t*)e != shared.base + STEAM_UNIT_VTABLE_RVA)
        return 0;

    uintptr_t vid = *(uintptr_t*)(e + STEAM_ENTITY_VID_OFFSET);
    if (!vid)
        return 0;
    /* Without a required entity, watch mode looks only for the ordinary monster. */
    int searching = request->watching == PUPPET_WATCH && !request->required;
    if (searching && *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET) != MONSTER_VID_MONSTER1)
        return 0;
    if (*(unsigned int*)(vid + STEAM_VID_CLASS_OFFSET) != STEAM_UNIT_CLASS)
        return 0;
    if (*(unsigned int*)(vid + STEAM_VID_CATEGORY_OFFSET) != cat)
        return 0;
    if (*(int*)(e + STEAM_ENTITY_HEALTH_OFFSET) <= 0)
        return 0;

    unsigned int anim = *(unsigned int*)(e + STEAM_ENTITY_ANIM_OFFSET);
    if (request->watching == PUPPET_STATIONARY && request->required)
        return anim < STEAM_ANIMATION_COUNT;
    return anim == STEAM_ANIM_IDLE || anim == STEAM_ANIM_RUN;
}

/* Caller in SEH. True when child is the stock linked part of UNIT e. */
static int linked_part_candidate(uintptr_t e, uintptr_t vid, uintptr_t child, uintptr_t child_vid)
{
    if (!child_vid || child_vid != *(uintptr_t*)(vid + STEAM_VID_LINKED_OFFSET))
        return 0;
    if (*(unsigned int*)(child_vid + STEAM_VID_CLASS_OFFSET) != STEAM_PUPPET_PART_CLASS)
        return 0;
    if (*(uintptr_t*)child != shared.base + STEAM_SPRITE_VTABLE_RVA)
        return 0;
    if (*(uintptr_t*)(child + STEAM_ENTITY_CHILD_OFFSET))
        return 0;
    return *(uintptr_t*)(child + STEAM_ENTITY_PARENT_OFFSET) == e;
}

/* True when another slot already owns entity e. */
static int entity_taken(const UnitPuppet* puppet, uintptr_t e)
{
    for (unsigned int slot = 0; slot < UNIT_PUPPET_CAPACITY; ++slot)
    {
        if (&puppets[slot] != puppet && puppets[slot].entity == e)
            return 1;
    }
    return 0;
}

/* Starts tracking a newly attached entity with fresh counters and form. */
static void reset_tracking(UnitPuppet* puppet, uintptr_t e, uintptr_t vid, enum PuppetMode watching)
{
    puppet->behavior = *(unsigned int*)(e + STEAM_UNIT_BEHAVIOR_OFFSET);
    puppet->state = UNIT_PUPPET_ACTIVE;
    puppet->combat = 0;
    puppet->death.dying = 0;
    puppet->predictive = 0;
    puppet->predicted_hits = 0;
    puppet->form.changing_vid = 0;
    puppet->last_event = 0;
    puppet->stats.replayed = 0;
    puppet->stats.duplicates = 0;
    puppet->stats.attack_frame_changes = 0;
    puppet->watching = watching;
    puppet->captured_attacks = 0;
    puppet->death.captured = 0;
    puppet->form.current_vid = *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET);
    puppet->form.revision = 1u;
    puppet->form.captured = 0;
    puppet->form.transitioning = 0;
    puppet->death.animation = 0;
    memset(&puppet->death.pose, 0, sizeof(puppet->death.pose));
}

/* Caller in SEH. Clears the native command, stack and behavior of a
   controlled or stationary entity and records its animation frames. */
static void take_control(UnitPuppet* puppet, uintptr_t e, uintptr_t child, unsigned int anim)
{
    /* Original actions preserve goal reference counting and the
       LIST<ACT> ABI; no raw goal=NULL or list pointer tricks. */
    shared.api.action((void*)e, NULL, STEAM_ACT_CLEAR_COMMAND, 0, 0, 0);
    shared.api.action((void*)e, NULL, STEAM_ACT_CLEAR_STACK, 0, 0, 0);
    if (child)
    {
        shared.api.part_action((void*)child, NULL, STEAM_ACT_CLEAR_COMMAND, 0, 0, 0);
        shared.api.part_action((void*)child, NULL, STEAM_ACT_CLEAR_STACK, 0, 0, 0);
        *(unsigned int*)(child + STEAM_ENTITY_COMMAND_TIMER_OFFSET) = 0;
    }
    shared.api.action((void*)e, NULL, STEAM_ACT_STOP, 1, 0, 0);
    shared.api.action((void*)e, NULL, STEAM_ACT_SET_BEHAVIOR, 0, 0, 0);
    *(unsigned int*)(e + STEAM_ENTITY_COMMAND_TIMER_OFFSET) = 0;
    *(int*)(e + STEAM_UNIT_AVOIDANCE_OFFSET) = 0;
    *(float*)(e + STEAM_ENTITY_Z_VELOCITY_OFFSET) = 0;
    memset(&puppet->pose, 0, sizeof(puppet->pose));
    if (anim == STEAM_ANIM_IDLE || anim == STEAM_ANIM_RUN)
        select_animation(puppet, (void*)e);
    puppet->frames.last_frame = *(unsigned int*)(e + STEAM_ENTITY_FRAME_CURRENT_OFFSET);
    puppet->frames.last_animation = *(unsigned int*)(e + STEAM_ENTITY_ANIM_OFFSET);
    puppet->frames.last_first = *(unsigned int*)(e + STEAM_ENTITY_FRAME_FIRST_OFFSET);
    if (child)
    {
        puppet->frames.child_last_frame =
            *(unsigned int*)(child + STEAM_ENTITY_FRAME_CURRENT_OFFSET);
        puppet->frames.child_last_first = *(unsigned int*)(child + STEAM_ENTITY_FRAME_FIRST_OFFSET);
        puppet->frames.child_last_animation = *(unsigned int*)(child + STEAM_ENTITY_ANIM_OFFSET);
    }
}

static int is_death_animation(unsigned int animation)
{
    return animation == STEAM_ANIM_DEATH || animation == STEAM_ANIM_GIB_DEATH;
}

/* Caller in SEH. Attaches the slot to e when it qualifies. Returns 1 attached. */
static int try_attach_entity(UnitPuppet* puppet, const AttachRequest* request, uintptr_t e,
                             unsigned int cat)
{
    if (!unit_candidate(request, e, cat))
        return 0;
    uintptr_t vid = *(uintptr_t*)(e + STEAM_ENTITY_VID_OFFSET);
    unsigned int anim = *(unsigned int*)(e + STEAM_ENTITY_ANIM_OFFSET);
    uintptr_t child = *(uintptr_t*)(e + STEAM_ENTITY_CHILD_OFFSET);
    uintptr_t child_vid = 0;
    if (child)
    {
        child_vid = *(uintptr_t*)(child + STEAM_ENTITY_VID_OFFSET);
        if (!linked_part_candidate(e, vid, child, child_vid))
            return 0;
    }
    if (entity_taken(puppet, e))
        return 0;

    puppet->game = request->game;
    puppet->entity = e;
    puppet->vid = vid;
    puppet->category = cat;
    puppet->child = child;
    puppet->child_vid = child_vid;
    if (!child_registered(puppet))
    {
        puppet->child = 0;
        puppet->entity = 0;
        return 0;
    }
    reset_tracking(puppet, e, vid, request->watching);
    if (request->watching == PUPPET_WATCH)
        return 1;

    /* A private armor successor may already be hit or killed before
       the next coordinator tick. Adopt that native state without
       rejecting it or restarting its death animation. */
    if (request->watching == PUPPET_STATIONARY && is_death_animation(anim))
    {
        puppet->form.transitioning =
            anim == STEAM_ANIM_DEATH && mp_monster_armored(puppet->form.current_vid);
        if (!puppet->form.transitioning)
            capture_death(puppet, e);
        return 1;
    }
    take_control(puppet, e, child, anim);
    return 1;
}

/* Scans the tick entity lists for the first entity that qualifies and attaches the slot.
   Only a READY slot may attach. */
static enum UnitResult attach(UnitPuppet* puppet, uintptr_t game, uintptr_t player,
                              uintptr_t required, enum PuppetMode watching)
{
    if (!puppet || puppet->state != UNIT_PUPPET_READY)
        return UNIT_REFUSED;
    __try
    {
        if (!game || *(uintptr_t*)game != shared.base + STEAM_GAME_VTABLE_RVA)
            return UNIT_REFUSED;
        for (unsigned int cat = 0; cat < STEAM_TICK_ENTITY_LIST_COUNT; ++cat)
        {
            uintptr_t* entries;
            unsigned int count;
            if (!world_list_read(game, cat, &entries, &count))
                return UNIT_REFUSED;
            for (unsigned int i = 0; i < count; ++i)
            {
                AttachRequest request = {game, player, required, watching};
                if (try_attach_entity(puppet, &request, entries[i], cat))
                    return UNIT_OK;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        puppet->state = UNIT_PUPPET_FAULT;
        return UNIT_FAULT;
    }
    return UNIT_REFUSED;
}

/* Attaches to the first suitable monster (controlled mode). */
enum UnitResult unit_puppet_attach_first(UnitPuppet* puppet, uintptr_t game, uintptr_t player)
{
    return attach(puppet, game, player, 0, PUPPET_CONTROLLED);
}

/* Attaches in watch mode: native behavior stays, attacks and deaths are captured. */
enum UnitResult unit_puppet_watch_first(UnitPuppet* puppet, uintptr_t game, uintptr_t player)
{
    return attach(puppet, game, player, 0, PUPPET_WATCH);
}

/* Controlled attach to a specific entity. */
enum UnitResult unit_puppet_attach_entity(UnitPuppet* puppet, uintptr_t game, uintptr_t player,
                                          uintptr_t entity)
{
    return entity ? attach(puppet, game, player, entity, PUPPET_CONTROLLED) : UNIT_REFUSED;
}

/* Watch-mode attach to a specific entity. */
enum UnitResult unit_puppet_watch_entity(UnitPuppet* puppet, uintptr_t game, uintptr_t player,
                                         uintptr_t entity)
{
    return entity ? attach(puppet, game, player, entity, PUPPET_WATCH) : UNIT_REFUSED;
}

/* Watch mode with AI and movement frozen; damage and death stay native. */
enum UnitResult unit_puppet_watch_stationary(UnitPuppet* puppet, uintptr_t game, uintptr_t player,
                                             uintptr_t entity)
{
    return entity ? attach(puppet, game, player, entity, PUPPET_STATIONARY) : UNIT_REFUSED;
}

/* True when the slot watches a live native monster. */
static int watching_alive(const UnitPuppet* puppet)
{
    if (!puppet || !puppet->watching)
        return 0;
    return puppet->state == UNIT_PUPPET_ACTIVE && !puppet->death.dying;
}

/* True when the slot controls a live private copy (not watch mode). */
static int controlling_alive(const UnitPuppet* puppet)
{
    if (!puppet || puppet->watching)
        return 0;
    return puppet->state == UNIT_PUPPET_ACTIVE && !puppet->death.dying;
}

/* Caller in SEH. True when the root and part are registered and both descriptors are
   sanitized private copies, so native actions cannot run stock scripts. */
static int private_copy_intact(UnitPuppet* puppet, uintptr_t game)
{
    if (!registered(puppet, game) || !child_registered(puppet))
        return 0;
    if (!sanitized(puppet->vid))
        return 0;
    if (puppet->child && !sanitized(puppet->child_vid))
        return 0;
    return 1;
}

/* Diagnostic: kills the watched monster through native SET_HP. */
enum UnitResult unit_puppet_host_kill(UnitPuppet* puppet, uintptr_t game)
{
    return unit_puppet_host_kill_variant(puppet, game, STEAM_ANIM_DEATH);
}

/* Diagnostic: plays the frozen form stage through native CHANGE_VID. */
enum UnitResult unit_puppet_host_freeze(UnitPuppet* puppet, uintptr_t game)
{
    if (!watching_alive(puppet) || !mp_monster_origin(puppet->form.current_vid))
        return UNIT_REFUSED;
    __try
    {
        if (!registered(puppet, game))
            return UNIT_REFUSED;
        unsigned int frozen = puppet->form.current_vid + MONSTER_VID_FROZEN_DELTA;
        on_action((void*)puppet->entity, NULL, STEAM_ACT_CHANGE_VID, frozen, STEAM_ANIM_IDLE, 0);
        if (puppet->form.current_vid != frozen || !registered(puppet, game))
            return UNIT_REFUSED;
        shared.api.animation((void*)puppet->entity, NULL, STEAM_ANIM_IDLE);
        shared.api.action((void*)puppet->entity, NULL, STEAM_ACT_SET_BEHAVIOR, 0, 0, 0);
        shared.api.action((void*)puppet->entity, NULL, STEAM_ACT_CLEAR_COMMAND, 0, 0, 0);
        shared.api.action((void*)puppet->entity, NULL, STEAM_ACT_CLEAR_STACK, 0, 0, 0);
        shared.api.action((void*)puppet->entity, NULL, STEAM_ACT_STOP, 1, 0, 0);
        return UNIT_OK;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        puppet->state = UNIT_PUPPET_FAULT;
        return UNIT_FAULT;
    }
}

enum UnitResult unit_puppet_host_kill_variant(UnitPuppet* puppet, uintptr_t game,
                                              unsigned int animation)
{
    if (!watching_alive(puppet) || !is_death_animation(animation))
        return UNIT_REFUSED;
    __try
    {
        if (!registered(puppet, game))
            return UNIT_REFUSED;

        int health = animation == STEAM_ANIM_GIB_DEATH ? PUPPET_GIB_HEALTH : 0;
        on_action((void*)puppet->entity, NULL, STEAM_ACT_SET_HP, health, 0, 0);
        (void)unit_puppet_sample(puppet, game);
        return UNIT_OK;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        puppet->state = UNIT_PUPPET_FAULT;
        return UNIT_FAULT;
    }
}

/* True when every number in the pose is finite and in range. */
static int pose_valid(const UnitPuppetPose* p)
{
    if (!p)
        return 0;
    if (!_finite(p->x) || !_finite(p->y) || !_finite(p->z) || !_finite(p->velocity))
        return 0;
    return p->direction <= STEAM_DIRECTION_MAX && p->moving <= 1u;
}

/* Caller in SEH. Writes velocity and the moving flag; a frozen copy stands still. */
static void apply_motion(UnitPuppet* puppet, const UnitPuppetPose* p, int frozen)
{
    *(float*)(puppet->entity + STEAM_ENTITY_VELOCITY_OFFSET) = frozen ? 0 : p->velocity;

    uint32_t* flags = (uint32_t*)(puppet->entity + STEAM_ENTITY_FLAGS_OFFSET);
    *flags &= ~STEAM_ENTITY_MOVING_FLAG;
    if (p->moving && !frozen)
        *flags |= STEAM_ENTITY_MOVING_FLAG;
}

/* Caller in SEH. True when the root should switch to the run/idle animation for the
   new pose: it is not frozen, not in a predicted pain or a replayed attack, and does
   not already show it. */
static int needs_pose_animation(const UnitPuppet* puppet, const UnitPuppetPose* p,
                                unsigned int previous, int frozen)
{
    if (frozen)
        return 0;
    if (puppet->predictive && (previous == STEAM_ANIM_PAIN || previous == STEAM_ANIM_PAIN_ALT))
        return 0;

    unsigned int current = *(unsigned int*)(puppet->entity + STEAM_ENTITY_ANIM_OFFSET);
    if (puppet->combat && current == STEAM_ANIM_ATTACK)
        return 0;
    unsigned int wanted = p->moving && p->velocity != 0 ? STEAM_ANIM_RUN : STEAM_ANIM_IDLE;
    return current != wanted;
}

/* Writes a replicated pose into a controlled entity after validating the numbers.
   A vanished entity marks the slot LOST. */
enum UnitResult unit_puppet_apply(UnitPuppet* puppet, uintptr_t game, const UnitPuppetPose* p)
{
    if (!controlling_alive(puppet) || !pose_valid(p))
        return UNIT_REFUSED;
    __try
    {
        if (!registered(puppet, game))
        {
            mark_entity_lost(puppet);
            return UNIT_REFUSED;
        }
        if (!child_registered(puppet))
        {
            puppet->state = UNIT_PUPPET_FAULT;
            return UNIT_FAULT;
        }
        /* A predicted death or armor change plays natively; the pose must not move it. */
        unsigned int animation = *(unsigned int*)(puppet->entity + STEAM_ENTITY_ANIM_OFFSET);
        if (puppet->predictive && (is_death_animation(animation) || puppet->form.transitioning))
            return UNIT_OK;

        puppet->pose = *p;
        shared.api.move((void*)puppet->entity, NULL, p->x, p->y, p->z);
        shared.api.rotate((void*)puppet->entity, NULL, p->direction);

        int frozen = puppet->predictive && mp_monster_frozen(puppet->form.current_vid);
        apply_motion(puppet, p, frozen);
        if (needs_pose_animation(puppet, p, animation, frozen))
            select_animation(puppet, (void*)puppet->entity);
        return UNIT_OK;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        puppet->state = UNIT_PUPPET_FAULT;
        return UNIT_FAULT;
    }
}

/* True when the descriptor has no scripts, creations, damage or service object, i.e. it
   is a private copy. */
static int sanitized(uintptr_t vid)
{
    if (*(int*)(vid + STEAM_VID_DAMAGE_OFFSET))
        return 0;
    if (*(uintptr_t*)(vid + STEAM_VID_SERVICE_OFFSET))
        return 0;
    if (*(int*)(vid + STEAM_VID_DELETE_SCRIPT_OFFSET) != -1)
        return 0;
    if (*(int*)(vid + STEAM_VID_DAMAGE_SCRIPT_OFFSET) != -1)
        return 0;
    if (*(int*)(vid + STEAM_VID_COLLISION_SCRIPT_OFFSET) != -1)
        return 0;

    /* -1 means "no script" in every per-animation script entry. */
    for (unsigned int i = 0; i < STEAM_ANIMATION_COUNT; ++i)
    {
        if (*(int*)(vid + STEAM_VID_ANIMATION_SCRIPTS_OFFSET + i * sizeof(int)) != -1)
            return 0;
        if (*(uintptr_t*)(vid + STEAM_VID_CREATIONS_OFFSET + i * sizeof(uintptr_t)))
            return 0;
    }
    return 1;
}

/* Switches an attached private copy to local hit prediction. */
enum UnitResult unit_puppet_prediction(UnitPuppet* puppet, uintptr_t game)
{
    if (!controlling_alive(puppet))
        return UNIT_REFUSED;
    __try
    {
        if (!registered(puppet, game) || !prediction_intact(puppet))
            return UNIT_REFUSED;
        puppet->predictive = 1;
        return UNIT_OK;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        puppet->state = UNIT_PUPPET_FAULT;
        return UNIT_FAULT;
    }
}

enum UnitResult unit_puppet_prediction_test_hit(UnitPuppet* puppet, uintptr_t game, int damage,
                                                int freeze)
{
    if (!controlling_alive(puppet) || !puppet->predictive)
        return UNIT_REFUSED;
    if (damage <= 0 || damage > PUPPET_HEALTH_LIMIT)
        return UNIT_REFUSED;
    __try
    {
        if (!registered(puppet, game))
            return UNIT_REFUSED;
        on_action((void*)puppet->entity, NULL, STEAM_ACT_DAMAGE, damage, 0, 0);
        if (puppet->state != UNIT_PUPPET_ACTIVE || !registered(puppet, game))
            return UNIT_REFUSED;
        unsigned int animation = *(unsigned int*)(puppet->entity + STEAM_ENTITY_ANIM_OFFSET);
        if (freeze && is_death_animation(animation))
        {
            if (!mp_monster_origin(puppet->form.current_vid))
                return UNIT_REFUSED;
            unsigned int frozen = puppet->form.current_vid + MONSTER_VID_FROZEN_DELTA;
            on_action((void*)puppet->entity, NULL, STEAM_ACT_CHANGE_VID, frozen, STEAM_ANIM_IDLE,
                      0);
            if (puppet->form.current_vid != frozen || !registered(puppet, game))
                return UNIT_REFUSED;
            on_action((void*)puppet->entity, NULL, STEAM_ACT_SET_ANIMATION, STEAM_ANIM_IDLE, 0, 0);
            on_action((void*)puppet->entity, NULL, STEAM_ACT_SET_BEHAVIOR, 0, 0, 0);
            on_action((void*)puppet->entity, NULL, STEAM_ACT_CLEAR_COMMAND, 0, 0, 0);
            on_action((void*)puppet->entity, NULL, STEAM_ACT_CLEAR_STACK, 0, 0, 0);
            on_action((void*)puppet->entity, NULL, STEAM_ACT_STOP, 1, 0, 0);
        }
        return puppet->state == UNIT_PUPPET_ACTIVE ? UNIT_OK : UNIT_REFUSED;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        puppet->state = UNIT_PUPPET_FAULT;
        return UNIT_FAULT;
    }
}

/* Sets the health of a private copy through native SET_HP; stock descriptors are refused. */
enum UnitResult unit_puppet_health(UnitPuppet* puppet, uintptr_t game, int health)
{
    if (!controlling_alive(puppet))
        return UNIT_REFUSED;
    if (health <= 0 || health > PUPPET_HEALTH_LIMIT)
        return UNIT_REFUSED;
    __try
    {
        if (!private_copy_intact(puppet, game))
            return UNIT_REFUSED;
        shared.api.action((void*)puppet->entity, NULL, STEAM_ACT_SET_HP, health, 0, 0);
        return UNIT_OK;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        puppet->state = UNIT_PUPPET_FAULT;
        return UNIT_FAULT;
    }
}

/* Plays an attack, or kills through SET_HP 0, on a private copy. Old or repeated
   `sequence` values are counted as duplicates and refused. */
enum UnitResult unit_puppet_replay(UnitPuppet* puppet, uintptr_t game, uint32_t sequence,
                                   unsigned int animation)
{
    if (!puppet || puppet->watching || puppet->state != UNIT_PUPPET_ACTIVE)
        return UNIT_REFUSED;
    if (!sequence)
        return UNIT_REFUSED;
    if (animation != STEAM_ANIM_ATTACK && animation != STEAM_ANIM_DEATH)
        return UNIT_REFUSED;
    __try
    {
        if (!private_copy_intact(puppet, game))
            return UNIT_REFUSED;

        /* Sequences wrap: a delta of zero or in the upper half means old or repeated. */
        uint32_t delta = sequence - puppet->last_event;
        int not_newer = !delta || delta >= PUPPET_SEQUENCE_HALF_RANGE;
        if (puppet->last_event && not_newer)
        {
            ++puppet->stats.duplicates;
            return UNIT_REFUSED;
        }
        if (puppet->death.dying)
            return UNIT_REFUSED;

        puppet->combat = 1;
        if (animation == STEAM_ANIM_DEATH)
        {
            puppet->death.dying = 1;
            shared.api.action((void*)puppet->entity, NULL, STEAM_ACT_SET_HP, 0, 0, 0);
        }
        else
        {
            shared.api.animation((void*)puppet->entity, NULL, animation);
            if (puppet->child)
                shared.api.animation((void*)puppet->child, NULL, animation);
        }
        puppet->last_event = sequence;
        ++puppet->stats.replayed;
        return UNIT_OK;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        puppet->state = UNIT_PUPPET_FAULT;
        return UNIT_FAULT;
    }
}

enum UnitResult unit_puppet_test_damage(UnitPuppet* puppet, uintptr_t game)
{
    if (!puppet || puppet->state != UNIT_PUPPET_ACTIVE)
        return UNIT_REFUSED;
    __try
    {
        if (!registered(puppet, game) || !child_registered(puppet))
            return UNIT_REFUSED;
        int hp = *(int*)(puppet->entity + STEAM_ENTITY_HEALTH_OFFSET);
        on_action((void*)puppet->entity, NULL, STEAM_ACT_DAMAGE, PUPPET_HEALTH_LIMIT, 0, 0);
        on_action((void*)puppet->entity, NULL, STEAM_ACT_SET_HP, 0, 0, 0);
        if (*(int*)(puppet->entity + STEAM_ENTITY_HEALTH_OFFSET) != hp)
            return UNIT_REFUSED;
        if (puppet->child)
        {
            hp = *(int*)(puppet->child + STEAM_ENTITY_HEALTH_OFFSET);
            on_part_action((void*)puppet->child, NULL, STEAM_ACT_DAMAGE, PUPPET_HEALTH_LIMIT, 0, 0);
            on_part_action((void*)puppet->child, NULL, STEAM_ACT_SET_HP, 0, 0, 0);
            if (*(int*)(puppet->child + STEAM_ENTITY_HEALTH_OFFSET) != hp)
                return UNIT_REFUSED;
        }
        return UNIT_OK;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        puppet->state = UNIT_PUPPET_FAULT;
        return UNIT_FAULT;
    }
}

/* Caller in SEH, root registered. Notices an armor transition or a death in
   the root's animation and counts its native frame progression. */
static void sample_root_animation(UnitPuppet* puppet, uintptr_t e, UnitPuppetSample* s)
{
    int observed = puppet->watching || puppet->predictive;
    if (observed && mp_monster_armored(puppet->form.current_vid) &&
        s->animation == STEAM_ANIM_DEATH)
        puppet->form.transitioning = 1;
    if (observed && !puppet->death.dying && !puppet->form.transitioning &&
        is_death_animation(s->animation))
        capture_death(puppet, e);

    s->frame = *(unsigned int*)(e + STEAM_ENTITY_FRAME_CURRENT_OFFSET);
    s->frame_first = *(unsigned int*)(e + STEAM_ENTITY_FRAME_FIRST_OFFSET);
    s->frame_last = *(unsigned int*)(e + STEAM_ENTITY_FRAME_LAST_OFFSET);
    /* A direction change selects another range; that alone is not
       evidence of native frame progression. Single-frame art is valid. */
    int same_range = s->animation == puppet->frames.last_animation &&
                     s->frame_first == puppet->frames.last_first;
    if (same_range && s->frame != puppet->frames.last_frame)
    {
        ++puppet->stats.frame_changes;
        if (s->animation == STEAM_ANIM_ATTACK)
            ++puppet->stats.attack_frame_changes;
    }
    puppet->frames.last_first = s->frame_first;
    puppet->frames.last_frame = s->frame;
    puppet->frames.last_animation = s->animation;
}

/* Caller in SEH, root registered. Reads the root's form, health, animation,
   command state and pose into s. */
static void sample_root(UnitPuppet* puppet, uintptr_t e, UnitPuppetSample* s)
{
    s->vid = *(unsigned int*)(puppet->vid + STEAM_VID_INDEX_OFFSET);
    s->class_id = STEAM_UNIT_CLASS;
    s->health = *(int*)(e + STEAM_ENTITY_HEALTH_OFFSET);
    s->animation = *(unsigned int*)(e + STEAM_ENTITY_ANIM_OFFSET);
    sample_root_animation(puppet, e, s);
    s->behavior = *(unsigned int*)(e + STEAM_UNIT_BEHAVIOR_OFFSET);
    s->goal = *(uintptr_t*)(e + STEAM_ENTITY_GOAL_OFFSET);
    s->command = *(unsigned int*)(e + STEAM_ENTITY_FLAGS_OFFSET) & STEAM_ENTITY_COMMAND_MASK;
    s->stack = *(unsigned int*)(e + STEAM_ENTITY_STACK_COUNT_OFFSET);
    s->moving = !!(*(unsigned int*)(e + STEAM_ENTITY_FLAGS_OFFSET) & STEAM_ENTITY_MOVING_FLAG);
    s->direction = *(unsigned char*)(e + STEAM_ENTITY_DIRECTION_OFFSET);
    s->velocity = *(float*)(e + STEAM_ENTITY_VELOCITY_OFFSET);
    s->x = *(float*)(e + STEAM_ENTITY_X_OFFSET);
    s->y = *(float*)(e + STEAM_ENTITY_Y_OFFSET);
    s->z = *(float*)(e + STEAM_ENTITY_Z_OFFSET);
}

/* Caller in SEH, part registered. Reads the part's command state and counts
   its native frame progression. */
static void sample_part(UnitPuppet* puppet, UnitPuppetSample* s)
{
    uintptr_t child = puppet->child;
    s->child = child;
    s->child_goal = *(uintptr_t*)(child + STEAM_ENTITY_GOAL_OFFSET);
    s->child_command =
        *(unsigned int*)(child + STEAM_ENTITY_FLAGS_OFFSET) & STEAM_ENTITY_COMMAND_MASK;
    s->child_stack = *(unsigned int*)(child + STEAM_ENTITY_STACK_COUNT_OFFSET);
    s->child_frame_first = *(unsigned int*)(child + STEAM_ENTITY_FRAME_FIRST_OFFSET);
    s->child_frame = *(unsigned int*)(child + STEAM_ENTITY_FRAME_CURRENT_OFFSET);
    s->child_frame_last = *(unsigned int*)(child + STEAM_ENTITY_FRAME_LAST_OFFSET);
    unsigned int child_animation = *(unsigned int*)(child + STEAM_ENTITY_ANIM_OFFSET);
    s->child_animation = child_animation;
    int same_range = child_animation == puppet->frames.child_last_animation &&
                     s->child_frame_first == puppet->frames.child_last_first;
    if (same_range && s->child_frame != puppet->frames.child_last_frame)
    {
        ++puppet->stats.child_frame_changes;
        if (child_animation == STEAM_ANIM_ATTACK)
            ++puppet->stats.attack_frame_changes;
    }
    puppet->frames.child_last_animation = child_animation;
    puppet->frames.child_last_frame = s->child_frame;
    puppet->frames.child_last_first = s->child_frame_first;
}

/* Copies the slot's form, death and counter state into s after sampling. */
static void copy_tracked_state(const UnitPuppet* puppet, UnitPuppetSample* s)
{
    s->frame_changes = puppet->stats.frame_changes;
    s->child_frame_changes = puppet->stats.child_frame_changes;
    s->vid = puppet->form.current_vid;
    s->form_revision = puppet->form.revision;
    s->captured_forms = puppet->form.captured;
    s->transitioning = puppet->form.transitioning;
    s->captured_deaths = puppet->death.captured;
    s->death_animation = puppet->death.animation;
    s->death_pose = puppet->death.pose;
    s->predicted_hits = puppet->predicted_hits;
    s->predictive = puppet->predictive;
    s->state = puppet->state;
    s->attack_frame_changes = puppet->stats.attack_frame_changes;
}

/* Reads the slot's state for the tick. Observes the form only after finding the entity
   in the current world list. */
UnitPuppetSample unit_puppet_sample(UnitPuppet* puppet, uintptr_t game)
{
    UnitPuppetSample s = {0};
    if (!puppet)
        return s;
    s.state = puppet->state;
    s.entity = puppet->entity;
    s.decisions = puppet->stats.decisions;
    s.movements = puppet->stats.movements;
    s.blocked = puppet->stats.blocked;
    s.replayed = puppet->stats.replayed;
    s.duplicates = puppet->stats.duplicates;
    s.captured_attacks = puppet->captured_attacks;
    __try
    {
        if (puppet->entity && (puppet->watching || puppet->predictive))
        {
            /* Validate membership before observing a potentially changed VID. */
            if (game == puppet->game && world_list_find(game, puppet->category, puppet->entity))
                observe_form(puppet, puppet->entity, 0, NULL);
        }
        if (puppet->entity && !registered(puppet, game))
        {
            mark_entity_lost(puppet);
            s.state = puppet->state;
            s.entity = 0;
        }
        if (s.entity)
        {
            sample_root(puppet, s.entity, &s);
            if (!child_registered(puppet))
            {
                puppet->state = UNIT_PUPPET_FAULT;
                s.state = UNIT_PUPPET_FAULT;
            }
            else if (puppet->child)
                sample_part(puppet, &s);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        puppet->state = UNIT_PUPPET_FAULT;
        s.state = UNIT_PUPPET_FAULT;
    }
    copy_tracked_state(puppet, &s);
    return s;
}

/* Detaches the slot and, when safe, restores the native behavior and idle pose.
   The native entity is never deleted here. */
enum UnitResult unit_puppet_release(UnitPuppet* puppet, uintptr_t game)
{
    if (!puppet)
        return UNIT_REFUSED;
    __try
    {
        /* Only a controlled or stationary copy that is still alive gets its AI back. */
        int restorable = !puppet->death.dying && !puppet->form.transitioning &&
                         !puppet->predictive && puppet->watching != PUPPET_WATCH;
        if (puppet->entity && registered(puppet, game) && restorable)
        {
            shared.api.action((void*)puppet->entity, NULL, STEAM_ACT_STOP, 1, 0, 0);
            shared.api.action((void*)puppet->entity, NULL, STEAM_ACT_SET_BEHAVIOR, puppet->behavior,
                              0, 0);
            shared.api.animation((void*)puppet->entity, NULL, STEAM_ANIM_IDLE);
        }
        puppet->entity = 0;
        puppet->child = 0;
        if (puppet->state != UNIT_PUPPET_OFF)
            puppet->state = UNIT_PUPPET_READY;
        return UNIT_OK;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        puppet->state = UNIT_PUPPET_FAULT;
        return UNIT_REFUSED;
    }
}

/* Unhooks only when no slot holds an entity. Returns UNIT_REFUSED otherwise. */
enum UnitResult unit_puppet_stop(void)
{
    for (unsigned int i = 0; i < UNIT_PUPPET_CAPACITY; ++i)
        if (puppets[i].entity)
            return UNIT_REFUSED;
    int a = slot_stop(&shared.action) == SLOT_OK;
    int m = slot_stop(&shared.movement) == SLOT_OK;
    int d = slot_stop(&shared.destroy) == SLOT_OK;
    int pa = slot_stop(&shared.part_action) == SLOT_OK;
    int pd = slot_stop(&shared.part_destroy) == SLOT_OK;
    if (a && m && d && pa && pd)
        for (unsigned int i = 0; i < UNIT_PUPPET_CAPACITY; ++i)
            puppets[i].state = UNIT_PUPPET_OFF;
    /* Keep original call targets: a foreign hook or already fetched callback
       may still chain to us after stop. */
    return a && m && d && pa && pd;
}
