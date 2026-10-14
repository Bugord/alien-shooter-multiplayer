#ifndef ASMP_MENU_VIEW_H
#define ASMP_MENU_VIEW_H

/* What the multiplayer menus show: the form's status line and every text of the room
   menu, built from the room snapshot and the strings of the menu's text bank. Layer:
   multiplayer/ (session); pure, no native calls, so tests drive it directly. The
   session applies the result to the menu entities (session.c). */

#include <windows.h>
#include <stddef.h>
#include "session.h"
#include "map_catalog.h"
#include "menu_layout.h"

#define MENU_VIEW_TEXT_MAX     128
#define MENU_VIEW_BRIEFING_MAX 512
#define MENU_VIEW_PATH_MAX     64
#define MENU_VIEW_FILE_MAX     4096 /* A whole briefing file (Text\*.txt is under 1 KB). */

/* Reads bank strings of the current menu. `text` copies string `id` into `out`
   (cp1251, terminated) and leaves "" when the menu has no such string. */
typedef struct TextBank
{
    const void* context;
    void (*text)(const void* context, enum MenuText id, char* out, size_t capacity);
} TextBank;

typedef struct LobbyViewInput
{
    const MpLobby* lobby;
    uint32_t local_id;
    MapChoice settings;           /* What the settings panel shows. */
    int settings_known;           /* 0: the room's map is not in the catalogue. */
    uint32_t character;           /* This player's character (enum MpCharacter). */
    unsigned int available_modes; /* CATALOG_MODE_BIT of the modes this copy has. */
    int settings_changed;         /* Guest: the host changed the settings after our ready. */
    const char* address;          /* Host: own LAN address; guest: the server's address. */
    unsigned int port;
} LobbyViewInput;

typedef struct LobbyRowView
{
    char number[MENU_VIEW_TEXT_MAX];
    char waiting[MENU_VIEW_TEXT_MAX];
    char name[MENU_VIEW_TEXT_MAX];
    char tag[MENU_VIEW_TEXT_MAX];
    char detail[MENU_VIEW_TEXT_MAX];
} LobbyRowView;

typedef struct LobbyView
{
    enum MenuStatus code; /* Room code for the script (SM_LOBBY_*). */
    int action_enabled;
    char action[MENU_VIEW_TEXT_MAX];
    char status[MENU_VIEW_TEXT_MAX];
    char header[MENU_VIEW_TEXT_MAX];
    char settings_note[MENU_VIEW_TEXT_MAX];
    char players[MENU_VIEW_TEXT_MAX];
    LobbyRowView rows[MENU_ROWS];
    char hero[MENU_VIEW_TEXT_MAX];
    char mode[MENU_VIEW_TEXT_MAX];
    char mission_caption[MENU_VIEW_TEXT_MAX];
    char mission[MENU_VIEW_TEXT_MAX];
    char briefing_file[MENU_VIEW_PATH_MAX]; /* "" when briefing_text is shown instead. */
    char briefing_text[MENU_VIEW_BRIEFING_MAX];
} LobbyView;

/* 1 when `local_id` is the room's host. */
int menu_view_is_host(const MpLobby* lobby, uint32_t local_id);

/* The form's status line for `code`; "" for codes without a text. `now` animates the
   dots while connecting; `port` names the port that is in use. */
void menu_view_form_status(enum MenuStatus code, unsigned int port, DWORD now, const TextBank* bank,
                           char* out, size_t capacity);

void menu_view_lobby(const LobbyViewInput* input, const TextBank* bank, LobbyView* out);

/* Splits a mission briefing (Text\level_NN.txt) into its title and its description.
   The files are paragraphs separated by blank lines: the mission number and title,
   then the description, then a tip. The title's lines are joined with spaces; the
   description keeps its lines. The tip is left out: it does not fit the room menu.
   Returns 0 when the text has no paragraph. */
int menu_view_split_briefing(const char* text, char* title, size_t title_capacity, char* body,
                             size_t body_capacity);

#endif
