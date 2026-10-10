#include <string.h>
#include <math.h>
#include <float.h>
#include "unit_puppet.h"
#include "slot_hook.h"

enum PuppetMode { PUPPET_CONTROLLED, PUPPET_WATCH, PUPPET_STATIONARY };
typedef struct PuppetSlot {
    uintptr_t game, entity, vid, child, child_vid;
    unsigned int category, behavior, decisions, movements, blocked, frame_changes, last_frame, last_animation;
    unsigned int last_first, child_last_frame, child_last_first, child_last_animation, child_frame_changes;
    enum UnitPuppetState state;
    UnitPuppetPose pose;
    uint32_t last_event;
    unsigned int replayed, duplicates, attack_frame_changes;
    int combat, dying;
    enum PuppetMode watching;
    unsigned int captured_attacks, captured_deaths;
    unsigned int death_animation;
    UnitPuppetPose death_pose;
    unsigned int current_vid, form_revision, captured_forms;
    int transitioning;
    unsigned int predicted_hits;
    int predictive;
    uintptr_t changing_vid;
} PuppetSlot;
static PuppetSlot puppets[UNIT_PUPPET_CAPACITY];
static unsigned int puppet_slot;
#define puppet (puppets[puppet_slot])
static struct {
    uintptr_t base;
    UnitPuppetEngine api;
    SlotHook action, movement, destroy, part_action, part_destroy;
    UnitFormObserver forms;
} shared;
void unit_puppet_set_form_observer(UnitFormObserver observer) { shared.forms = observer; }
int unit_puppet_select(unsigned int slot) {
    if (slot >= UNIT_PUPPET_CAPACITY) return 0;
    puppet_slot = slot; return 1;
}
static unsigned int route(void* entity, int part) {
    for (unsigned int i = 0; i < UNIT_PUPPET_CAPACITY; ++i)
        if ((part ? puppets[i].child : puppets[i].entity) == (uintptr_t)entity &&
            (puppets[i].state == UNIT_PUPPET_ACTIVE || puppets[i].state == UNIT_PUPPET_FAULT)) return i;
    return puppet_slot;
}

