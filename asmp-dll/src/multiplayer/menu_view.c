/* Builds the texts of the multiplayer menus: the form's status line and the room
   menu (header, player rows, settings panel, briefing, status line, action button).
   Russian words come from the menu's text bank; this module only adds ASCII names,
   numbers and addresses. Layer: multiplayer/ (session); pure, no native calls. */

#include <stdio.h>
#include <string.h>
#include "menu_view.h"

#define CONNECTING_DOT_MS 300u
#define CONNECTING_DOTS   4u

/* Appends `text`, cutting it when the buffer is full. */
static void append(char* out, size_t capacity, const char* text)
{
    strncat_s(out, capacity, text, _TRUNCATE);
}

/* Appends bank string `id`. */
static void append_bank(char* out, size_t capacity, const TextBank* bank, enum MenuText id)
{
    char word[MENU_VIEW_TEXT_MAX];
    word[0] = 0;
    bank->text(bank->context, id, word, sizeof(word));
    append(out, capacity, word);
}

/* Replaces `out` with bank string `id`. */
static void set_bank(char* out, size_t capacity, const TextBank* bank, enum MenuText id)
{
    out[0] = 0;
    append_bank(out, capacity, bank, id);
}

static void append_number(char* out, size_t capacity, unsigned int value)
{
    char digits[16];
    sprintf_s(digits, sizeof(digits), "%u", value);
    append(out, capacity, digits);
}

int menu_view_is_host(const MpLobby* lobby, uint32_t local_id)
{
    const MpLobbyEntry* entry = mp_lobby_entry(lobby, local_id);
    return entry && entry->id == lobby->host_id && entry->session == lobby->host_session;
}

/* Bank string of a failure or progress code shown on the form, or -1. */
static int form_text(enum MenuStatus code)
{
    switch (code)
    {
    case SM_CONNECTING:
        return MENU_TEXT_CONNECTING;
    case SM_BAD_NICKNAME:
        return MENU_TEXT_BAD_NAME;
    case SM_BAD_ADDRESS:
        return MENU_TEXT_BAD_ADDRESS;
    case SM_FAILED:
        return MENU_TEXT_FAILED;
    case SM_CONNECTION_LOST:
        return MENU_TEXT_LOST;
    case SM_NO_ANSWER:
        return MENU_TEXT_NO_ANSWER;
    case SM_INCOMPATIBLE:
        return MENU_TEXT_INCOMPATIBLE;
    case SM_SERVER_MISSING:
        return MENU_TEXT_SERVER_MISSING;
    case SM_PORT_IN_USE:
        return MENU_TEXT_PORT_IN_USE;
    case SM_HOST_LEFT:
        return MENU_TEXT_HOST_LEFT;
    case SM_SERVER_FAILED:
        return MENU_TEXT_SERVER_FAILED;
    default:
        return -1;
    }
}

void menu_view_form_status(enum MenuStatus code, unsigned int port, DWORD now, const TextBank* bank,
                           char* out, size_t capacity)
{
    out[0] = 0;
    int id = form_text(code);
    if (id < 0)
        return;

    append_bank(out, capacity, bank, (enum MenuText)id);
    if (code == SM_CONNECTING)
    {
        unsigned int dots = (now / CONNECTING_DOT_MS) % CONNECTING_DOTS;
        for (unsigned int i = 0; i < dots; ++i)
            append(out, capacity, ".");
    }
    else if (code == SM_PORT_IN_USE)
    {
        append(out, capacity, " ");
        append_number(out, capacity, port);
    }
}

/* True when every member other than the host is ready. */
static int guests_ready(const MpLobby* lobby)
{
    for (unsigned int i = 0; i < lobby->count; ++i)
    {
        const MpLobbyEntry* entry = &lobby->entries[i];
        if (entry->id == lobby->host_id)
            continue;
        if (!(entry->flags & MP_LOBBY_ENTRY_READY))
            return 0;
    }
    return 1;
}

