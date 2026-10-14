/* Repeats ready, start, loaded, settings and character requests until the room
   snapshot reflects them. Layer: multiplayer/ (session orchestration). Worker thread
   only; it holds no connection, the caller's LobbySender sends. */

#include <string.h>
#include "lobby_requests.h"

/* True when nothing was sent yet or the last request is old enough to send again. */
static int due(const RequestTimer* timer, DWORD now)
{
    return !timer->sent || now - timer->sent_at >= LOBBY_REQUEST_REPEAT_MS;
}

static void mark_sent(RequestTimer* timer, DWORD now)
{
    timer->sent = 1;
    timer->sent_at = now;
}

static int is_host(const MpLobby* lobby, const MpLobbyEntry* entry)
{
    return entry->id == lobby->host_id && entry->session == lobby->host_session;
}

/* Guest only: sends the wanted ready value while the room still shows the other one
   for the same settings. */
static int step_ready(LobbyRequests* requests, const LobbyIntent* intent, const MpLobby* lobby,
                      const MpLobbyEntry* entry, DWORD now, const LobbySender* sender)
{
    if (!intent->ready_wanted || lobby->phase != MP_LOBBY_OPEN || is_host(lobby, entry))
        return 0;
    if (intent->ready_settings_revision != lobby->settings_revision)
        return 0; /* The settings changed; the player must agree again. */

    uint32_t shown = (entry->flags & MP_LOBBY_ENTRY_READY) ? 1u : 0u;
    if (shown == intent->ready_value || !due(&requests->ready, now))
        return 0;

    MpLobbyReady request = {lobby->room, lobby->settings_revision, intent->ready_value};
    if (!sender->ready(sender->context, &request))
        return 0;
    mark_sent(&requests->ready, now);
    return 1;
}

/* Host only: repeats the start while the room is open at the revision it was asked for.
   A newer revision means the room changed after the click; the start is dropped. */
static int step_start(LobbyRequests* requests, const LobbyIntent* intent, const MpLobby* lobby,
                      const MpLobbyEntry* entry, DWORD now, const LobbySender* sender)
{
    if (!intent->start_wanted || lobby->phase != MP_LOBBY_OPEN || !is_host(lobby, entry))
        return 0;
    if (intent->start_room != lobby->room || intent->start_revision != lobby->revision)
        return 0;
    if (!due(&requests->start, now))
        return 0;

    MpLobbyStart request = {lobby->room, lobby->revision};
    if (!sender->start(sender->context, &request))
        return 0;
    mark_sent(&requests->start, now);
    return 1;
}

/* Participant only: reports its load until the snapshot marks it loaded. */
static int step_loaded(LobbyRequests* requests, const LobbyIntent* intent, const MpLobby* lobby,
                       const MpLobbyEntry* entry, DWORD now, const LobbySender* sender)
{
    if (!intent->loaded_run_epoch || lobby->phase != MP_LOBBY_STARTING)
        return 0;
    if (intent->loaded_room != lobby->room || intent->loaded_run_epoch != lobby->run_epoch)
        return 0;
    if (!(entry->flags & MP_LOBBY_ENTRY_PARTICIPANT) || (entry->flags & MP_LOBBY_ENTRY_LOADED))
        return 0;
    if (!due(&requests->loaded, now))
        return 0;

    MpLobbyLoaded request = {lobby->room, lobby->run_epoch};
    if (!sender->loaded(sender->context, &request))
        return 0;
    mark_sent(&requests->loaded, now);
    return 1;
}

/* Host only: asks for the wanted map until the room shows it. Only the host changes
   settings, so the current settings revision is always the one to name. */
static int step_settings(LobbyRequests* requests, const LobbyIntent* intent, const MpLobby* lobby,
                         const MpLobbyEntry* entry, DWORD now, const LobbySender* sender)
{
    if (!intent->settings_wanted || lobby->phase != MP_LOBBY_OPEN || !is_host(lobby, entry))
        return 0;
    if (!strcmp(intent->settings_map, lobby->map) || !due(&requests->settings, now))
        return 0;

    MpLobbySettings request;
    memset(&request, 0, sizeof(request));
    request.room = lobby->room;
    request.settings_revision = lobby->settings_revision;
    strcpy_s(request.map, sizeof(request.map), intent->settings_map);
    if (!sender->settings(sender->context, &request))
        return 0;
    mark_sent(&requests->settings, now);
    return 1;
}

/* Any member: asks for the wanted character until the room shows it. A ready guest is
   locked by the server, so nothing is sent then. */
static int step_profile(LobbyRequests* requests, const LobbyIntent* intent, const MpLobby* lobby,
                        const MpLobbyEntry* entry, DWORD now, const LobbySender* sender)
{
    if (!intent->character_wanted || lobby->phase != MP_LOBBY_OPEN)
        return 0;
    if (entry->character == intent->character_value || (entry->flags & MP_LOBBY_ENTRY_READY))
        return 0;
    if (!due(&requests->profile, now))
        return 0;

    MpLobbyProfile request = {lobby->room, intent->character_value};
    if (!sender->profile(sender->context, &request))
        return 0;
    mark_sent(&requests->profile, now);
    return 1;
}

unsigned int lobby_requests_step(LobbyRequests* requests, const LobbyIntent* intent,
                                 const MpLobby* lobby, uint32_t local_id, DWORD now,
                                 const LobbySender* sender)
{
    if (!requests || !intent || !lobby || !sender)
        return 0;
    if (requests->generation != intent->generation)
    {
        memset(requests, 0, sizeof(*requests));
        requests->generation = intent->generation;
    }

    const MpLobbyEntry* entry = mp_lobby_entry(lobby, local_id);
    if (!entry)
        return 0;

    unsigned int sent = 0;
    sent += (unsigned int)step_ready(requests, intent, lobby, entry, now, sender);
    sent += (unsigned int)step_start(requests, intent, lobby, entry, now, sender);
    sent += (unsigned int)step_loaded(requests, intent, lobby, entry, now, sender);
    sent += (unsigned int)step_settings(requests, intent, lobby, entry, now, sender);
    sent += (unsigned int)step_profile(requests, intent, lobby, entry, now, sender);
    return sent;
}
