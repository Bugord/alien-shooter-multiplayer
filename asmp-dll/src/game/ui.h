#ifndef ASMP_UI_H
#define ASMP_UI_H
#include "actor.h"
/* Version-specific native UI and map bindings. All calls are game-thread only.
   Ownership of native objects this layer touches:
   - Name label: ui_name_label draws a caller-owned string through the game's
     Render::DrawText during the display hook; it creates no native object.
     (A child STEXT was destroyed by the MAN destructor cascade.)
   - Map string: ui_load_map creates it with the game allocator and load_map
     consumes it; the caller never frees it.
   - Menu items and the status entity belong to the menu map; never stored.
   - Text argument: ui_set_text and ui_set_text_file pass one native string object to
     the STEXT action. ui.c creates it with the game allocator on first use, reassigns
     it for every call and keeps it for the life of the process (no native free is
     bound, so it is never released; it is one short allocation). */
int ui_bind(uintptr_t base);
void ui_stop(void);
uintptr_t ui_menu_item(uintptr_t game, unsigned int vid, unsigned int direction);
int ui_text(uintptr_t entity, char* out, unsigned int capacity);
void ui_menu_status(uintptr_t entity, int status);
void ui_button(uintptr_t entity, int disabled);
/* Sets the text of a menu STEXT through its native ACT_SET_TEXT, so the game also
   recalculates the visible length. The text is copied; cp1251 bytes pass through.
   Returns 1 when the action ran, 0 for an entity that is not an STEXT or a fault. */
int ui_set_text(uintptr_t entity, const char* text);
/* Loads a text file (relative to the game folder) into a menu STEXT (ACT_SET_FILE). */
int ui_set_text_file(uintptr_t entity, const char* path);
int ui_load_map(uintptr_t game, const char* map);
/* Draws ASCII text at screen pixel (x, y) with Render::DrawText; display hook only.
   The text is borrowed for the call. */
void ui_screen_text(float x, float y, const char* text, uint32_t color);
/* Draws a name above the entity; call from the display hook, before EndScene. */
void ui_name_label(uintptr_t game, uintptr_t entity, const char* name);
/* Shows every weapon icon and ammo bar of the native state bar (they stay hidden
   for weapons granted after the level started). Returns how many were shown. */
int ui_show_weapon_icons(uintptr_t game);
/* max_health <= 0 (unknown) falls back to the default player maximum. */
void ui_health_bar(uintptr_t game, uintptr_t entity, int health, int max_health);
#endif
