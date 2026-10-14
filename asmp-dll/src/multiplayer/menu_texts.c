/* Applies the views of the multiplayer menus to the native menu texts: each text is
   found by font VID and direction byte (menu_layout.h) and changed only when it shows
   something else, so the native text action runs once per change. Layer:
   multiplayer/ (session); game thread only. */

#include <string.h>
#include "menu_texts.h"

/* TextBank callback: copies the bank text `id` of the current menu, or "". */
static void read_bank(const void* context, enum MenuText id, char* out, size_t capacity)
{
    const MenuBankContext* bank = context;
    out[0] = 0;
    uintptr_t entity =
        bank->engine->menu_item(bank->game, MENU_BANK_VID, MENU_BANK_DIR + (unsigned int)id);
    if (!entity || !bank->engine->text(entity, out, (unsigned int)capacity))
        out[0] = 0;
}

TextBank menu_texts_bank(const MenuBankContext* context)
{
    TextBank bank = {context, read_bank};
    return bank;
}

void menu_texts_set(const SessionEngine* engine, uintptr_t game, unsigned int vid, unsigned int dir,
                    const char* text)
{
    if (!engine->set_text)
        return;
    uintptr_t entity = engine->menu_item(game, vid, dir);
    if (!entity)
        return;

    char shown[MENU_VIEW_BRIEFING_MAX];
    if (engine->text(entity, shown, sizeof(shown)) && !strcmp(shown, text))
        return;
    engine->set_text(entity, text);
}

/* Loads the briefing file into the hidden source text once per entity and file; the
   native action reads the file again on every call. A source that reads empty was
   never loaded (the menu record holds ""). Returns 1 when the source holds the file. */
static int load_briefing_source(const SessionEngine* engine, uintptr_t source, const char* file,
                                MenuTextCache* cache)
{
    char shown[2];
    int empty = !engine->text(source, shown, sizeof(shown)) || !shown[0];
    int same = source == cache->briefing_entity && !strcmp(cache->briefing_file, file);
    if (same && !empty)
        return 1;
    if (!engine->set_text_file(source, file))
        return 0;
    cache->briefing_entity = source;
    strcpy_s(cache->briefing_file, sizeof(cache->briefing_file), file);
    return 1;
}

/* Mission title and description. A mission's come from its Text\*.txt file, which the
   game loads into a hidden text; survival shows a fixed text from the bank. */
static void show_briefing(const SessionEngine* engine, uintptr_t game, const LobbyView* view,
                          MenuTextCache* cache)
{
    static char file_text[MENU_VIEW_FILE_MAX];
    char title[MENU_VIEW_TEXT_MAX];
    char body[MENU_VIEW_BRIEFING_MAX];

    title[0] = 0;
    strcpy_s(body, sizeof(body), view->briefing_text);
    if (view->briefing_file[0])
    {
        uintptr_t source = engine->menu_item(game, MENU_VID_CAPTION, MENU_DIR_BRIEFING_SOURCE);
        if (!source || !engine->set_text_file)
            return;
        if (!load_briefing_source(engine, source, view->briefing_file, cache))
            return;
        if (!engine->text(source, file_text, sizeof(file_text)))
            return;
        menu_view_split_briefing(file_text, title, sizeof(title), body, sizeof(body));
    }
    else
    {
        cache->briefing_file[0] = 0;
    }
    menu_texts_set(engine, game, MENU_VID_CAPTION, MENU_DIR_MISSION_TITLE, title);
    menu_texts_set(engine, game, MENU_VID_CAPTION, MENU_DIR_BRIEFING, body);
}

/* Greys the action button out while it does nothing. The native animation restarts
   on every call, so it is set only when the state or the button changes. */
static void show_action_button(const SessionEngine* engine, uintptr_t button, int disabled,
                               MenuTextCache* cache)
{
    if (!button)
        return;
    if (button == cache->action_button && disabled == cache->action_disabled)
        return;
    engine->button(button, disabled);
    cache->action_button = button;
    cache->action_disabled = disabled;
}

static void show_rows(const SessionEngine* engine, uintptr_t game, const LobbyView* view)
{
    for (unsigned int i = 0; i < MENU_ROWS; ++i)
    {
        const LobbyRowView* row = &view->rows[i];
        menu_texts_set(engine, game, MENU_VID_LABEL, MENU_DIR_ROW_NAME + i, row->name);
        menu_texts_set(engine, game, MENU_VID_CAPTION, MENU_DIR_ROW_TAG + i, row->tag);
        menu_texts_set(engine, game, MENU_VID_CAPTION, MENU_DIR_ROW_DETAIL + i, row->detail);
        menu_texts_set(engine, game, MENU_VID_CAPTION, MENU_DIR_ROW_NUMBER + i, row->number);
        menu_texts_set(engine, game, MENU_VID_CAPTION, MENU_DIR_ROW_WAITING + i, row->waiting);
    }
}

void menu_texts_show_lobby(const SessionEngine* engine, uintptr_t game, uintptr_t button,
                           const LobbyView* view, MenuTextCache* cache)
{
    menu_texts_set(engine, game, MENU_VID_CAPTION, MENU_DIR_STATUS_TEXT, view->status);
    menu_texts_set(engine, game, MENU_VID_LABEL, MENU_DIR_ACTION_LABEL, view->action);
    menu_texts_set(engine, game, MENU_VID_CAPTION, MENU_DIR_HEADER, view->header);
    menu_texts_set(engine, game, MENU_VID_CAPTION, MENU_DIR_SETTINGS_NOTE, view->settings_note);
    menu_texts_set(engine, game, MENU_VID_CAPTION, MENU_DIR_PLAYERS_CAPTION, view->players);
    show_rows(engine, game, view);
    menu_texts_set(engine, game, MENU_VID_LABEL, MENU_DIR_HERO_VALUE, view->hero);
    menu_texts_set(engine, game, MENU_VID_LABEL, MENU_DIR_MODE_VALUE, view->mode);
    menu_texts_set(engine, game, MENU_VID_CAPTION, MENU_DIR_MISSION_CAPTION, view->mission_caption);
    menu_texts_set(engine, game, MENU_VID_LABEL, MENU_DIR_MISSION_VALUE, view->mission);
    show_briefing(engine, game, view, cache);
    show_action_button(engine, button, !view->action_enabled, cache);
}