/* The room code the script reacts to; the same rules decide the action button. */
static enum MenuStatus room_code(const LobbyViewInput* input)
{
    const MpLobby* lobby = input->lobby;
    const MpLobbyEntry* entry = mp_lobby_entry(lobby, input->local_id);
    int participant = entry && (entry->flags & MP_LOBBY_ENTRY_PARTICIPANT);

    if (lobby->phase == MP_LOBBY_IN_GAME || (lobby->phase == MP_LOBBY_STARTING && !participant))
        return SM_LOBBY_IN_PROGRESS;
    if (lobby->phase == MP_LOBBY_STARTING)
        return SM_LOBBY_STARTING;
    if (menu_view_is_host(lobby, input->local_id))
        return guests_ready(lobby) ? SM_LOBBY_HOST_CAN_START : SM_LOBBY_HOST_WAITING;
    if (entry && (entry->flags & MP_LOBBY_ENTRY_READY))
        return SM_LOBBY_READY;
    return SM_LOBBY_NOT_READY;
}

/* A guest cannot play a mode whose maps this copy of the game does not have. */
static int maps_missing(const LobbyViewInput* input)
{
    if (!input->settings_known)
        return 0;
    return !(input->available_modes & CATALOG_MODE_BIT(input->settings.mode));
}

static enum MenuText character_text(uint32_t character)
{
    return character == MP_CHARACTER_FEMALE ? MENU_TEXT_FEMALE : MENU_TEXT_MALE;
}

/* The tag at the right of a player row. */
static int row_tag(const MpLobby* lobby, const MpLobbyEntry* entry)
{
    int participant = (entry->flags & MP_LOBBY_ENTRY_PARTICIPANT) != 0;

    if (lobby->phase == MP_LOBBY_STARTING && participant)
        return (entry->flags & MP_LOBBY_ENTRY_LOADED) ? MENU_TEXT_TAG_READY : MENU_TEXT_TAG_LOADING;
    if (lobby->phase == MP_LOBBY_IN_GAME && participant)
        return MENU_TEXT_TAG_PLAYING;
    if (lobby->phase != MP_LOBBY_OPEN)
        return -1;
    if (entry->id == lobby->host_id && entry->session == lobby->host_session)
        return MENU_TEXT_TAG_HOST;
    return (entry->flags & MP_LOBBY_ENTRY_READY) ? MENU_TEXT_TAG_READY : MENU_TEXT_TAG_NOT_READY;
}

static void build_member_row(const LobbyViewInput* input, unsigned int index, const TextBank* bank,
                             LobbyRowView* row)
{
    const MpLobbyEntry* entry = &input->lobby->entries[index];

    strcpy_s(row->name, sizeof(row->name), entry->name);

    int tag = row_tag(input->lobby, entry);
    if (tag >= 0)
        set_bank(row->tag, sizeof(row->tag), bank, (enum MenuText)tag);

    if (entry->id == input->local_id)
    {
        append_bank(row->detail, sizeof(row->detail), bank, MENU_TEXT_YOU);
        append(row->detail, sizeof(row->detail), " ");
    }
    append_bank(row->detail, sizeof(row->detail), bank, character_text(entry->character));
}

/* Members first in slot order, then free places up to the room's capacity. Every
   place of the room gets its number; rows beyond the capacity stay empty. */
static void build_rows(const LobbyViewInput* input, const TextBank* bank, LobbyView* out)
{
    const MpLobby* lobby = input->lobby;
    for (unsigned int i = 0; i < MENU_ROWS; ++i)
    {
        LobbyRowView* row = &out->rows[i];
        if (i >= lobby->capacity)
            continue;
        append_number(row->number, sizeof(row->number), i + 1u);
        if (i < lobby->count)
            build_member_row(input, i, bank, row);
        else
            set_bank(row->waiting, sizeof(row->waiting), bank, MENU_TEXT_WAITING_PLAYER);
    }

    set_bank(out->players, sizeof(out->players), bank, MENU_TEXT_PLAYERS);
    append(out->players, sizeof(out->players), " ");
    append_number(out->players, sizeof(out->players), lobby->count);
    append(out->players, sizeof(out->players), "/");
    append_number(out->players, sizeof(out->players), lobby->capacity);
}

