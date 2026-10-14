/* Checks for asmp-dll/src/multiplayer/menu_view.c: the form's status line and the room
   view of the host and of a guest. The fake text bank returns "#<id>" for every
   string, so the checks see which bank string went where. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../asmp-dll/src/multiplayer/menu_view.h"

#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define HOST_ID  0u
#define GUEST_ID 1u

static void fake_text(const void* context, enum MenuText id, char* out, size_t capacity)
{
    (void)context;
    sprintf_s(out, capacity, "#%d", (int)id);
}

static const TextBank bank = {NULL, fake_text};

/* "#<id>" of one bank string, for comparisons. */
static const char* word(enum MenuText id)
{
    static char buffers[4][16];
    static int next;
    char* out = buffers[next++ % 4];
    sprintf_s(out, 16, "#%d", (int)id);
    return out;
}

static MpLobby make_room(void)
{
    MpLobby room;
    memset(&room, 0, sizeof(room));
    room.room = 5u;
    room.revision = 3u;
    room.settings_revision = 1u;
    room.phase = MP_LOBBY_OPEN;
    room.host_id = HOST_ID;
    room.host_session = 10u;
    room.mode = MP_MODE_CAMPAIGN;
    room.capacity = 4u;
    strcpy_s(room.map, sizeof(room.map), "maps\\Level_03.map");
    room.count = 2u;
    room.entries[0].id = HOST_ID;
    room.entries[0].session = 10u;
    strcpy_s(room.entries[0].name, sizeof(room.entries[0].name), "Sarge");
    room.entries[1].id = GUEST_ID;
    room.entries[1].session = 20u;
    room.entries[1].character = MP_CHARACTER_FEMALE;
    strcpy_s(room.entries[1].name, sizeof(room.entries[1].name), "Kate");
    return room;
}

static LobbyViewInput make_input(const MpLobby* room, uint32_t local_id)
{
    LobbyViewInput input;
    memset(&input, 0, sizeof(input));
    input.lobby = room;
    input.local_id = local_id;
    input.settings_known = map_catalog_parse(room->map, &input.settings);
    input.character = mp_lobby_entry(room, local_id)->character;
    input.available_modes = CATALOG_ALWAYS_AVAILABLE;
    input.address = local_id == HOST_ID ? "192.168.1.34" : "10.0.0.7";
    input.port = 27020u;
    return input;
}

static void check_form(void)
{
    char text[MENU_VIEW_TEXT_MAX];
    menu_view_form_status(SM_CONNECTING, 27020u, 0u, &bank, text, sizeof(text));
    CHECK(!strcmp(text, word(MENU_TEXT_CONNECTING)));
    menu_view_form_status(SM_CONNECTING, 27020u, 600u, &bank, text, sizeof(text));
    CHECK(!strcmp(text, "#0..")); /* The dots grow with time. */
    menu_view_form_status(SM_PORT_IN_USE, 27020u, 0u, &bank, text, sizeof(text));
    CHECK(!strcmp(text, "#8 27020"));
    menu_view_form_status(SM_NO_ANSWER, 27020u, 0u, &bank, text, sizeof(text));
    CHECK(!strcmp(text, word(MENU_TEXT_NO_ANSWER)));
    menu_view_form_status(SM_ENTER_LOBBY, 27020u, 0u, &bank, text, sizeof(text));
    CHECK(!text[0]); /* Codes for the script show no text. */
}

static void check_host(void)
{
    MpLobby room = make_room();
    LobbyViewInput input = make_input(&room, HOST_ID);
    LobbyView view;
    menu_view_lobby(&input, &bank, &view);

    CHECK(view.code == SM_LOBBY_HOST_WAITING && !view.action_enabled);
    CHECK(!strcmp(view.action, word(MENU_TEXT_ACTION_START)));
    CHECK(!strcmp(view.header, "#30 192.168.1.34:27020"));
    CHECK(!view.settings_note[0]);
    CHECK(!strcmp(view.players, "#29 2/4"));
    CHECK(!strcmp(view.rows[0].number, "1") && !strcmp(view.rows[0].name, "Sarge"));
    CHECK(!strcmp(view.rows[0].tag, "#20") && !view.rows[0].waiting[0]);
    CHECK(!strcmp(view.rows[0].detail, "#25 #26")); /* you, male */
    CHECK(!strcmp(view.rows[1].name, "Kate") && !strcmp(view.rows[1].tag, "#22"));
    CHECK(!strcmp(view.rows[1].detail, "#27"));
    /* A free place: its number and the waiting text, nothing else. */
    CHECK(!strcmp(view.rows[2].number, "3") && !strcmp(view.rows[2].waiting, "#28"));
    CHECK(!view.rows[2].name[0] && !view.rows[2].detail[0] && !view.rows[2].tag[0]);
    CHECK(!strcmp(view.status, "#48 Kate"));
    CHECK(!strcmp(view.mode, "#33") && !strcmp(view.mission_caption, "#37"));
    CHECK(!strcmp(view.mission, "3 #39 10"));
    CHECK(!strcmp(view.briefing_file, "Text\\level_03.txt"));
    CHECK(!strcmp(view.hero, "#26"));

    room.entries[1].flags = MP_LOBBY_ENTRY_READY;
    menu_view_lobby(&input, &bank, &view);
    CHECK(view.code == SM_LOBBY_HOST_CAN_START && view.action_enabled);
    CHECK(!strcmp(view.status, word(MENU_TEXT_ALL_READY)));
    CHECK(!strcmp(view.rows[1].tag, "#21"));

    /* Survival: the map caption, one map and a fixed briefing. */
    strcpy_s(room.map, sizeof(room.map), "maps\\survive_01.map");
    input = make_input(&room, HOST_ID);
    menu_view_lobby(&input, &bank, &view);
    CHECK(!strcmp(view.mode, "#36") && !strcmp(view.mission_caption, "#38"));
    CHECK(!strcmp(view.mission, "1 #39 1"));
    CHECK(!view.briefing_file[0] && !strcmp(view.briefing_text, "#40"));

    /* Loading: participants show their load, the button is off. */
    room.phase = MP_LOBBY_STARTING;
    room.run_epoch = 1u;
    room.entries[0].flags = MP_LOBBY_ENTRY_PARTICIPANT | MP_LOBBY_ENTRY_LOADED;
    room.entries[1].flags = MP_LOBBY_ENTRY_PARTICIPANT;
    menu_view_lobby(&input, &bank, &view);
    CHECK(view.code == SM_LOBBY_STARTING && !view.action_enabled);
    CHECK(!strcmp(view.rows[0].tag, "#21") && !strcmp(view.rows[1].tag, "#23"));
    CHECK(!strcmp(view.status, word(MENU_TEXT_STARTING)));
}

