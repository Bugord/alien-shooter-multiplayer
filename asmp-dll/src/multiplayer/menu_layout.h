#ifndef ASMP_MENU_LAYOUT_H
#define ASMP_MENU_LAYOUT_H

/* Where the DLL finds the texts of the multiplayer menus (asmp_play.men and
   asmp_lobby.men): a menu text is found by its font VID and the raw direction byte of
   its record. analysis/build_asmp_menus.py reads the MENU_* defines of this file, so
   the generated menus and the DLL always agree. Layer: multiplayer/ (session).

   Every visible string lives in the menus, in the game's cp1251, so the DLL never
   holds Russian text. Strings the DLL composes come from the menu's text bank:
   off-screen caption texts at MENU_BANK_DIR + enum MenuText. */

/* Font VIDs of menu texts. */
#define MENU_VID_STATUS  2 /* Status line; its health field carries enum MenuStatus. */
#define MENU_VID_LABEL   3 /* Button label font (red). */
#define MENU_VID_CAPTION 4 /* Caption font (pale). */

/* Both menus: the status entity (VID 2, its health carries the code) and the status
   line itself. The status font has no Cyrillic letters (live check), so the line is
   a caption text and the status entity stays empty. */
#define MENU_DIR_STATUS      0
#define MENU_DIR_STATUS_TEXT 13 /* VID 4 */

/* Room menu texts the DLL fills. The action label also tells the room menu apart
   from the form (the script never looks it up). */
#define MENU_DIR_ACTION_LABEL    5  /* VID 3 */
#define MENU_DIR_HEADER          10 /* VID 4: address for friends / whose room */
#define MENU_DIR_SETTINGS_NOTE   11 /* VID 4: "chosen by the host" for guests */
#define MENU_DIR_PLAYERS_CAPTION 12 /* VID 4: "players 2/4" */
#define MENU_DIR_ROW_NAME        20 /* VID 3, + row: the player's name */
#define MENU_DIR_ROW_TAG         24 /* VID 4, + row: host / ready / not ready */
#define MENU_DIR_ROW_DETAIL      28 /* VID 4, + row: "you - male" under the name */
#define MENU_DIR_ROW_NUMBER      40 /* VID 4, + row: the slot number */
#define MENU_DIR_ROW_WAITING     44 /* VID 4, + row: "waiting for a player" in a free row */
#define MENU_DIR_HERO_VALUE      32 /* VID 3 */
#define MENU_DIR_MODE_VALUE      33 /* VID 3 */
#define MENU_DIR_MISSION_CAPTION 34 /* VID 4: "mission:" or "map:" */
#define MENU_DIR_MISSION_VALUE   35 /* VID 3: "3 of 10" */
#define MENU_DIR_BRIEFING        36 /* VID 4: the description paragraph of the briefing */
#define MENU_DIR_MISSION_TITLE   38 /* VID 4: "mission 03: title" from the briefing */
#define MENU_DIR_BRIEFING_SOURCE 39 /* VID 4, off screen: the whole Text\*.txt file */
#define MENU_ROWS                4

/* Text bank: VID 4 records at MENU_BANK_DIR + id, placed off screen. */
#define MENU_BANK_VID MENU_VID_CAPTION
#define MENU_BANK_DIR 100

/* Buttons (VID 705) and selector arrows (VID 716) by ndir, as the script sees them. */
#define MENU_BUTTON_NDIR_MAIN   1 /* Form: connect. Room: start / ready / cancel. */
#define MENU_BUTTON_NDIR_BACK   2 /* Main menu; in the room it leaves. */
#define MENU_BUTTON_NDIR_HOST   3 /* Form: create a room. */
#define MENU_ARROW_NDIR_MODE    0 /* + 0 previous, + 1 next */
#define MENU_ARROW_NDIR_MISSION 2
#define MENU_ARROW_NDIR_HERO    4

/* Ids of the bank strings. The generator writes the Russian text of each one. */
enum MenuText
{
    MENU_TEXT_CONNECTING = 0,
    MENU_TEXT_BAD_NAME = 1,
    MENU_TEXT_BAD_ADDRESS = 2,
    MENU_TEXT_FAILED = 3,
    MENU_TEXT_LOST = 4,
    MENU_TEXT_NO_ANSWER = 5,
    MENU_TEXT_INCOMPATIBLE = 6,
    MENU_TEXT_SERVER_MISSING = 7,
    MENU_TEXT_PORT_IN_USE = 8,
    MENU_TEXT_HOST_LEFT = 9,
    MENU_TEXT_SERVER_FAILED = 10,

    MENU_TEXT_TAG_HOST = 20,
    MENU_TEXT_TAG_READY = 21,
    MENU_TEXT_TAG_NOT_READY = 22,
    MENU_TEXT_TAG_LOADING = 23,
    MENU_TEXT_TAG_PLAYING = 24,
    MENU_TEXT_YOU = 25,
    MENU_TEXT_MALE = 26,
    MENU_TEXT_FEMALE = 27,
    MENU_TEXT_WAITING_PLAYER = 28,
    MENU_TEXT_PLAYERS = 29,
    MENU_TEXT_ADDRESS_FOR_FRIENDS = 30,
    MENU_TEXT_ROOM_OF = 31,
    MENU_TEXT_HOST_CHOOSES = 32,
    MENU_TEXT_MODE_CAMPAIGN = 33, /* + enum CatalogMode */
    MENU_TEXT_MISSION = 37,
    MENU_TEXT_MAP = 38,
    MENU_TEXT_OF = 39,
    MENU_TEXT_SURVIVAL_BRIEFING = 40,
    MENU_TEXT_ACTION_START = 41,
    MENU_TEXT_ACTION_READY = 42,
    MENU_TEXT_ACTION_CANCEL = 43,
    MENU_TEXT_GUEST_NOT_READY = 44,
    MENU_TEXT_GUEST_READY = 45,
    MENU_TEXT_SETTINGS_CHANGED = 46,
    MENU_TEXT_MISSING_MAPS = 47,
    MENU_TEXT_WAITING_FOR = 48,
    MENU_TEXT_ALL_READY = 49,
    MENU_TEXT_STARTING = 50,
    MENU_TEXT_IN_PROGRESS = 51,
    MENU_TEXT_START_TIMEOUT = 52,
    MENU_TEXT_START_PLAYER_LEFT = 53,
    MENU_TEXT_UNKNOWN_MAP = 54,
    MENU_TEXT_COUNT = 55
};

#endif
