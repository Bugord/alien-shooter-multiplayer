#include <string.h>
#include <math.h>
#include <float.h>
#include "unit_puppet.h"
#include "slot_hook.h"

static struct {
    uintptr_t base, game, entity, vid, child, child_vid;
    unsigned int category, behavior, decisions, movements, blocked, frame_changes, last_frame, last_animation;
    unsigned int last_first, child_last_frame, child_last_first, child_last_animation, child_frame_changes;
    enum UnitPuppetState state;
    UnitPuppetEngine api;
    UnitPuppetPose pose;
    uint32_t last_event;
    unsigned int replayed, duplicates, attack_frame_changes;
    int combat, dying;
    SlotHook action, movement, destroy, part_action, part_destroy;
} puppet;

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
        *(uintptr_t*)game != puppet.base + STEAM_GAME_VTABLE_RVA) return 0;
    uintptr_t list = game + STEAM_WORLD_LIST_OFFSET + puppet.category * STEAM_LIST_SIZE;
    unsigned int count = *(unsigned int*)(list + 4), cap = *(unsigned int*)(list + 8);
    uintptr_t* items = *(uintptr_t**)(list + 12);
    if (!items || count > cap || count > 65536u) return 0;
    for (unsigned int i = 0; i < count; ++i)
        if (items[i] == puppet.entity)
            return *(uintptr_t*)puppet.entity == puppet.base + STEAM_UNIT_VTABLE_RVA &&
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
            return *(uintptr_t*)puppet.child == puppet.base + STEAM_SPRITE_VTABLE_RVA &&
                *(uintptr_t*)(puppet.child + STEAM_ENTITY_VID_OFFSET) == puppet.child_vid &&
                !*(uintptr_t*)(puppet.child + STEAM_ENTITY_CHILD_OFFSET) &&
                *(uintptr_t*)(puppet.child + STEAM_ENTITY_PARENT_OFFSET) == puppet.entity;
    return 0;
}
static void select_animation(void* entity) {
    unsigned int animation = puppet.pose.moving && puppet.pose.velocity != 0 ? STEAM_ANIM_RUN : STEAM_ANIM_IDLE;
    puppet.api.animation(entity, NULL, animation);
}
static int __fastcall on_action(void* e, void* unused, unsigned int kind, intptr_t a, intptr_t b, intptr_t c) {
    (void)unused;
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
    return puppet.api.action(e, NULL, kind, a, b, c);
}
static void __fastcall on_movement(void* e, void* unused) {
    (void)unused;
    if (selected(e)) {
        if (puppet.state == UNIT_PUPPET_FAULT) return;
        __try {
            if (registered(puppet.game)) { ++puppet.movements; return; }
            lost();
        } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; return; }
    }
    puppet.api.movement(e, NULL);
}
static void* __fastcall on_destroy(void* e, void* unused, int release) {
    (void)unused;
    if (puppet.entity == (uintptr_t)e) lost();
    return puppet.api.destroy(e, NULL, release);
}
static int __fastcall on_part_action(void* e, void* unused, unsigned int kind, intptr_t a, intptr_t b, intptr_t c) {
    (void)unused;
    if (selected_part(e)) {
        __try {
            if (!child_registered()) { puppet.state = UNIT_PUPPET_FAULT; return 0; }
            if (kind == STEAM_ACT_NEXT_COMMAND) {
                ++puppet.decisions;
                if (puppet.combat && !puppet.dying) puppet.api.animation(e, NULL, STEAM_ANIM_IDLE);
                return 0;
            }
            if (kind != 15u && kind != 0x57u && kind != 0x5Cu && kind != 0x5Eu && kind != 0x60u) {
                ++puppet.blocked; return 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; return 0; }
    }
    return puppet.api.part_action(e, NULL, kind, a, b, c);
}
static void* __fastcall on_part_destroy(void* e, void* unused, int release) {
    (void)unused;
    if (puppet.child == (uintptr_t)e) {
        puppet.child = 0;
        if (!puppet.dying) puppet.state = UNIT_PUPPET_FAULT;
    }
    return puppet.api.part_destroy(e, NULL, release);
}
int unit_puppet_install(uintptr_t base, const UnitPuppetEngine* api) {
    if (puppet.action.slot || puppet.movement.slot || puppet.destroy.slot || puppet.part_action.slot || puppet.part_destroy.slot || !base || !api ||
        !api->action || !api->destroy || !api->movement || !api->animation || !api->move || !api->rotate || !api->part_action || !api->part_destroy) return 0;
    memset(&puppet, 0, sizeof(puppet)); puppet.base = base; puppet.api = *api;
    void* volatile* vt = (void* volatile*)(base + STEAM_UNIT_VTABLE_RVA);
    void* volatile* part_vt = (void* volatile*)(base + STEAM_SPRITE_VTABLE_RVA);
    if (slot_install(&puppet.part_destroy, part_vt, (void*)api->part_destroy, (void*)on_part_destroy) != SLOT_OK ||
        slot_install(&puppet.part_action, part_vt + 1, (void*)api->part_action, (void*)on_part_action) != SLOT_OK ||
        slot_install(&puppet.destroy, vt, (void*)api->destroy, (void*)on_destroy) != SLOT_OK ||
        slot_install(&puppet.movement, vt + 3, (void*)api->movement, (void*)on_movement) != SLOT_OK ||
        slot_install(&puppet.action, vt + 1, (void*)api->action, (void*)on_action) != SLOT_OK) {
        unit_puppet_stop(); return 0;
    }
    puppet.state = UNIT_PUPPET_READY; return 1;
}
int unit_puppet_attach_first(uintptr_t game, uintptr_t player) {
    if (puppet.state != UNIT_PUPPET_READY) return 0;
    __try {
        if (!game || *(uintptr_t*)game != puppet.base + STEAM_GAME_VTABLE_RVA) return 0;
        for (unsigned int cat = 0; cat < STEAM_TICK_ENTITY_LIST_COUNT; ++cat) {
            uintptr_t list = game + STEAM_WORLD_LIST_OFFSET + cat * STEAM_LIST_SIZE;
            unsigned int count = *(unsigned int*)(list + 4), cap = *(unsigned int*)(list + 8);
            uintptr_t* entries = *(uintptr_t**)(list + 12);
            if (!count) continue;
            if (!entries || count > cap || count > 65536u) return 0;
            for (unsigned int i = 0; i < count; ++i) {
                uintptr_t e = entries[i];
                if (!e || e == player || *(uintptr_t*)e != puppet.base + STEAM_UNIT_VTABLE_RVA) continue;
                uintptr_t vid = *(uintptr_t*)(e + STEAM_ENTITY_VID_OFFSET);
                unsigned int anim = *(unsigned int*)(e + STEAM_ENTITY_ANIM_OFFSET);
                if (!vid || *(unsigned int*)(vid + STEAM_VID_CLASS_OFFSET) != STEAM_UNIT_CLASS ||
                    *(unsigned int*)(vid + STEAM_VID_CATEGORY_OFFSET) != cat ||
                    *(int*)(e + STEAM_ENTITY_HEALTH_OFFSET) <= 0 ||
                    (anim != STEAM_ANIM_IDLE && anim != STEAM_ANIM_RUN)) continue;
                uintptr_t child = *(uintptr_t*)(e + STEAM_ENTITY_CHILD_OFFSET), child_vid = 0;
                if (child) {
                    child_vid = *(uintptr_t*)(child + STEAM_ENTITY_VID_OFFSET);
                    if (!child_vid || child_vid != *(uintptr_t*)(vid + STEAM_VID_LINKED_OFFSET) ||
                        *(unsigned int*)(child_vid + STEAM_VID_CLASS_OFFSET) != STEAM_PUPPET_PART_CLASS ||
                        *(uintptr_t*)child != puppet.base + STEAM_SPRITE_VTABLE_RVA ||
                        *(uintptr_t*)(child + STEAM_ENTITY_CHILD_OFFSET) ||
                        *(uintptr_t*)(child + STEAM_ENTITY_PARENT_OFFSET) != e) continue;
                }
                puppet.game = game; puppet.entity = e; puppet.vid = vid; puppet.category = cat;
                puppet.child = child; puppet.child_vid = child_vid;
                if (!child_registered()) { puppet.child = puppet.entity = 0; continue; }
                puppet.behavior = *(unsigned int*)(e + STEAM_UNIT_BEHAVIOR_OFFSET);
                puppet.state = UNIT_PUPPET_ACTIVE;
                puppet.combat = puppet.dying = 0;
                puppet.last_event = puppet.replayed = puppet.duplicates = 0;
                puppet.attack_frame_changes = 0;
                /* Original actions preserve goal reference counting and the
                   LIST<ACT> ABI; no raw goal=NULL or list pointer tricks. */
                puppet.api.action((void*)e, NULL, STEAM_ACT_CLEAR_COMMAND, 0, 0, 0);
                puppet.api.action((void*)e, NULL, STEAM_ACT_CLEAR_STACK, 0, 0, 0);
                if (child) {
                    puppet.api.part_action((void*)child, NULL, STEAM_ACT_CLEAR_COMMAND, 0, 0, 0);
                    puppet.api.part_action((void*)child, NULL, STEAM_ACT_CLEAR_STACK, 0, 0, 0);
                    *(unsigned int*)(child + STEAM_ENTITY_COMMAND_TIMER_OFFSET) = 0;
                }
                puppet.api.action((void*)e, NULL, STEAM_ACT_STOP, 1, 0, 0);
                puppet.api.action((void*)e, NULL, STEAM_ACT_SET_BEHAVIOR, 0, 0, 0);
                *(unsigned int*)(e + STEAM_ENTITY_COMMAND_TIMER_OFFSET) = 0;
                *(int*)(e + STEAM_UNIT_AVOIDANCE_OFFSET) = 0;
                *(float*)(e + STEAM_ENTITY_Z_VELOCITY_OFFSET) = 0;
                memset(&puppet.pose, 0, sizeof(puppet.pose));
                select_animation((void*)e);
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
int unit_puppet_apply(uintptr_t game, const UnitPuppetPose* p) {
    if (puppet.state != UNIT_PUPPET_ACTIVE || !p || !_finite(p->x) || !_finite(p->y) ||
        !_finite(p->z) || !_finite(p->velocity) || p->direction > 255u || p->moving > 1u) return 0;
    __try {
        if (!registered(game)) { lost(); return 0; }
        if (!child_registered()) { puppet.state = UNIT_PUPPET_FAULT; return -1; }
        puppet.pose = *p;
        puppet.api.move((void*)puppet.entity, NULL, p->x, p->y, p->z);
        puppet.api.rotate((void*)puppet.entity, NULL, p->direction);
        *(float*)(puppet.entity + STEAM_ENTITY_VELOCITY_OFFSET) = p->velocity;
        uint32_t* flags = (uint32_t*)(puppet.entity + STEAM_ENTITY_FLAGS_OFFSET);
        *flags = (*flags & ~STEAM_ENTITY_MOVING_FLAG) | (p->moving ? STEAM_ENTITY_MOVING_FLAG : 0);
        if (!puppet.combat && *(unsigned int*)(puppet.entity + STEAM_ENTITY_ANIM_OFFSET) !=
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
int unit_puppet_replay(uintptr_t game, uint32_t sequence, unsigned int animation) {
    if (puppet.state != UNIT_PUPPET_ACTIVE || !sequence ||
        (animation != STEAM_ANIM_ATTACK && animation != STEAM_ANIM_DEATH)) return 0;
    __try {
        if (!registered(game) || !child_registered() || !sanitized(puppet.vid) ||
            (puppet.child && !sanitized(puppet.child_vid))) return 0;
        if (sequence <= puppet.last_event) { ++puppet.duplicates; return 0; }
        if (puppet.dying) return 0;
        puppet.combat = 1;
        if (animation == STEAM_ANIM_DEATH) {
            puppet.dying = 1;
            puppet.api.action((void*)puppet.entity, NULL, STEAM_ACT_SET_HP, 0, 0, 0);
        } else {
            puppet.api.animation((void*)puppet.entity, NULL, animation);
            if (puppet.child) puppet.api.animation((void*)puppet.child, NULL, animation);
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
    __try {
        if (puppet.entity && !registered(game)) { lost(); s.state = puppet.state; s.entity = 0; }
        if (s.entity) {
            uintptr_t e = s.entity;
            s.vid = *(unsigned int*)(puppet.vid + STEAM_VID_INDEX_OFFSET);
            s.class_id = STEAM_UNIT_CLASS;
            s.health = *(int*)(e + STEAM_ENTITY_HEALTH_OFFSET);
            s.animation = *(unsigned int*)(e + STEAM_ENTITY_ANIM_OFFSET);
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
    s.attack_frame_changes = puppet.attack_frame_changes; return s;
}
int unit_puppet_release(uintptr_t game) {
    __try {
        if (puppet.entity && registered(game) && !puppet.dying) {
            puppet.api.action((void*)puppet.entity, NULL, STEAM_ACT_STOP, 1, 0, 0);
            puppet.api.action((void*)puppet.entity, NULL, STEAM_ACT_SET_BEHAVIOR, puppet.behavior, 0, 0);
            puppet.api.animation((void*)puppet.entity, NULL, STEAM_ANIM_IDLE);
        }
        puppet.entity = puppet.child = 0;
        if (puppet.state != UNIT_PUPPET_OFF) puppet.state = UNIT_PUPPET_READY;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { puppet.state = UNIT_PUPPET_FAULT; return 0; }
}
int unit_puppet_stop(void) {
    if (puppet.entity) return 0;
    int a = slot_stop(&puppet.action) == SLOT_OK;
    int m = slot_stop(&puppet.movement) == SLOT_OK;
    int d = slot_stop(&puppet.destroy) == SLOT_OK;
    int pa = slot_stop(&puppet.part_action) == SLOT_OK;
    int pd = slot_stop(&puppet.part_destroy) == SLOT_OK;
    if (a && m && d && pa && pd) puppet.state = UNIT_PUPPET_OFF;
    /* Keep original call targets: a foreign hook or already fetched callback
       may still chain to us after stop. */
    return a && m && d && pa && pd;
}