static void append_address(char* out, size_t capacity, const LobbyViewInput* input)
{
    if (input->address && input->address[0])
        append(out, capacity, input->address);
    append(out, capacity, ":");
    append_number(out, capacity, input->port);
}

/* Host: the address friends type. Guest: whose room this is and where. */
static void build_header(const LobbyViewInput* input, const TextBank* bank, LobbyView* out)
{
    const MpLobby* lobby = input->lobby;
    if (menu_view_is_host(lobby, input->local_id))
    {
        set_bank(out->header, sizeof(out->header), bank, MENU_TEXT_ADDRESS_FOR_FRIENDS);
        append(out->header, sizeof(out->header), " ");
        append_address(out->header, sizeof(out->header), input);
        return;
    }

    set_bank(out->header, sizeof(out->header), bank, MENU_TEXT_ROOM_OF);
    const MpLobbyEntry* host = mp_lobby_entry(lobby, lobby->host_id);
    if (host)
    {
        append(out->header, sizeof(out->header), " ");
        append(out->header, sizeof(out->header), host->name);
    }
    append(out->header, sizeof(out->header), " (");
    append_address(out->header, sizeof(out->header), input);
    append(out->header, sizeof(out->header), ")");
    set_bank(out->settings_note, sizeof(out->settings_note), bank, MENU_TEXT_HOST_CHOOSES);
}

/* Mode, mission and briefing. A map outside the catalogue is shown by its file name. */
static void build_settings(const LobbyViewInput* input, const TextBank* bank, LobbyView* out)
{
    if (!input->settings_known)
    {
        set_bank(out->mode, sizeof(out->mode), bank, MENU_TEXT_UNKNOWN_MAP);
        set_bank(out->mission_caption, sizeof(out->mission_caption), bank, MENU_TEXT_MAP);
        append(out->mission, sizeof(out->mission), input->lobby->map);
        return;
    }

    const MapChoice* choice = &input->settings;
    int survival = choice->mode == CATALOG_SURVIVAL;
    set_bank(out->mode, sizeof(out->mode), bank,
             (enum MenuText)(MENU_TEXT_MODE_CAMPAIGN + choice->mode));
    set_bank(out->mission_caption, sizeof(out->mission_caption), bank,
             survival ? MENU_TEXT_MAP : MENU_TEXT_MISSION);

    append_number(out->mission, sizeof(out->mission), choice->mission);
    append(out->mission, sizeof(out->mission), " ");
    append_bank(out->mission, sizeof(out->mission), bank, MENU_TEXT_OF);
    append(out->mission, sizeof(out->mission), " ");
    append_number(out->mission, sizeof(out->mission), map_catalog_missions(choice->mode));

    if (maps_missing(input))
        return; /* No briefing for maps this copy does not have. */
    if (!map_catalog_text_path(choice, out->briefing_file, sizeof(out->briefing_file)))
    {
        out->briefing_file[0] = 0;
        set_bank(out->briefing_text, sizeof(out->briefing_text), bank, MENU_TEXT_SURVIVAL_BRIEFING);
    }
}

/* "Waiting for: A, B" with the names of the guests that are not ready. */
static void build_waiting_status(const MpLobby* lobby, const TextBank* bank, LobbyView* out)
{
    set_bank(out->status, sizeof(out->status), bank, MENU_TEXT_WAITING_FOR);
    const char* separator = " ";
    for (unsigned int i = 0; i < lobby->count; ++i)
    {
        const MpLobbyEntry* entry = &lobby->entries[i];
        if (entry->id == lobby->host_id || (entry->flags & MP_LOBBY_ENTRY_READY))
            continue;
        append(out->status, sizeof(out->status), separator);
        append(out->status, sizeof(out->status), entry->name);
        separator = ", ";
    }
}

/* The status line for an open room seen by a guest. */
static enum MenuText guest_status(const LobbyViewInput* input)
{
    if (input->lobby->notice == MP_LOBBY_NOTICE_LOAD_TIMEOUT)
        return MENU_TEXT_START_TIMEOUT;
    if (input->lobby->notice == MP_LOBBY_NOTICE_PLAYER_LEFT)
        return MENU_TEXT_START_PLAYER_LEFT;
    if (maps_missing(input))
        return MENU_TEXT_MISSING_MAPS;
    if (input->settings_changed)
        return MENU_TEXT_SETTINGS_CHANGED;
    return MENU_TEXT_GUEST_NOT_READY;
}

