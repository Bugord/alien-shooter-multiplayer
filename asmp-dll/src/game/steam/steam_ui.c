#if defined(ASMP_STEAM_BUILD)
#include <string.h>
#include "steam_ui.h"
static uintptr_t base;
typedef void* (__fastcall* StringCreate)(void*, void*, const char*);
typedef void (__fastcall* LoadMap)(void*, void*, char*);
typedef int (__fastcall* ListRemove)(void*, void*, void*);
typedef int (__fastcall* AddChild)(void*, void*, void*);
typedef void (__fastcall* SetAnimation)(void*, void*, unsigned int);
typedef void (__fastcall* DrawRect)(void*, void*, float, float, float, float, uint32_t);
int steam_ui_bind(uintptr_t image)
{
    if (base) return 0;
    /* Steam strings own their allocation: load_map consumes a string by value.
       STEXT text is at +0x74, not the old +0x70; assign with the game's allocator. */
    static const struct { unsigned int rva; unsigned char bytes[5]; } signatures[] = {
        {STEAM_LOAD_MAP_RVA, {0x55,0x8B,0xEC,0x6A,0xFF}},
        {STEAM_STRING_CREATE_RVA, {0x55,0x8B,0xEC,0x53,0x57}},
        {STEAM_STRING_ASSIGN_RVA, {0x55,0x8B,0xEC,0x53,0x8B}},
        {STEAM_LIST_REMOVE_RVA, {0x55,0x8B,0xEC,0x83,0x7D}},
        {STEAM_ADD_CHILD_RVA, {0x55,0x8B,0xEC,0x8B,0x55}},
        {STEAM_DRAW_RECT_RVA, {0x55,0x8B,0xEC,0x83,0xEC}},
        {STEAM_SET_ANIMATION_RVA, {0x55,0x8B,0xEC,0x51,0x53}}
    };
    __try {
        for (unsigned int i = 0; i < sizeof(signatures)/sizeof(signatures[0]); ++i)
            if (memcmp((void*)(image + signatures[i].rva), signatures[i].bytes, 5)) return 0;
        if (*(uintptr_t*)(image + STEAM_GAME_VTABLE_RVA + STEAM_LOAD_MAP_SLOT * sizeof(uintptr_t)) != image + STEAM_LOAD_MAP_RVA) return 0;
        /* Verified STEXT vtable destructor/action and constructor assignment. */
        if (*(uintptr_t*)(image + STEAM_STEXT_VTABLE_RVA) != image + STEAM_STEXT_DESTRUCTOR_RVA ||
            *(uintptr_t*)(image + STEAM_STEXT_VTABLE_RVA + sizeof(uintptr_t)) != image + STEAM_STEXT_ACTION_RVA) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    base = image; return 1;
}
void steam_ui_stop(void) { base = 0; }
uintptr_t steam_ui_menu_item(uintptr_t game, unsigned int vid, unsigned int direction)
{
    __try {
        uintptr_t list = game + STEAM_GAME_MENU_LIST_OFFSET;
        unsigned int count = *(unsigned int*)(list + 4);
        unsigned int cap = *(unsigned int*)(list + 8);
        uintptr_t* items = *(uintptr_t**)(list + 12);
        if (!items || count > cap || count > 4096) return 0;
        for (unsigned int i = 0; i < count; ++i) {
            uintptr_t e = items[i]; if (!e) continue;
            uintptr_t v = *(uintptr_t*)(e + STEAM_ENTITY_VID_OFFSET);
            if (v && *(unsigned int*)(v + STEAM_VID_INDEX_OFFSET) == vid &&
                *(unsigned char*)(e + STEAM_ENTITY_DIRECTION_OFFSET) == direction) return e;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 0;
}
int steam_ui_text(uintptr_t entity, char* out, unsigned int capacity)
{
    __try {
        if (!capacity || *(uintptr_t*)entity != base + STEAM_STEXT_VTABLE_RVA) return 0;
        const char* text = *(const char**)(entity + STEAM_STEXT_TEXT_OFFSET);
        if (!text) return 0;
        unsigned int i;
        for (i = 0; i + 1 < capacity && text[i]; ++i) out[i] = text[i];
        out[i] = 0; return text[i] == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
void steam_ui_menu_status(uintptr_t entity, int status)
{ __try { *(int*)(entity + STEAM_ENTITY_HEALTH_OFFSET) = status; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
void steam_ui_button(uintptr_t entity, int disabled)
{ __try { ((SetAnimation)(base + STEAM_SET_ANIMATION_RVA))((void*)entity, NULL, disabled ? 2u : 0u); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
int steam_ui_load_map(uintptr_t game, const char* map)
{
    __try {
        char* owned = NULL;
        ((StringCreate)(base + STEAM_STRING_CREATE_RVA))(&owned, NULL, map);
        /* Use the validated MAP slot so the production load observer also sees
           direct connections and return-to-menu loads. The callee owns owned. */
        ((LoadMap)(*(uintptr_t*)(base + STEAM_GAME_VTABLE_RVA + STEAM_LOAD_MAP_SLOT * sizeof(uintptr_t))))((void*)game, NULL, owned);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static uintptr_t find_name(const SteamActor* actor)
{
    uintptr_t child = *(uintptr_t*)(actor->entity + STEAM_ENTITY_CHILD_OFFSET);
    /* Engine child chains can contain the torso and other attachments. */
    for (unsigned int i = 0; child && i < 64; ++i) {
        if (*(uintptr_t*)child == base + STEAM_STEXT_VTABLE_RVA) return child;
        child = *(uintptr_t*)(child + STEAM_ENTITY_CHILD_OFFSET);
    }
    return 0;
}
int steam_ui_name(SteamActor* actor, const char* name)
{
    __try {
        if (!actor->entity || steam_actor_torso_ready(actor) != 1) return 0;
        uintptr_t text = find_name(actor);
        if (!text) {
            uintptr_t font = *(uintptr_t*)(actor->game + STEAM_GAME_VID_TABLE_OFFSET + STEAM_FONT_VID_INDEX * sizeof(uintptr_t));
            if (!font || *(unsigned int*)(font + STEAM_VID_INDEX_OFFSET) != STEAM_FONT_VID_INDEX) return 0;
            text = (uintptr_t)actor->engine.create((void*)actor->game, NULL, (void*)font,
                0, 0, 0, 0, (void*)actor->entity);
            if (!text || *(uintptr_t*)text != base + STEAM_STEXT_VTABLE_RVA) return 0;
            ((AddChild)(base + STEAM_ADD_CHILD_RVA))((void*)actor->entity, NULL, (void*)text);
            /* Native remove-by-pointer decrements the menu reference; the
               world list still owns STEXT. Never splice reference counts. */
            ((ListRemove)(base + STEAM_LIST_REMOVE_RVA))((void*)(actor->game + STEAM_GAME_MENU_LIST_OFFSET), NULL, (void*)text);
        }
        const char* previous = *(const char**)(text + STEAM_STEXT_TEXT_OFFSET);
        if (!previous || strcmp(previous, name))
            ((StringCreate)(base + STEAM_STRING_ASSIGN_RVA))((void*)(text + STEAM_STEXT_TEXT_OFFSET), NULL, name);
        *(float*)(text + STEAM_ENTITY_X_OFFSET) = *(float*)(actor->entity + STEAM_ENTITY_X_OFFSET) - 40.0f;
        *(float*)(text + STEAM_ENTITY_Y_OFFSET) = *(float*)(actor->entity + STEAM_ENTITY_Y_OFFSET) - 92.0f;
        *(float*)(text + STEAM_ENTITY_Z_OFFSET) = *(float*)(actor->entity + STEAM_ENTITY_Z_OFFSET);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
void steam_ui_health_bar(uintptr_t game, uintptr_t entity, int health)
{
    __try {
        uintptr_t render = *(uintptr_t*)(base + STEAM_RENDER_PTR_RVA);
        if (!render || !entity || health <= 0) return;
        float x = *(float*)(entity + STEAM_ENTITY_X_OFFSET) - *(float*)(game + STEAM_GAME_CAMERA_X_OFFSET) - 40.0f;
        float y = *(float*)(entity + STEAM_ENTITY_Y_OFFSET) - *(float*)(game + STEAM_GAME_CAMERA_Y_OFFSET) - 80.0f;
        /* Preserve the original prototype's 110 maximum until the protocol
           carries player stats. Clamp the bar when campaigns give more HP. */
        float width = (float)(health > 110 ? 110 : health) * 78.0f / 110.0f;
        DrawRect draw = (DrawRect)(base + STEAM_DRAW_RECT_RVA);
        draw((void*)render, NULL, x, y, x+80, y+7, 0xFFADA698);
        draw((void*)render, NULL, x+1, y+1, x+78, y+5, 0xFF000000);
        draw((void*)render, NULL, x+1, y+1, x+1+width, y+5, 0xFFD33104);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
#endif