static void check_guest(void)
{
    MpLobby room = make_room();
    LobbyViewInput input = make_input(&room, GUEST_ID);
    LobbyView view;
    menu_view_lobby(&input, &bank, &view);

    CHECK(view.code == SM_LOBBY_NOT_READY && view.action_enabled);
    CHECK(!strcmp(view.action, word(MENU_TEXT_ACTION_READY)));
    CHECK(!strcmp(view.header, "#31 Sarge (10.0.0.7:27020)"));
    CHECK(!strcmp(view.settings_note, word(MENU_TEXT_HOST_CHOOSES)));
    CHECK(!strcmp(view.rows[1].detail, "#25 #27")); /* you, female */
    CHECK(!strcmp(view.status, word(MENU_TEXT_GUEST_NOT_READY)));
    CHECK(!strcmp(view.hero, "#27"));

    input.settings_changed = 1;
    menu_view_lobby(&input, &bank, &view);
    CHECK(!strcmp(view.status, word(MENU_TEXT_SETTINGS_CHANGED)));

    room.entries[1].flags = MP_LOBBY_ENTRY_READY;
    menu_view_lobby(&input, &bank, &view);
    CHECK(view.code == SM_LOBBY_READY && view.action_enabled);
    CHECK(!strcmp(view.action, word(MENU_TEXT_ACTION_CANCEL)));
    CHECK(!strcmp(view.status, word(MENU_TEXT_GUEST_READY)));

    /* An add-on mission this copy lacks: no ready, no briefing. */
    room.entries[1].flags = 0;
    strcpy_s(room.map, sizeof(room.map), "maps\\addon\\Level_02.map");
    input = make_input(&room, GUEST_ID);
    menu_view_lobby(&input, &bank, &view);
    CHECK(!view.action_enabled && !strcmp(view.status, word(MENU_TEXT_MISSING_MAPS)));
    CHECK(!view.briefing_file[0]);
    input.available_modes |= CATALOG_MODE_BIT(CATALOG_ADDON);
    menu_view_lobby(&input, &bank, &view);
    CHECK(view.action_enabled && !strcmp(view.briefing_file, "Text\\addon_level_02.txt"));

    /* A cancelled start explains itself. */
    room.notice = MP_LOBBY_NOTICE_LOAD_TIMEOUT;
    menu_view_lobby(&input, &bank, &view);
    CHECK(!strcmp(view.status, word(MENU_TEXT_START_TIMEOUT)));

    /* A late joiner waits for the running game. */
    room.notice = MP_LOBBY_NOTICE_NONE;
    room.phase = MP_LOBBY_IN_GAME;
    room.run_epoch = 1u;
    room.entries[0].flags = MP_LOBBY_ENTRY_PARTICIPANT | MP_LOBBY_ENTRY_LOADED;
    menu_view_lobby(&input, &bank, &view);
    CHECK(view.code == SM_LOBBY_IN_PROGRESS && !view.action_enabled);
    CHECK(!strcmp(view.rows[0].tag, "#24") && !view.rows[1].tag[0]);

    /* A map outside the catalogue is named by its path. */
    strcpy_s(room.map, sizeof(room.map), "maps\\Level_11.map");
    input = make_input(&room, GUEST_ID);
    menu_view_lobby(&input, &bank, &view);
    CHECK(!strcmp(view.mode, "#54") && !strcmp(view.mission, "maps\\Level_11.map"));
}

static void check_briefing(void)
{
    char title[MENU_VIEW_TEXT_MAX];
    char body[MENU_VIEW_BRIEFING_MAX];
    static const char file[] = "MISSION 03: \r\ndisaster!\r\n\r\nDESCRIPTION: \r\n"
                               "first line \r\nsecond line.\r\n\r\nTIP:\r\nnot shown\r\n";
    CHECK(menu_view_split_briefing(file, title, sizeof(title), body, sizeof(body)));
    CHECK(!strcmp(title, "MISSION 03: disaster!"));
    CHECK(!strcmp(body, "DESCRIPTION:\nfirst line\nsecond line."));

    CHECK(menu_view_split_briefing("only a title", title, sizeof(title), body, sizeof(body)));
    CHECK(!strcmp(title, "only a title") && !body[0]);
    CHECK(!menu_view_split_briefing("\r\n\r\n", title, sizeof(title), body, sizeof(body)));
}

int main(void)
{
    check_briefing();
    check_form();
    check_host();
    check_guest();
    puts("Menu view checks passed: form status, host and guest rooms, rows, settings, "
         "briefing split, missing add-ons, notices and late join.");
    return 0;
}