static void build_status(const LobbyViewInput* input, const TextBank* bank, LobbyView* out)
{
    const MpLobby* lobby = input->lobby;
    switch (out->code)
    {
    case SM_LOBBY_IN_PROGRESS:
        set_bank(out->status, sizeof(out->status), bank, MENU_TEXT_IN_PROGRESS);
        break;
    case SM_LOBBY_STARTING:
        set_bank(out->status, sizeof(out->status), bank, MENU_TEXT_STARTING);
        break;
    case SM_LOBBY_HOST_WAITING:
        build_waiting_status(lobby, bank, out);
        break;
    case SM_LOBBY_HOST_CAN_START:
        set_bank(out->status, sizeof(out->status), bank, MENU_TEXT_ALL_READY);
        break;
    case SM_LOBBY_READY:
        set_bank(out->status, sizeof(out->status), bank, MENU_TEXT_GUEST_READY);
        break;
    default:
        set_bank(out->status, sizeof(out->status), bank, guest_status(input));
        break;
    }
}

/* Host: start, enabled once everyone is ready. Guest: ready or cancel. */
static void build_action(const LobbyViewInput* input, const TextBank* bank, LobbyView* out)
{
    int host = menu_view_is_host(input->lobby, input->local_id);
    enum MenuText label = MENU_TEXT_ACTION_READY;
    if (host)
        label = MENU_TEXT_ACTION_START;
    else if (out->code == SM_LOBBY_READY)
        label = MENU_TEXT_ACTION_CANCEL;
    set_bank(out->action, sizeof(out->action), bank, label);

    if (host)
        out->action_enabled = out->code == SM_LOBBY_HOST_CAN_START;
    else if (out->code == SM_LOBBY_READY)
        out->action_enabled = 1;
    else
        out->action_enabled = out->code == SM_LOBBY_NOT_READY && !maps_missing(input);
}

/* Copies the line at `text` without its line break into `out`; returns where the
   next line starts. Trailing spaces and carriage returns are dropped. */
static const char* read_line(const char* text, char* out, size_t capacity)
{
    size_t length = 0;
    while (text[length] && text[length] != '\n')
        ++length;
    const char* next = text[length] ? text + length + 1 : text + length;

    while (length && (text[length - 1] == '\r' || text[length - 1] == ' '))
        --length;
    if (length >= capacity)
        length = capacity - 1;
    memcpy(out, text, length);
    out[length] = 0;
    return next;
}

int menu_view_split_briefing(const char* text, char* title, size_t title_capacity, char* body,
                             size_t body_capacity)
{
    title[0] = 0;
    body[0] = 0;
    int paragraph = 0; /* 1 title, 2 description, 3 and later ignored */
    int in_paragraph = 0;
    char line[MENU_VIEW_TEXT_MAX];

    while (*text && paragraph <= 2)
    {
        text = read_line(text, line, sizeof(line));
        if (!line[0])
        {
            in_paragraph = 0;
            continue;
        }
        if (!in_paragraph)
        {
            in_paragraph = 1;
            ++paragraph;
        }
        if (paragraph == 1)
        {
            if (title[0])
                append(title, title_capacity, " ");
            append(title, title_capacity, line);
        }
        else if (paragraph == 2)
        {
            if (body[0])
                append(body, body_capacity, "\n");
            append(body, body_capacity, line);
        }
    }
    return paragraph > 0;
}

void menu_view_lobby(const LobbyViewInput* input, const TextBank* bank, LobbyView* out)
{
    memset(out, 0, sizeof(*out));
    out->code = room_code(input);

    build_header(input, bank, out);
    build_rows(input, bank, out);
    set_bank(out->hero, sizeof(out->hero), bank, character_text(input->character));
    build_settings(input, bank, out);
    build_status(input, bank, out);
    build_action(input, bank, out);
}