int unit_puppet_bind(uintptr_t base, UnitPuppetEngine* out) {
    static const struct { unsigned int rva; unsigned char bytes[8]; } signatures[] = {
        {STEAM_UNIT_DESTROY_RVA, {0x55,0x8B,0xEC,0x6A,0xFF,0x68,0x90,0x7C}},
        {STEAM_UNIT_ACTION_RVA, {0x55,0x8B,0xEC,0x6A,0xFF,0x68,0xC0,0x7C}},
        {STEAM_UNIT_UPDATE_RVA, {0x55,0x8B,0xEC,0x83,0xEC,0x10,0x56,0x57}},
        {STEAM_UNIT_MOVEMENT_RVA, {0x55,0x8B,0xEC,0x83,0xEC,0x10,0x53,0x56}},
        {STEAM_SPRITE_DESTROY_RVA, {0x55,0x8B,0xEC,0x56,0x8B,0xF1,0xE8,0x55}},
        {STEAM_SPRITE_ACTION_RVA, {0x55,0x8B,0xEC,0x6A,0xFF,0x68,0xCF,0x7A}},
        {STEAM_SET_ANIMATION_RVA, {0x55,0x8B,0xEC,0x51,0x53,0x56,0x57,0x8B}},
        {STEAM_ACTOR_MOVE_RVA, {0x55,0x8B,0xEC,0xF3,0x0F,0x10,0x4D,0x08}},
        {STEAM_ACTOR_ROTATE_RVA, {0x55,0x8B,0xEC,0x83,0xEC,0x10,0x8A,0x45}}
    };
    memset(out, 0, sizeof(*out));
    __try {
        for (unsigned int i = 0; i < sizeof(signatures)/sizeof(signatures[0]); ++i)
            if (memcmp((void*)(base + signatures[i].rva), signatures[i].bytes, 8)) return 0;
        const unsigned int rvas[] = {STEAM_UNIT_DESTROY_RVA, STEAM_UNIT_ACTION_RVA,
            STEAM_UNIT_UPDATE_RVA, STEAM_UNIT_MOVEMENT_RVA};
        for (unsigned int i = 0; i < 4; ++i)
            if (*(uintptr_t*)(base + STEAM_UNIT_VTABLE_RVA + i * sizeof(uintptr_t)) != base + rvas[i]) return 0;
        if (*(uintptr_t*)(base + STEAM_SPRITE_VTABLE_RVA) != base + STEAM_SPRITE_DESTROY_RVA ||
            *(uintptr_t*)(base + STEAM_SPRITE_VTABLE_RVA + 4u) != base + STEAM_SPRITE_ACTION_RVA ||
            *(uintptr_t*)(base + STEAM_SPRITE_VTABLE_RVA + 8u) != base + STEAM_UNIT_UPDATE_RVA) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    out->action = (ActorAction)(base + STEAM_UNIT_ACTION_RVA);
    out->destroy = (ActorDestroy)(base + STEAM_UNIT_DESTROY_RVA);
    out->movement = (UnitMovement)(base + STEAM_UNIT_MOVEMENT_RVA);
    out->animation = (UnitAnimation)(base + STEAM_SET_ANIMATION_RVA);
    out->move = (ActorMove)(base + STEAM_ACTOR_MOVE_RVA);
    out->rotate = (ActorRotate)(base + STEAM_ACTOR_ROTATE_RVA);
    out->part_action = (ActorAction)(base + STEAM_SPRITE_ACTION_RVA);
    out->part_destroy = (ActorDestroy)(base + STEAM_SPRITE_DESTROY_RVA);
    return 1;
}
/* Caller is inside SEH. Never dereference a cached entity before finding it
   in the current map list. Destructor hook invalidates before native deletion,
   including same-address/same-VID allocation reuse in the same world. */
static int registered(uintptr_t game) {
    if (!puppet.entity || !game || game != puppet.game ||
        *(uintptr_t*)game != shared.base + STEAM_GAME_VTABLE_RVA) return 0;
    uintptr_t list = game + STEAM_WORLD_LIST_OFFSET + puppet.category * STEAM_LIST_SIZE;
    unsigned int count = *(unsigned int*)(list + 4), cap = *(unsigned int*)(list + 8);
    uintptr_t* items = *(uintptr_t**)(list + 12);
    if (!items || count > cap || count > 65536u) return 0;
    for (unsigned int i = 0; i < count; ++i)
        if (items[i] == puppet.entity)
            return *(uintptr_t*)puppet.entity == shared.base + STEAM_UNIT_VTABLE_RVA &&
                *(uintptr_t*)(puppet.entity + STEAM_ENTITY_VID_OFFSET) == puppet.vid;
    return 0;
}
static void lost(void) { puppet.entity = puppet.child = 0; puppet.state = UNIT_PUPPET_LOST; }
static int selected(void* e) { return puppet.entity == (uintptr_t)e &&
    (puppet.state == UNIT_PUPPET_ACTIVE || puppet.state == UNIT_PUPPET_FAULT); }
static int selected_part(void* e) { return puppet.child && puppet.child == (uintptr_t)e &&
    (puppet.state == UNIT_PUPPET_ACTIVE || puppet.state == UNIT_PUPPET_FAULT); }
/* Child is dereferenced only after finding it in its current category list. */
static int child_registered(void) {
    if (!puppet.child) return 1;
    if (!registered(puppet.game) || *(uintptr_t*)(puppet.entity + STEAM_ENTITY_CHILD_OFFSET) != puppet.child) return 0;
    unsigned int category = *(unsigned int*)(puppet.child_vid + STEAM_VID_CATEGORY_OFFSET);
    if (category >= STEAM_TICK_ENTITY_LIST_COUNT) return 0;
    uintptr_t list = puppet.game + STEAM_WORLD_LIST_OFFSET + category * STEAM_LIST_SIZE;
    unsigned int count = *(unsigned int*)(list + 4), cap = *(unsigned int*)(list + 8);
    uintptr_t* items = *(uintptr_t**)(list + 12);
    if (!items || count > cap || count > 65536u) return 0;
    for (unsigned int i = 0; i < count; ++i)
        if (items[i] == puppet.child)
            return *(uintptr_t*)puppet.child == shared.base + STEAM_SPRITE_VTABLE_RVA &&
                *(uintptr_t*)(puppet.child + STEAM_ENTITY_VID_OFFSET) == puppet.child_vid &&
                !*(uintptr_t*)(puppet.child + STEAM_ENTITY_CHILD_OFFSET) &&
                *(uintptr_t*)(puppet.child + STEAM_ENTITY_PARENT_OFFSET) == puppet.entity;
    return 0;
}
static void select_animation(void* entity) {
    unsigned int animation = puppet.pose.moving && puppet.pose.velocity != 0 ? STEAM_ANIM_RUN : STEAM_ANIM_IDLE;
    shared.api.animation(entity, NULL, animation);
}
static void capture_death(uintptr_t e) {
    puppet.dying = 1; ++puppet.captured_deaths;
    unsigned int a = *(unsigned int*)(e + STEAM_ENTITY_ANIM_OFFSET);
    puppet.death_animation = a == STEAM_ANIM_GIB_DEATH ? a : STEAM_ANIM_DEATH;
    puppet.death_pose.x = *(float*)(e + STEAM_ENTITY_X_OFFSET);
    puppet.death_pose.y = *(float*)(e + STEAM_ENTITY_Y_OFFSET);
    puppet.death_pose.z = *(float*)(e + STEAM_ENTITY_Z_OFFSET);
    puppet.death_pose.direction = *(unsigned char*)(e + STEAM_ENTITY_DIRECTION_OFFSET);
}
/* Called only while the entity is live in a native action or after locating it
   in its category list. Native CHANGE_VID performs child teardown and counters;
   observe the resulting descriptor rather than editing those structures. */
static int observe_form(uintptr_t entity, unsigned int requested, uintptr_t* prepared) {
    uintptr_t vid = 0; unsigned int index = puppet.current_vid, revision = puppet.form_revision;
    int transitioning = puppet.transitioning, result = 0;
    if (shared.forms) result = shared.forms(puppet.game, entity, &vid, &index, &revision, &transitioning, requested);
    if (result < 0) { puppet.state = UNIT_PUPPET_FAULT; return -1; }
    if (requested) { if (prepared) *prepared = result > 0 ? vid : 0; return result; }
    uintptr_t actual = *(uintptr_t*)(entity + STEAM_ENTITY_VID_OFFSET);
    if (!result && actual != puppet.vid && puppet.watching) {
        unsigned int frozen = puppet.current_vid / 10u * 10u + 9u;
        if (puppet.current_vid < 40u || puppet.current_vid > 90u || puppet.current_vid % 10u ||
            frozen >= 1024u || actual != *(uintptr_t*)(puppet.game + STEAM_GAME_VID_TABLE_OFFSET + frozen * 4u) ||
            !actual || *(unsigned int*)(actual + STEAM_VID_INDEX_OFFSET) != frozen ||
            *(unsigned int*)(actual + STEAM_VID_CLASS_OFFSET) != STEAM_UNIT_CLASS) return 0;
        vid = actual; index = frozen; revision = puppet.form_revision + 1u; result = 1;
    }
    if (result > 0) {
        puppet.transitioning = transitioning;
        if (vid && actual == vid && vid != puppet.vid) {
            unsigned int category = *(unsigned int*)(vid + STEAM_VID_CATEGORY_OFFSET);
            if (category >= STEAM_TICK_ENTITY_LIST_COUNT) { puppet.state = UNIT_PUPPET_FAULT; return -1; }
            puppet.vid = vid; puppet.current_vid = index;
            puppet.category = category;
            puppet.form_revision = revision; ++puppet.captured_forms;
            puppet.dying = 0; puppet.behavior = *(unsigned int*)(entity + STEAM_UNIT_BEHAVIOR_OFFSET);
            puppet.child = *(uintptr_t*)(entity + STEAM_ENTITY_CHILD_OFFSET);
            puppet.child_vid = puppet.child ? *(uintptr_t*)(puppet.child + STEAM_ENTITY_VID_OFFSET) : 0;
        }
    }
    return result;
}
static int sanitized(uintptr_t vid);
static int prediction_descriptor(uintptr_t vid, int root) {
    if (!vid || !sanitized(vid) || *(int*)(vid + STEAM_VID_CREATE_SCRIPT_OFFSET) != -1) return 0;
    unsigned int index = *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET);
    if (index >= 1024u || (vid == *(uintptr_t*)(puppet.game + STEAM_GAME_VID_TABLE_OFFSET + index * 4u) &&
        vid != puppet.changing_vid)) return 0;
    if (root) {
        uintptr_t behavior = *(uintptr_t*)(vid + STEAM_VID_BEHAVIOR_OFFSET);
        if (!behavior || *(unsigned int*)(behavior + STEAM_BEHAVIOR_UNIT_FLAGS_OFFSET)) return 0;
    }
    return 1;
}
static int observed_action(void* e, unsigned int kind, intptr_t a, intptr_t b, intptr_t c, ActorAction original) {
    int result = 0;
    /* CHANGE_VID takes a table index, not a VID pointer. A scoped substitution
       lets the native path perform linked teardown, health and VID counters on
       a sanitized private form. Restore even when the constructor raises SEH. */
    uintptr_t prepared = 0;
    if ((uintptr_t)e == puppet.entity && kind == STEAM_ACT_CHANGE_VID) {
        if (puppet.predictive && (puppet.current_vid < 40u || puppet.current_vid > 90u ||
            puppet.current_vid % 10u || a != (intptr_t)(puppet.current_vid + 9u))) return 0;
        if (a > 0 && a < 1024 && observe_form((uintptr_t)e, (unsigned int)a, &prepared) < 0) return 0;
        /* Prediction must never escape through an unsupported stock descriptor. */
        if (puppet.predictive && (!prepared || !prediction_descriptor(prepared, 1))) return 0;
    }
    if (puppet.predictive && (uintptr_t)e == puppet.entity && kind == STEAM_ACT_DAMAGE) ++puppet.predicted_hits;
    if (prepared) {
        uintptr_t* entry = (uintptr_t*)(puppet.game + STEAM_GAME_VID_TABLE_OFFSET + (unsigned int)a * 4u);
        uintptr_t saved = *entry;
        uintptr_t saved_changing = puppet.changing_vid;
        __try { *entry = prepared; puppet.changing_vid = prepared; result = original(e, NULL, kind, a, b, c); }
        __finally { *entry = saved; puppet.changing_vid = saved_changing; }
    } else result = original(e, NULL, kind, a, b, c);
    /* Verified native ExecuteCommand(46A8A0) calls USE_AMMO then animation8.
       It also does this for zero-cost melee; no pose/frame inference is used. */
    if (kind == STEAM_ACT_USE_AMMO && a <= 0 && !puppet.dying) ++puppet.captured_attacks;
    __try {
        if (puppet.entity) observe_form(puppet.entity, 0, NULL);
        /* Nested DAMAGE can select death before F145_15 changes to frozen VID.
           Final death is decided at the game-tick sample boundary/destructor. */
    } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; }
    return result;
}
static int prediction_action(void* e, unsigned int kind, intptr_t a, intptr_t b, intptr_t c, ActorAction original, int part) {
    __try {
        if (puppet.state == UNIT_PUPPET_FAULT) return 0;
        /* Native CHANGE_VID can recurse into actions after assigning its new
           descriptor. Adopt only our explicitly in-flight private form. */
        if (!part && puppet.changing_vid && *(uintptr_t*)((uintptr_t)e + STEAM_ENTITY_VID_OFFSET) == puppet.changing_vid)
            observe_form((uintptr_t)e, 0, NULL);
        if (!registered(puppet.game)) { lost(); return 0; }
        if (!child_registered() || !prediction_descriptor(puppet.vid, 1) ||
            (puppet.child && !prediction_descriptor(puppet.child_vid, 0))) { puppet.state = UNIT_PUPPET_FAULT; return 0; }
        if (kind == STEAM_ACT_NEXT_COMMAND) {
            ++puppet.decisions;
            /* Native animation completion may leave pain and resume the
               replicated movement pose, but never reset frozen/death forms. */
            unsigned int anim = *(unsigned int*)(puppet.entity + STEAM_ENTITY_ANIM_OFFSET);
            if (!puppet.dying && !puppet.transitioning && puppet.current_vid % 10u != 9u &&
                (anim == STEAM_ANIM_IDLE || anim == STEAM_ANIM_RUN || anim == STEAM_ANIM_ATTACK || anim == 13u || anim == 14u)) {
                if (part) shared.api.animation(e, NULL, STEAM_ANIM_IDLE); else select_animation(e);
            }
            return 0;
        }
        if (kind == STEAM_ACT_DAMAGE || kind == STEAM_ACT_SET_HP ||
            (!part && kind == STEAM_ACT_CHANGE_VID) ||
            (kind == STEAM_ACT_SET_ANIMATION && a == STEAM_ANIM_IDLE) ||
            (kind == STEAM_ACT_SET_BEHAVIOR && a == 0) || kind == STEAM_ACT_CLEAR_COMMAND ||
            kind == STEAM_ACT_CLEAR_STACK || kind == STEAM_ACT_STOP || kind == STEAM_ACT_GET_HP ||
            kind == 0x5Cu || kind == 0x5Eu || kind == 0x60u)
            return observed_action(e, kind, a, b, c, original);
        ++puppet.blocked; return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; return 0; }
}
static int __fastcall scoped_action(void* e, void* unused, unsigned int kind, intptr_t a, intptr_t b, intptr_t c) {
    (void)unused;
    if (puppet.predictive && selected(e)) return prediction_action(e, kind, a, b, c, shared.api.action, 0);
    if (puppet.watching && selected(e)) {
        if (puppet.watching == PUPPET_STATIONARY && (kind == STEAM_ACT_NEXT_COMMAND || (kind == STEAM_ACT_USE_AMMO && a <= 0))) {
            ++puppet.decisions; return 0;
        }
        return observed_action(e, kind, a, b, c, shared.api.action);
    }
    if (selected(e)) {
        if (puppet.state == UNIT_PUPPET_FAULT && kind != 15u) return 0;
        __try {
            if (!registered(puppet.game)) { lost(); }
            else if (kind == STEAM_ACT_NEXT_COMMAND) {
                ++puppet.decisions;
                if (!puppet.dying) select_animation(e);
                return 0;
            } else if (kind != 15u && kind != 0x57u && kind != 0x5Cu && kind != 0x5Eu && kind != 0x60u) {
                /* This movement-only experiment rejects ALL other actions,
                   including scripts assigning targets/stack, damage and HP.
                   It is not the future combat replay policy. */
                ++puppet.blocked; return 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; return 0; }
    }
    return shared.api.action(e, NULL, kind, a, b, c);
}
static void __fastcall scoped_movement(void* e, void* unused) {
    (void)unused;
    if (puppet.watching == PUPPET_WATCH && selected(e)) { shared.api.movement(e, NULL); return; }
    if (selected(e)) {
        if (puppet.state == UNIT_PUPPET_FAULT) return;
        __try {
            if (registered(puppet.game)) { ++puppet.movements; return; }
            lost();
        } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; return; }
    }
    shared.api.movement(e, NULL);
}
static void* __fastcall scoped_destroy(void* e, void* unused, int release) {
    (void)unused;
    if ((puppet.watching || puppet.predictive) && puppet.entity == (uintptr_t)e && !puppet.dying) {
        __try {
            observe_form((uintptr_t)e, 0, NULL);
            if ((puppet.current_vid == 70u || puppet.current_vid == 90u) &&
                *(unsigned int*)((uintptr_t)e + STEAM_ENTITY_ANIM_OFFSET) == STEAM_ANIM_DEATH) puppet.transitioning = 1;
            if (!puppet.transitioning) capture_death((uintptr_t)e);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; }
    }
    if (puppet.entity == (uintptr_t)e) lost();
    return shared.api.destroy(e, NULL, release);
}
static int __fastcall scoped_part_action(void* e, void* unused, unsigned int kind, intptr_t a, intptr_t b, intptr_t c) {
    (void)unused;
    if (puppet.predictive && selected_part(e)) return prediction_action(e, kind, a, b, c, shared.api.part_action, 1);
    if (puppet.watching && selected_part(e)) {
        if (puppet.watching == PUPPET_STATIONARY && (kind == STEAM_ACT_NEXT_COMMAND || (kind == STEAM_ACT_USE_AMMO && a <= 0))) {
            ++puppet.decisions; return 0;
        }
        return observed_action(e, kind, a, b, c, shared.api.part_action);
    }
    if (selected_part(e)) {
        __try {
            if (!child_registered()) { puppet.state = UNIT_PUPPET_FAULT; return 0; }
            if (kind == STEAM_ACT_NEXT_COMMAND) {
                ++puppet.decisions;
                if (puppet.combat && !puppet.dying) shared.api.animation(e, NULL, STEAM_ANIM_IDLE);
                return 0;
            }
            if (kind != 15u && kind != 0x57u && kind != 0x5Cu && kind != 0x5Eu && kind != 0x60u) {
                ++puppet.blocked; return 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; return 0; }
    }
    return shared.api.part_action(e, NULL, kind, a, b, c);
}
static void* __fastcall scoped_part_destroy(void* e, void* unused, int release) {
    (void)unused;
    if (puppet.child == (uintptr_t)e) {
        puppet.child = 0;
        if (!puppet.dying && !puppet.watching && !puppet.predictive) puppet.state = UNIT_PUPPET_FAULT;
    }
    return shared.api.part_destroy(e, NULL, release);
}
/* Native callbacks may interleave slots, including recursive root/part calls.
   Selection is restored even if the native engine raises an SEH exception. */
static int __fastcall on_action(void* e, void* unused, unsigned int kind, intptr_t a, intptr_t b, intptr_t c) {
    unsigned int saved = puppet_slot; int result = 0;
    puppet_slot = route(e, 0);
    __try { result = scoped_action(e, unused, kind, a, b, c); }
    __finally { puppet_slot = saved; }
    return result;
}
static void __fastcall on_movement(void* e, void* unused) {
    unsigned int saved = puppet_slot; puppet_slot = route(e, 0);
    __try { scoped_movement(e, unused); } __finally { puppet_slot = saved; }
}
static void* __fastcall on_destroy(void* e, void* unused, int release) {
    unsigned int saved = puppet_slot; void* result = NULL; puppet_slot = route(e, 0);
    __try { result = scoped_destroy(e, unused, release); } __finally { puppet_slot = saved; }
    return result;
}
static int __fastcall on_part_action(void* e, void* unused, unsigned int kind, intptr_t a, intptr_t b, intptr_t c) {
    unsigned int saved = puppet_slot; int result = 0; puppet_slot = route(e, 1);
    __try { result = scoped_part_action(e, unused, kind, a, b, c); } __finally { puppet_slot = saved; }
    return result;
}
static void* __fastcall on_part_destroy(void* e, void* unused, int release) {
    unsigned int saved = puppet_slot; void* result = NULL; puppet_slot = route(e, 1);
    __try { result = scoped_part_destroy(e, unused, release); } __finally { puppet_slot = saved; }
    return result;
}
int unit_puppet_install(uintptr_t base, const UnitPuppetEngine* api) {
    if (shared.action.slot || shared.movement.slot || shared.destroy.slot || shared.part_action.slot || shared.part_destroy.slot || !base || !api ||
        !api->action || !api->destroy || !api->movement || !api->animation || !api->move || !api->rotate || !api->part_action || !api->part_destroy) return 0;
    memset(puppets, 0, sizeof(puppets)); memset(&shared, 0, sizeof(shared));
    puppet_slot = 0; shared.base = base; shared.api = *api;
    void* volatile* vt = (void* volatile*)(base + STEAM_UNIT_VTABLE_RVA);
    void* volatile* part_vt = (void* volatile*)(base + STEAM_SPRITE_VTABLE_RVA);
    if (slot_install(&shared.part_destroy, part_vt, (void*)api->part_destroy, (void*)on_part_destroy) != SLOT_OK ||
        slot_install(&shared.part_action, part_vt + 1, (void*)api->part_action, (void*)on_part_action) != SLOT_OK ||
        slot_install(&shared.destroy, vt, (void*)api->destroy, (void*)on_destroy) != SLOT_OK ||
        slot_install(&shared.movement, vt + 3, (void*)api->movement, (void*)on_movement) != SLOT_OK ||
        slot_install(&shared.action, vt + 1, (void*)api->action, (void*)on_action) != SLOT_OK) {
        unit_puppet_stop(); return 0;
    }
    for (unsigned int i = 0; i < UNIT_PUPPET_CAPACITY; ++i) puppets[i].state = UNIT_PUPPET_READY;
    return 1;
}
static int attach(uintptr_t game, uintptr_t player, uintptr_t required, enum PuppetMode watching) {
    if (puppet.state != UNIT_PUPPET_READY) return 0;
    __try {
        if (!game || *(uintptr_t*)game != shared.base + STEAM_GAME_VTABLE_RVA) return 0;
        for (unsigned int cat = 0; cat < STEAM_TICK_ENTITY_LIST_COUNT; ++cat) {
            uintptr_t list = game + STEAM_WORLD_LIST_OFFSET + cat * STEAM_LIST_SIZE;
            unsigned int count = *(unsigned int*)(list + 4), cap = *(unsigned int*)(list + 8);
            uintptr_t* entries = *(uintptr_t**)(list + 12);
            if (!count) continue;
            if (!entries || count > cap || count > 65536u) return 0;
            for (unsigned int i = 0; i < count; ++i) {
                uintptr_t e = entries[i];
                if (!e || e == player || (required && e != required) || *(uintptr_t*)e != shared.base + STEAM_UNIT_VTABLE_RVA) continue;
                uintptr_t vid = *(uintptr_t*)(e + STEAM_ENTITY_VID_OFFSET);
                unsigned int anim = *(unsigned int*)(e + STEAM_ENTITY_ANIM_OFFSET);
                if (!vid || (watching == PUPPET_WATCH && !required && *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET) != 50u) ||
                    *(unsigned int*)(vid + STEAM_VID_CLASS_OFFSET) != STEAM_UNIT_CLASS ||
                    *(unsigned int*)(vid + STEAM_VID_CATEGORY_OFFSET) != cat ||
                    *(int*)(e + STEAM_ENTITY_HEALTH_OFFSET) <= 0 ||
                    (watching == PUPPET_STATIONARY && required ? anim >= STEAM_ANIMATION_COUNT :
                        (anim != STEAM_ANIM_IDLE && anim != STEAM_ANIM_RUN))) continue;
                uintptr_t child = *(uintptr_t*)(e + STEAM_ENTITY_CHILD_OFFSET), child_vid = 0;
                if (child) {
                    child_vid = *(uintptr_t*)(child + STEAM_ENTITY_VID_OFFSET);
                    if (!child_vid || child_vid != *(uintptr_t*)(vid + STEAM_VID_LINKED_OFFSET) ||
                        *(unsigned int*)(child_vid + STEAM_VID_CLASS_OFFSET) != STEAM_PUPPET_PART_CLASS ||
                        *(uintptr_t*)child != shared.base + STEAM_SPRITE_VTABLE_RVA ||
                        *(uintptr_t*)(child + STEAM_ENTITY_CHILD_OFFSET) ||
                        *(uintptr_t*)(child + STEAM_ENTITY_PARENT_OFFSET) != e) continue;
                }
                int occupied = 0;
                for (unsigned int slot = 0; slot < UNIT_PUPPET_CAPACITY; ++slot)
                    if (slot != puppet_slot && puppets[slot].entity == e) { occupied = 1; break; }
                if (occupied) continue;
                puppet.game = game; puppet.entity = e; puppet.vid = vid; puppet.category = cat;
                puppet.child = child; puppet.child_vid = child_vid;
                if (!child_registered()) { puppet.child = puppet.entity = 0; continue; }
                puppet.behavior = *(unsigned int*)(e + STEAM_UNIT_BEHAVIOR_OFFSET);
                puppet.state = UNIT_PUPPET_ACTIVE;
                puppet.combat = puppet.dying = 0;
                puppet.predictive = 0; puppet.predicted_hits = 0; puppet.changing_vid = 0;
                puppet.last_event = puppet.replayed = puppet.duplicates = 0;
                puppet.attack_frame_changes = 0;
                puppet.watching = watching;
                puppet.captured_attacks = puppet.captured_deaths = 0;
                puppet.current_vid = *(unsigned int*)(vid + STEAM_VID_INDEX_OFFSET);
                puppet.form_revision = 1u; puppet.captured_forms = 0; puppet.transitioning = 0;
                puppet.death_animation = 0; memset(&puppet.death_pose, 0, sizeof(puppet.death_pose));
                if (watching == PUPPET_WATCH) return 1;
                /* A private armor successor may already be hit or killed before
                   the next coordinator tick. Adopt that native state without
                   rejecting it or restarting its death animation. */
                if (watching == PUPPET_STATIONARY &&
                    (anim == STEAM_ANIM_DEATH || anim == STEAM_ANIM_GIB_DEATH)) {
                    puppet.transitioning = anim == STEAM_ANIM_DEATH &&
                        (puppet.current_vid == 70u || puppet.current_vid == 90u);
                    if (!puppet.transitioning) capture_death(e);
                    return 1;
                }
                /* Original actions preserve goal reference counting and the
                   LIST<ACT> ABI; no raw goal=NULL or list pointer tricks. */
                shared.api.action((void*)e, NULL, STEAM_ACT_CLEAR_COMMAND, 0, 0, 0);
                shared.api.action((void*)e, NULL, STEAM_ACT_CLEAR_STACK, 0, 0, 0);
                if (child) {
                    shared.api.part_action((void*)child, NULL, STEAM_ACT_CLEAR_COMMAND, 0, 0, 0);
                    shared.api.part_action((void*)child, NULL, STEAM_ACT_CLEAR_STACK, 0, 0, 0);
                    *(unsigned int*)(child + STEAM_ENTITY_COMMAND_TIMER_OFFSET) = 0;
                }
                shared.api.action((void*)e, NULL, STEAM_ACT_STOP, 1, 0, 0);
                shared.api.action((void*)e, NULL, STEAM_ACT_SET_BEHAVIOR, 0, 0, 0);
                *(unsigned int*)(e + STEAM_ENTITY_COMMAND_TIMER_OFFSET) = 0;
                *(int*)(e + STEAM_UNIT_AVOIDANCE_OFFSET) = 0;
                *(float*)(e + STEAM_ENTITY_Z_VELOCITY_OFFSET) = 0;
                memset(&puppet.pose, 0, sizeof(puppet.pose));
                if (anim == STEAM_ANIM_IDLE || anim == STEAM_ANIM_RUN) select_animation((void*)e);
                puppet.last_frame = *(unsigned int*)(e + STEAM_ENTITY_FRAME_CURRENT_OFFSET);
                puppet.last_animation = *(unsigned int*)(e + STEAM_ENTITY_ANIM_OFFSET);
                puppet.last_first = *(unsigned int*)(e + STEAM_ENTITY_FRAME_FIRST_OFFSET);
                if (child) {
                    puppet.child_last_frame = *(unsigned int*)(child + STEAM_ENTITY_FRAME_CURRENT_OFFSET);
                    puppet.child_last_first = *(unsigned int*)(child + STEAM_ENTITY_FRAME_FIRST_OFFSET);
                    puppet.child_last_animation = *(unsigned int*)(child + STEAM_ENTITY_ANIM_OFFSET);
                }
                return 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; return -1; }
    return 0;
}
int unit_puppet_attach_first(uintptr_t game, uintptr_t player) { return attach(game, player, 0, PUPPET_CONTROLLED); }
int unit_puppet_watch_first(uintptr_t game, uintptr_t player) { return attach(game, player, 0, PUPPET_WATCH); }
int unit_puppet_attach_entity(uintptr_t game, uintptr_t player, uintptr_t entity) {
    return entity ? attach(game, player, entity, PUPPET_CONTROLLED) : 0;
}
int unit_puppet_watch_entity(uintptr_t game, uintptr_t player, uintptr_t entity) {
    return entity ? attach(game, player, entity, PUPPET_WATCH) : 0;
}
int unit_puppet_watch_stationary(uintptr_t game, uintptr_t player, uintptr_t entity) {
    return entity ? attach(game, player, entity, PUPPET_STATIONARY) : 0;
}
int unit_puppet_host_kill(uintptr_t game) {
    return unit_puppet_host_kill_variant(game, STEAM_ANIM_DEATH);
}
int unit_puppet_host_freeze(uintptr_t game) {
    if (!puppet.watching || puppet.dying || puppet.state != UNIT_PUPPET_ACTIVE ||
        puppet.current_vid < 40u || puppet.current_vid > 90u || puppet.current_vid % 10u) return 0;
    __try {
        if (!registered(game)) return 0;
        unsigned int frozen = puppet.current_vid + 9u;
        on_action((void*)puppet.entity, NULL, STEAM_ACT_CHANGE_VID, frozen, STEAM_ANIM_IDLE, 0);
        if (puppet.current_vid != frozen || !registered(game)) return 0;
        shared.api.animation((void*)puppet.entity, NULL, STEAM_ANIM_IDLE);
        shared.api.action((void*)puppet.entity, NULL, STEAM_ACT_SET_BEHAVIOR, 0, 0, 0);
        shared.api.action((void*)puppet.entity, NULL, STEAM_ACT_CLEAR_COMMAND, 0, 0, 0);
        shared.api.action((void*)puppet.entity, NULL, STEAM_ACT_CLEAR_STACK, 0, 0, 0);
        shared.api.action((void*)puppet.entity, NULL, STEAM_ACT_STOP, 1, 0, 0);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; return -1; }
}
int unit_puppet_host_kill_variant(uintptr_t game, unsigned int animation) {
    if (!puppet.watching || puppet.dying || puppet.state != UNIT_PUPPET_ACTIVE) return 0;
    if (animation != STEAM_ANIM_DEATH && animation != STEAM_ANIM_GIB_DEATH) return 0;
    __try {
        if (!registered(game)) return 0;
        on_action((void*)puppet.entity, NULL, STEAM_ACT_SET_HP,
            animation == STEAM_ANIM_GIB_DEATH ? -100000 : 0, 0, 0);
        (void)unit_puppet_sample(game); return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; return -1; }
}
int unit_puppet_apply(uintptr_t game, const UnitPuppetPose* p) {
    if (puppet.watching || puppet.dying || puppet.state != UNIT_PUPPET_ACTIVE || !p || !_finite(p->x) || !_finite(p->y) ||
        !_finite(p->z) || !_finite(p->velocity) || p->direction > 255u || p->moving > 1u) return 0;
    __try {
        if (!registered(game)) { lost(); return 0; }
        if (!child_registered()) { puppet.state = UNIT_PUPPET_FAULT; return -1; }
        unsigned int animation = *(unsigned int*)(puppet.entity + STEAM_ENTITY_ANIM_OFFSET);
        if (puppet.predictive && (animation == STEAM_ANIM_DEATH || animation == STEAM_ANIM_GIB_DEATH || puppet.transitioning)) return 1;
        puppet.pose = *p;
        shared.api.move((void*)puppet.entity, NULL, p->x, p->y, p->z);
        shared.api.rotate((void*)puppet.entity, NULL, p->direction);
        int frozen = puppet.predictive && puppet.current_vid % 10u == 9u;
        *(float*)(puppet.entity + STEAM_ENTITY_VELOCITY_OFFSET) = frozen ? 0 : p->velocity;
        uint32_t* flags = (uint32_t*)(puppet.entity + STEAM_ENTITY_FLAGS_OFFSET);
        *flags = (*flags & ~STEAM_ENTITY_MOVING_FLAG) | (p->moving && !frozen ? STEAM_ENTITY_MOVING_FLAG : 0);
        if (!frozen && !(puppet.predictive && (animation == 13u || animation == 14u)) &&
            (!puppet.combat || *(unsigned int*)(puppet.entity + STEAM_ENTITY_ANIM_OFFSET) != STEAM_ANIM_ATTACK) &&
            *(unsigned int*)(puppet.entity + STEAM_ENTITY_ANIM_OFFSET) !=
            (p->moving && p->velocity != 0 ? STEAM_ANIM_RUN : STEAM_ANIM_IDLE)) select_animation((void*)puppet.entity);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; return -1; }
}
static int sanitized(uintptr_t vid) {
    if (*(int*)(vid + STEAM_VID_DAMAGE_OFFSET) ||
        *(int*)(vid + STEAM_VID_DELETE_SCRIPT_OFFSET) != -1 ||
        *(int*)(vid + STEAM_VID_DAMAGE_SCRIPT_OFFSET) != -1 ||
        *(int*)(vid + STEAM_VID_COLLISION_SCRIPT_OFFSET) != -1 ||
        *(uintptr_t*)(vid + STEAM_VID_SERVICE_OFFSET)) return 0;
    for (unsigned int i = 0; i < STEAM_ANIMATION_COUNT; ++i)
        if (*(int*)(vid + STEAM_VID_ANIMATION_SCRIPTS_OFFSET + i * 4u) != -1 ||
            *(uintptr_t*)(vid + STEAM_VID_CREATIONS_OFFSET + i * 4u)) return 0;
    return 1;
}
int unit_puppet_prediction(uintptr_t game) {
    if (puppet.watching || puppet.state != UNIT_PUPPET_ACTIVE || puppet.dying) return 0;
    __try {
        if (!registered(game) || !child_registered() || !prediction_descriptor(puppet.vid, 1) ||
            (puppet.child && !prediction_descriptor(puppet.child_vid, 0))) return 0;
        puppet.predictive = 1; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; return -1; }
}
int unit_puppet_prediction_test_hit(uintptr_t game, int damage, int freeze) {
    if (!puppet.predictive || puppet.state != UNIT_PUPPET_ACTIVE || puppet.dying || damage <= 0 || damage > 100000) return 0;
    __try {
        if (!registered(game)) return 0;
        on_action((void*)puppet.entity, NULL, STEAM_ACT_DAMAGE, damage, 0, 0);
        if (puppet.state != UNIT_PUPPET_ACTIVE || !registered(game)) return 0;
        unsigned int animation = *(unsigned int*)(puppet.entity + STEAM_ENTITY_ANIM_OFFSET);
        if (freeze && (animation == STEAM_ANIM_DEATH || animation == STEAM_ANIM_GIB_DEATH)) {
            if (puppet.current_vid < 40u || puppet.current_vid > 90u || puppet.current_vid % 10u) return 0;
            unsigned int frozen = puppet.current_vid + 9u;
            on_action((void*)puppet.entity, NULL, STEAM_ACT_CHANGE_VID, frozen, STEAM_ANIM_IDLE, 0);
            if (puppet.current_vid != frozen || !registered(game)) return 0;
            on_action((void*)puppet.entity, NULL, STEAM_ACT_SET_ANIMATION, STEAM_ANIM_IDLE, 0, 0);
            on_action((void*)puppet.entity, NULL, STEAM_ACT_SET_BEHAVIOR, 0, 0, 0);
            on_action((void*)puppet.entity, NULL, STEAM_ACT_CLEAR_COMMAND, 0, 0, 0);
            on_action((void*)puppet.entity, NULL, STEAM_ACT_CLEAR_STACK, 0, 0, 0);
            on_action((void*)puppet.entity, NULL, STEAM_ACT_STOP, 1, 0, 0);
        }
        return puppet.state == UNIT_PUPPET_ACTIVE ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; return -1; }
}
int unit_puppet_health(uintptr_t game, int health) {
    if (puppet.watching || puppet.state != UNIT_PUPPET_ACTIVE || health <= 0 || health > 100000 || puppet.dying) return 0;
    __try {
        if (!registered(game) || !child_registered() || !sanitized(puppet.vid) ||
            (puppet.child && !sanitized(puppet.child_vid))) return 0;
        shared.api.action((void*)puppet.entity, NULL, STEAM_ACT_SET_HP, health, 0, 0); return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; return -1; }
}
int unit_puppet_replay(uintptr_t game, uint32_t sequence, unsigned int animation) {
    if (puppet.watching || puppet.state != UNIT_PUPPET_ACTIVE || !sequence ||
        (animation != STEAM_ANIM_ATTACK && animation != STEAM_ANIM_DEATH)) return 0;
    __try {
        if (!registered(game) || !child_registered() || !sanitized(puppet.vid) ||
            (puppet.child && !sanitized(puppet.child_vid))) return 0;
        uint32_t delta = sequence - puppet.last_event;
        if (puppet.last_event && (!delta || delta >= 0x80000000u)) { ++puppet.duplicates; return 0; }
        if (puppet.dying) return 0;
        puppet.combat = 1;
        if (animation == STEAM_ANIM_DEATH) {
            puppet.dying = 1;
            shared.api.action((void*)puppet.entity, NULL, STEAM_ACT_SET_HP, 0, 0, 0);
        } else {
            shared.api.animation((void*)puppet.entity, NULL, animation);
            if (puppet.child) shared.api.animation((void*)puppet.child, NULL, animation);
        }
        puppet.last_event = sequence; ++puppet.replayed; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; return -1; }
}
int unit_puppet_test_damage(uintptr_t game) {
    if (puppet.state != UNIT_PUPPET_ACTIVE) return 0;
    __try {
        if (!registered(game) || !child_registered()) return 0;
        int hp = *(int*)(puppet.entity + STEAM_ENTITY_HEALTH_OFFSET);
        on_action((void*)puppet.entity, NULL, STEAM_ACT_DAMAGE, 100000, 0, 0);
        on_action((void*)puppet.entity, NULL, STEAM_ACT_SET_HP, 0, 0, 0);
        if (*(int*)(puppet.entity + STEAM_ENTITY_HEALTH_OFFSET) != hp) return 0;
        if (puppet.child) {
            hp = *(int*)(puppet.child + STEAM_ENTITY_HEALTH_OFFSET);
            on_part_action((void*)puppet.child, NULL, STEAM_ACT_DAMAGE, 100000, 0, 0);
            on_part_action((void*)puppet.child, NULL, STEAM_ACT_SET_HP, 0, 0, 0);
            if (*(int*)(puppet.child + STEAM_ENTITY_HEALTH_OFFSET) != hp) return 0;
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; return -1; }
}
UnitPuppetSample unit_puppet_sample(uintptr_t game) {
    UnitPuppetSample s = {0}; s.state = puppet.state; s.entity = puppet.entity;
    s.decisions = puppet.decisions; s.movements = puppet.movements; s.blocked = puppet.blocked;
    s.replayed = puppet.replayed; s.duplicates = puppet.duplicates;
    s.captured_attacks = puppet.captured_attacks; s.captured_deaths = puppet.captured_deaths;
    s.death_animation = puppet.death_animation; s.death_pose = puppet.death_pose;
    s.vid = puppet.current_vid; s.form_revision = puppet.form_revision;
    s.captured_forms = puppet.captured_forms; s.transitioning = puppet.transitioning;
    __try {
        if (puppet.entity && (puppet.watching || puppet.predictive)) {
            /* Validate membership before observing a potentially changed VID. */
            uintptr_t list = game + STEAM_WORLD_LIST_OFFSET + puppet.category * STEAM_LIST_SIZE;
            unsigned int count = *(unsigned int*)(list + 4), cap = *(unsigned int*)(list + 8);
            uintptr_t* entries = *(uintptr_t**)(list + 12);
            if (game == puppet.game && entries && count <= cap && count <= 65536u)
                for (unsigned int i = 0; i < count; ++i) if (entries[i] == puppet.entity) {
                    observe_form(puppet.entity, 0, NULL); break;
                }
        }
        if (puppet.entity && !registered(game)) { lost(); s.state = puppet.state; s.entity = 0; }
        if (s.entity) {
            uintptr_t e = s.entity;
            s.vid = *(unsigned int*)(puppet.vid + STEAM_VID_INDEX_OFFSET);
            s.class_id = STEAM_UNIT_CLASS;
            s.health = *(int*)(e + STEAM_ENTITY_HEALTH_OFFSET);
            s.animation = *(unsigned int*)(e + STEAM_ENTITY_ANIM_OFFSET);
            if ((puppet.watching || puppet.predictive) && (puppet.current_vid == 70u || puppet.current_vid == 90u) &&
                s.animation == STEAM_ANIM_DEATH) puppet.transitioning = 1;
            if ((puppet.watching || puppet.predictive) && !puppet.dying && !puppet.transitioning &&
                (s.animation == STEAM_ANIM_DEATH || s.animation == STEAM_ANIM_GIB_DEATH)) capture_death(e);
            s.frame = *(unsigned int*)(e + STEAM_ENTITY_FRAME_CURRENT_OFFSET);
            s.frame_first = *(unsigned int*)(e + STEAM_ENTITY_FRAME_FIRST_OFFSET);
            s.frame_last = *(unsigned int*)(e + STEAM_ENTITY_FRAME_LAST_OFFSET);
            /* A direction change selects another range; that alone is not
               evidence of native frame progression. Single-frame art is valid. */
            if (s.animation == puppet.last_animation && s.frame_first == puppet.last_first && s.frame != puppet.last_frame) {
                ++puppet.frame_changes;
                if (s.animation == STEAM_ANIM_ATTACK) ++puppet.attack_frame_changes;
            }
            puppet.last_first = s.frame_first;
            puppet.last_frame = s.frame; puppet.last_animation = s.animation;
            s.behavior = *(unsigned int*)(e + STEAM_UNIT_BEHAVIOR_OFFSET);
            s.goal = *(uintptr_t*)(e + STEAM_ENTITY_GOAL_OFFSET);
            s.command = *(unsigned int*)(e + STEAM_ENTITY_FLAGS_OFFSET) & STEAM_ENTITY_COMMAND_MASK;
            s.stack = *(unsigned int*)(e + STEAM_ENTITY_STACK_COUNT_OFFSET);
            s.moving = !!(*(unsigned int*)(e + STEAM_ENTITY_FLAGS_OFFSET) & STEAM_ENTITY_MOVING_FLAG);
            s.direction = *(unsigned char*)(e + STEAM_ENTITY_DIRECTION_OFFSET);
            s.velocity = *(float*)(e + STEAM_ENTITY_VELOCITY_OFFSET);
            s.x = *(float*)(e + STEAM_ENTITY_X_OFFSET); s.y = *(float*)(e + STEAM_ENTITY_Y_OFFSET);
            s.z = *(float*)(e + STEAM_ENTITY_Z_OFFSET);
            if (!child_registered()) { puppet.state = s.state = UNIT_PUPPET_FAULT; }
            else if (puppet.child) {
                s.child = puppet.child;
                s.child_goal = *(uintptr_t*)(puppet.child + STEAM_ENTITY_GOAL_OFFSET);
                s.child_command = *(unsigned int*)(puppet.child + STEAM_ENTITY_FLAGS_OFFSET) & STEAM_ENTITY_COMMAND_MASK;
                s.child_stack = *(unsigned int*)(puppet.child + STEAM_ENTITY_STACK_COUNT_OFFSET);
                s.child_frame_first = *(unsigned int*)(puppet.child + STEAM_ENTITY_FRAME_FIRST_OFFSET);
                s.child_frame = *(unsigned int*)(puppet.child + STEAM_ENTITY_FRAME_CURRENT_OFFSET);
                s.child_frame_last = *(unsigned int*)(puppet.child + STEAM_ENTITY_FRAME_LAST_OFFSET);
                unsigned int child_animation = *(unsigned int*)(puppet.child + STEAM_ENTITY_ANIM_OFFSET);
                s.child_animation = child_animation;
                if (child_animation == puppet.child_last_animation && s.child_frame_first == puppet.child_last_first &&
                    s.child_frame != puppet.child_last_frame) {
                    ++puppet.child_frame_changes;
                    if (child_animation == STEAM_ANIM_ATTACK) ++puppet.attack_frame_changes;
                }
                puppet.child_last_animation = child_animation; puppet.child_last_frame = s.child_frame;
                puppet.child_last_first = s.child_frame_first;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = s.state = UNIT_PUPPET_FAULT; }
    s.frame_changes = puppet.frame_changes; s.child_frame_changes = puppet.child_frame_changes;
    s.vid = puppet.current_vid; s.form_revision = puppet.form_revision;
    s.captured_forms = puppet.captured_forms; s.transitioning = puppet.transitioning;
    s.captured_deaths = puppet.captured_deaths; s.death_animation = puppet.death_animation; s.death_pose = puppet.death_pose;
    s.predicted_hits = puppet.predicted_hits; s.predictive = puppet.predictive; s.state = puppet.state;
    s.attack_frame_changes = puppet.attack_frame_changes; return s;
}
int unit_puppet_release(uintptr_t game) {
    __try {
        if (puppet.entity && registered(game) && !puppet.dying && !puppet.transitioning && !puppet.predictive && puppet.watching != PUPPET_WATCH) {
            shared.api.action((void*)puppet.entity, NULL, STEAM_ACT_STOP, 1, 0, 0);
            shared.api.action((void*)puppet.entity, NULL, STEAM_ACT_SET_BEHAVIOR, puppet.behavior, 0, 0);
            shared.api.animation((void*)puppet.entity, NULL, STEAM_ANIM_IDLE);
        }
        puppet.entity = puppet.child = 0;
        if (puppet.state != UNIT_PUPPET_OFF) puppet.state = UNIT_PUPPET_READY;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; return 0; }
}
int unit_puppet_stop(void) {
    for (unsigned int i = 0; i < UNIT_PUPPET_CAPACITY; ++i) if (puppets[i].entity) return 0;
    int a = slot_stop(&shared.action) == SLOT_OK;
    int m = slot_stop(&shared.movement) == SLOT_OK;
    int d = slot_stop(&shared.destroy) == SLOT_OK;
    int pa = slot_stop(&shared.part_action) == SLOT_OK;
    int pd = slot_stop(&shared.part_destroy) == SLOT_OK;
    if (a && m && d && pa && pd)
        for (unsigned int i = 0; i < UNIT_PUPPET_CAPACITY; ++i) puppets[i].state = UNIT_PUPPET_OFF;
    /* Keep original call targets: a foreign hook or already fetched callback
       may still chain to us after stop. */
    return a && m && d && pa && pd;
}
