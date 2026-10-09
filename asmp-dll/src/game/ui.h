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
   - Menu items and the status entity belong to the menu map; never stored. */
int ui_bind(uintptr_t base);
void ui_stop(void);
uintptr_t ui_menu_item(uintptr_t game, unsigned int vid, unsigned int direction);
int ui_text(uintptr_t entity, char* out, unsigned int capacity);
void ui_menu_status(uintptr_t entity, int status);
void ui_button(uintptr_t entity, int disabled);
int ui_load_map(uintptr_t game, const char* map);
/* Draws a name above the entity; call from the display hook, before EndScene. */
void ui_name_label(uintptr_t game, uintptr_t entity, const char* name);
/* max_health <= 0 (unknown) falls back to the default player maximum. */
void ui_health_bar(uintptr_t game, uintptr_t entity, int health, int max_health);
#endif
