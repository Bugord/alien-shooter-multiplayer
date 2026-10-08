#ifndef ASMP_UI_H
#define ASMP_UI_H
#include "actor.h"
/* Version-specific native UI and map bindings. All calls are game-thread only.
   Ownership of native objects this layer touches:
   - Name text: a native STEXT created by ui_name is a child of the replica MAN
     and in the world list; the MAN destructor (actor_remove) destroys it. Its
     menu-list reference is dropped at creation. The text string is allocated
     by the game's own string assign and freed with the STEXT.
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
int ui_name(Actor* actor, const char* name);
void ui_health_bar(uintptr_t game, uintptr_t entity, int health);
#endif
