#ifndef ASMP_STEAM_UI_H
#define ASMP_STEAM_UI_H
#include "steam_actor.h"
/* Version-specific native UI and map bindings. All calls are game-thread only. */
int steam_ui_bind(uintptr_t base);
void steam_ui_stop(void);
uintptr_t steam_ui_menu_item(uintptr_t game, unsigned int vid, unsigned int direction);
int steam_ui_text(uintptr_t entity, char* out, unsigned int capacity);
void steam_ui_menu_status(uintptr_t entity, int status);
void steam_ui_button(uintptr_t entity, int disabled);
int steam_ui_load_map(uintptr_t game, const char* map);
int steam_ui_name(SteamActor* actor, const char* name);
void steam_ui_health_bar(uintptr_t game, uintptr_t entity, int health);
#endif
