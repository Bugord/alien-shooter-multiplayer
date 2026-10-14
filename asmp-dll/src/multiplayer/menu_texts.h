#ifndef ASMP_MENU_TEXTS_H
#define ASMP_MENU_TEXTS_H

/* Puts menu views (menu_view.h) into the native menu texts and reads the menu's text
   bank. Layer: multiplayer/ (session); game thread only, inside the session's SEH
   frame. Menu entities belong to the menu map: they are looked up on every call and
   never kept, except as an identity to tell whether a briefing must be reloaded. */

#include "session.h"
#include "menu_view.h"

/* What was last pushed into entities whose native update is not idempotent. */
typedef struct MenuTextCache
{
    uintptr_t briefing_entity;
    char briefing_file[MENU_VIEW_PATH_MAX];
    uintptr_t action_button;
    int action_disabled;
} MenuTextCache;

/* Reads bank strings from the menu currently loaded in `game`. */
typedef struct MenuBankContext
{
    const SessionEngine* engine;
    uintptr_t game;
} MenuBankContext;

TextBank menu_texts_bank(const MenuBankContext* context);

/* Sets the text of the menu text (vid, dir) unless it already shows `text`. */
void menu_texts_set(const SessionEngine* engine, uintptr_t game, unsigned int vid, unsigned int dir,
                    const char* text);

/* Shows the whole room view. `button` is the action button (VID 705, ndir 1). */
void menu_texts_show_lobby(const SessionEngine* engine, uintptr_t game, uintptr_t button,
                           const LobbyView* view, MenuTextCache* cache);

#endif
