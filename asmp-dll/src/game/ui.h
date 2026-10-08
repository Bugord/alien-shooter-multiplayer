#ifndef ASMP_UI_H
#define ASMP_UI_H
#include "actor.h"
/* Version-specific native UI and map bindings. All calls are game-thread only. */
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
