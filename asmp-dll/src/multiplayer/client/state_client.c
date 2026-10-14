/* Network client for one relay-server connection: sends the local state, shots,
   monster data and lobby requests, and keeps the latest validated peer, shot, monster
   and room data. It talks to the relay through the transport boundary (transport.h). Layer:
   multiplayer/client/ (connection and packet handling). It touches no native game entity and
   no rendering. Used from the worker thread only; the game thread gets data through
   multiplayer.c and monster_sync.c handoffs. */

#include <stdlib.h>
#include <string.h>
#include "state_client.h"

typedef struct Peer
{
    MpState state;
    uint32_t session;
    unsigned long received_at;
    int present;
    uint32_t shot_session, shot_sequence;
    int shot_received;
} Peer;
struct StateClient
{
    Transport* transport;
    FILE* log;
    char name[MP_MAX_NAME_LEN + 1];
    char map[MP_MAX_MAP_NAME_LEN];
    int ready, joined, published;
    ClientLink link;
    unsigned long previous, requested_at, sent_at, published_at;
    uint32_t sequence, sent, received, rejected;
    MpState latest;
    Peer peers[MP_MAX_PEERS];
    /* Latest roster: names are trusted only for the session that the server lists. */
    char roster_name[MP_MAX_PEERS][MP_MAX_NAME_LEN + 1];
    uint32_t roster_session[MP_MAX_PEERS];
    uint32_t server_capacity;
    uint32_t shot_sequence, shot_dropped;
    ShotEvent shots[64];
    unsigned int shot_head, shot_count;
    int monster_claim_requested, monster_authority_known;
    int monster_seen[MP_MONSTER_CAPACITY], monster_event_seen[MP_MONSTER_CAPACITY];
    uint32_t monster_authority_id, monster_authority_session;
    uint32_t monster_event_sequence[MP_MONSTER_CAPACITY];
    unsigned long monster_claim_at, monster_received_at[MP_MONSTER_CAPACITY];
    MonsterState monster[MP_MONSTER_CAPACITY];
    MonsterEvent monster_events[64];
    unsigned int monster_head, monster_count, monster_dropped;
    /* Latest valid room snapshot from this connection. */
    struct
    {
        int known;
        MpLobby snapshot;
    } lobby;
};

/* Forgets all received monster states and queued monster events. */
static void clear_monster_(StateClient* c)
{
    memset(c->monster_seen, 0, sizeof(c->monster_seen));
    memset(c->monster_event_seen, 0, sizeof(c->monster_event_seen));
    c->monster_head = c->monster_count = 0;
    memset(&c->monster, 0, sizeof(c->monster));
}

/* True when this client published a fresh (under 250 ms) state in exactly this world. */
static int monster_local_world_(const StateClient* c, uint32_t low, uint32_t high, uint32_t epoch,
                                unsigned long now)
{
    return c->published && now - c->published_at <= 250u &&
           mp_monster_world(c->latest.world_low, c->latest.world_high, c->latest.world_epoch) &&
           c->latest.world_low == low && c->latest.world_high == high &&
           c->latest.world_epoch == epoch;
}

/* True when the monster authority (this client or a peer) is currently in exactly this world. */
static int monster_remote_world_(const StateClient* c, uint32_t low, uint32_t high, uint32_t epoch,
                                 unsigned long now)
{
    if (!c->ready || !c->monster_authority_known)
        return 0;
    uint32_t id = c->monster_authority_id;
    if (id == transport_local_id(c->transport))
        return c->roster_session[id] == c->monster_authority_session &&
               monster_local_world_(c, low, high, epoch, now);
    const Peer* p = &c->peers[id];
    return p->present && p->session == c->monster_authority_session &&
           now - p->received_at <= MP_MONSTER_EXPIRY_MS &&
           mp_monster_world(p->state.world_low, p->state.world_high, p->state.world_epoch) &&
           p->state.world_low == low && p->state.world_high == high &&
           p->state.world_epoch == epoch;
}

/* The link went down: a connection that never answered is "no answer", one that
   answered but never finished the handshake was refused, a ready one was lost. */
static ClientLink mark_link_down(ClientLink link)
{
    if (link == LINK_CONNECTING)
        return LINK_NO_ANSWER;
    if (link == LINK_HANDSHAKE)
        return LINK_REFUSED;
    if (link == LINK_READY)
        return LINK_LOST;
    return link;
}

/* Accepts a valid snapshot of the same build that is newer than the stored one, or
   the first snapshot of another room. A snapshot of another build marks the link
   incompatible and is not stored. */
static void receive_lobby(StateClient* c, const uint8_t* data, int length)
{
    MpLobby snapshot;
    if (!mp_lobby_decode(data, length, &snapshot))
    {
        ++c->rejected;
        return;
    }

    if (snapshot.compat != MP_COMPAT_ID)
    {
        c->link = LINK_INCOMPATIBLE;
        c->lobby.known = 0;
        return;
    }

    int same_room = c->lobby.known && c->lobby.snapshot.room == snapshot.room;
    if (same_room && !mp_sequence_newer(snapshot.revision, c->lobby.snapshot.revision))
        return; /* Periodic resend or an older packet. */

    c->lobby.snapshot = snapshot;
    c->lobby.known = 1;
    if (c->log)
        fprintf(c->log, "# LOBBY room=%u revision=%u phase=%u run=%u players=%u notice=%u\n",
                snapshot.room, snapshot.revision, snapshot.phase, snapshot.run_epoch,
                snapshot.count, snapshot.notice);
}

/* Wraps a connecting transport. The client takes ownership of `transport` even on
   failure (it is destroyed then). Returns NULL on a bad name or out of memory. */
StateClient* state_client_create_with_transport(Transport* transport, const char* name, FILE* log)
{
    if (!transport || !mp_is_player_name(name))
    {
        transport_destroy(transport);
        return NULL;
    }

    StateClient* c = calloc(1, sizeof(*c));
    if (!c)
    {
        transport_destroy(transport);
        return NULL;
    }

    c->log = log;
    c->transport = transport;
    c->link = LINK_CONNECTING;
    strcpy_s(c->name, sizeof(c->name), name);
    return c;
}

/* Connects to the relay over UDP. Returns NULL on a bad argument or transport failure;
   the caller owns the client and frees it with state_client_destroy. */
StateClient* state_client_create(const char* host, unsigned short port, const char* name, FILE* log)
{
    if (!host || !port || !mp_is_player_name(name))
        return NULL;

    Transport* transport = transport_udp_connect(host, port);
    if (!transport)
        return NULL;

    StateClient* c = state_client_create_with_transport(transport, name, log);
    if (c && log)
        fprintf(log, "# NET_CONNECT host=%s port=%u name=%s\n", host, port, name);
    return c;
}

/* Disconnects and frees the client (NULL is accepted). */
void state_client_destroy(StateClient* c)
{
    if (!c)
        return;
    transport_destroy(c->transport);
    free(c);
}

/* Remembers the newest local state; state_client_update sends it at the send interval. */
void state_client_publish(StateClient* c, const MpState* state, unsigned long now)
{
    if (!c || !state)
        return;
    c->latest = *state;
    c->published_at = now;
    c->published = 1;
}

/* Pumps the transport: sends hello and state, then validates each received packet
   (version, session, sequence) and updates peers, roster, shots and monsters. */
void state_client_update(StateClient* c, unsigned long now)
{
    if (!c)
        return;
    double dt = c->previous ? (now - c->previous) / 1000.0 : 0;
    c->previous = now;
    transport_update(c->transport, dt);
    TransportEvent ev;
    while (transport_poll(c->transport, &ev))
    {
        if (ev.type == TRANSPORT_CONNECTED)
        {
            c->joined = 1;
            c->requested_at = now - 500;
            if (c->link == LINK_CONNECTING)
                c->link = LINK_HANDSHAKE;
        }
        if (ev.type == TRANSPORT_DISCONNECTED)
        {
            c->link = mark_link_down(c->link);
            c->lobby.known = 0;
            c->ready = c->joined = 0;
            c->monster_authority_known = 0;
            clear_monster_(c);
            c->shot_count = c->shot_head = 0;
            memset(c->peers, 0, sizeof(c->peers));
            memset(c->roster_name, 0, sizeof(c->roster_name));
            memset(c->roster_session, 0, sizeof(c->roster_session));
            if (c->log)
                fprintf(c->log, "# NET_DISCONNECTED reason=%u\n", ev.reason);
        }
        if (ev.type != TRANSPORT_PACKET)
            continue;
        if (ev.packet_type == MPT_S_WELCOME && c->joined)
        {
            uint32_t id, capacity;
            char map[MP_MAX_MAP_NAME_LEN];
            if (!mp_welcome_decode(ev.data, ev.length, &id, &capacity, map) ||
                capacity > MP_MAX_PEERS || id != transport_local_id(c->transport))
            {
                ++c->rejected;
                continue;
            }
            memcpy(c->map, map, sizeof(c->map));
            c->server_capacity = capacity;
            if (!c->ready && c->log)
                fprintf(c->log, "# NET_READY id=%u\n", id);
            c->ready = 1;
            if (c->link == LINK_HANDSHAKE)
                c->link = LINK_READY;
        }
        if (ev.packet_type == MPT_S_LOBBY && c->ready)
        {
            receive_lobby(c, ev.data, ev.length);
            continue;
        }
        if (ev.packet_type == MPT_S_STATE && c->ready)
        {
            MpState state;
            const uint8_t* data = ev.data;
            if (ev.length != MP_STATE_RELAY_SIZE)
            {
                ++c->rejected;
                continue;
            }
            uint32_t id = mp_get_u32(data), session = mp_get_u32(data + 4);
            if (id >= MP_MAX_PEERS || id == transport_local_id(c->transport) || !session ||
                !mp_state_decode(data + 8, MP_STATE_SIZE, &state))
            {
                ++c->rejected;
                continue;
            }
            Peer* p = &c->peers[id];
            if (p->present &&
                ((session == p->session && !mp_sequence_newer(state.sequence, p->state.sequence)) ||
                 (session != p->session && !mp_sequence_newer(session, p->session))))
            {
                ++c->rejected;
                continue;
            }
            if (c->monster_authority_known && id == c->monster_authority_id &&
                (session != c->monster_authority_session ||
                 !mp_monster_world(state.world_low, state.world_high, state.world_epoch) ||
                 !p->present || state.world_low != p->state.world_low ||
                 state.world_high != p->state.world_high ||
                 state.world_epoch != p->state.world_epoch))
                clear_monster_(c);
            p->state = state;
            p->session = session;
            p->received_at = now;
            p->present = 1;
            ++c->received;
            if (c->log)
                fprintf(c->log,
                        "# NET_REMOTE id=%u session=%u seq=%u active=%u tick=%u x=%.3f y=%.3f "
                        "z=%.3f health=%d weapon=%d ammo=%d animation=%u direction=%u "
                        "velocity=%.3f moving=%u torso_present=%u torso_direction=%u\n",
                        id, session, state.sequence, state.active, state.tick, state.x, state.y,
                        state.z, state.health, state.weapon_slot, state.current_ammo,
                        state.animation, state.direction, state.velocity, state.moving,
                        state.torso_present, state.torso_direction);
        }
        if (ev.packet_type == MPT_S_ROSTER && c->ready)
        {
            MpRosterEntry entries[MP_ROSTER_MAX_ENTRIES];
            unsigned int count;
            if (!mp_roster_decode(ev.data, ev.length, entries, &count))
            {
                ++c->rejected;
                continue;
            }
            /* The roster replaces the whole table, so departed peers lose their names. */
            memset(c->roster_name, 0, sizeof(c->roster_name));
            memset(c->roster_session, 0, sizeof(c->roster_session));
            for (unsigned int i = 0; i < count; ++i)
            {
                if (entries[i].id >= MP_MAX_PEERS)
                    continue;
                memcpy(c->roster_name[entries[i].id], entries[i].name, sizeof(entries[i].name));
                c->roster_session[entries[i].id] = entries[i].session;
            }
        }
        if (ev.packet_type == MPT_S_SHOT && c->ready)
        {
            const uint8_t* data = ev.data;
            ShotEvent event = {0};
            if (ev.length != MP_SHOT_RELAY_SIZE)
            {
                ++c->rejected;
                continue;
            }
            event.id = mp_get_u32(data);
            event.session = mp_get_u32(data + 4);
            if (event.id >= MP_MAX_PEERS || event.id == transport_local_id(c->transport) ||
                !event.session || !mp_shot_decode(data + 8, MP_SHOT_SIZE, &event.shot))
            {
                ++c->rejected;
                continue;
            }
            Peer* p = &c->peers[event.id];
            if ((p->present && event.session != p->session &&
                 !mp_sequence_newer(event.session, p->session)) ||
                (p->shot_received && ((event.session == p->shot_session &&
                                       !mp_sequence_newer(event.shot.sequence, p->shot_sequence)) ||
                                      (event.session != p->shot_session &&
                                       !mp_sequence_newer(event.session, p->shot_session)))))
            {
                ++c->rejected;
                continue;
            }
            p->shot_received = 1;
            p->shot_session = event.session;
            p->shot_sequence = event.shot.sequence;
            if (c->shot_count == 64)
            {
                ++c->shot_dropped;
                continue;
            }
            c->shots[(c->shot_head + c->shot_count++) % 64] = event;
        }
        if (ev.packet_type == MPT_S_MONSTER_AUTHORITY && c->ready)
        {
            const uint8_t* data = ev.data;
            if (ev.length != 12 || mp_get_u32(data) != MP_MONSTER_VERSION)
            {
                ++c->rejected;
                continue;
            }
            uint32_t id = mp_get_u32(data + 4), session = mp_get_u32(data + 8);
            if (id == UINT32_MAX && !session)
                continue;
            if (id >= MP_MAX_PEERS || !session ||
                (c->monster_authority_known &&
                 (id != c->monster_authority_id || session != c->monster_authority_session)))
            {
                ++c->rejected;
                continue;
            }
            if (!c->monster_authority_known && c->log)
                fprintf(c->log, "# MONSTER_AUTHORITY ready=1 id=%u session=%u local=%d\n", id,
                        session, id == transport_local_id(c->transport));
            c->monster_authority_known = 1;
            c->monster_authority_id = id;
            c->monster_authority_session = session;
        }
        if (ev.packet_type == MPT_S_MONSTER_STATE && c->ready)
        {
            const uint8_t* data = ev.data;
            MpMonsterState state;
            if (ev.length != MP_MONSTER_STATE_RELAY_SIZE || !c->monster_authority_known ||
                mp_get_u32(data) != c->monster_authority_id ||
                mp_get_u32(data + 4) != c->monster_authority_session ||
                !mp_monster_state_decode(data + 8, MP_MONSTER_STATE_SIZE, &state) ||
                !monster_remote_world_(c, state.world_low, state.world_high, state.world_epoch,
                                       now) ||
                (c->monster_seen[state.monster_id - 1u] &&
                 !mp_monster_state_progresses(&c->monster[state.monster_id - 1u].state, &state)))
            {
                ++c->rejected;
                continue;
            }
            unsigned int slot = state.monster_id - 1u;
            c->monster[slot].state = state;
            c->monster[slot].authority_id = c->monster_authority_id;
            c->monster[slot].authority_session = c->monster_authority_session;
            c->monster[slot].present = 1;
            c->monster_seen[slot] = 1;
            c->monster_received_at[slot] = now;
        }
        if (ev.packet_type == MPT_S_MONSTER_EVENT && c->ready)
        {
            const uint8_t* data = ev.data;
            MonsterEvent event;
            if (ev.length != MP_MONSTER_EVENT_RELAY_SIZE || !c->monster_authority_known ||
                mp_get_u32(data) != c->monster_authority_id ||
                mp_get_u32(data + 4) != c->monster_authority_session ||
                !mp_monster_event_decode(data + 8, MP_MONSTER_EVENT_SIZE, &event.event) ||
                !c->monster_seen[event.event.monster_id - 1u] ||
                !monster_remote_world_(c, event.event.world_low, event.event.world_high,
                                       event.event.world_epoch, now) ||
                now - c->monster_received_at[event.event.monster_id - 1u] > MP_MONSTER_EXPIRY_MS ||
                event.event.world_low != c->monster[event.event.monster_id - 1u].state.world_low ||
                event.event.world_high !=
                    c->monster[event.event.monster_id - 1u].state.world_high ||
                event.event.world_epoch !=
                    c->monster[event.event.monster_id - 1u].state.world_epoch ||
                !mp_monster_event_matches(&c->monster[event.event.monster_id - 1u].state,
                                          &event.event) ||
                (c->monster_event_seen[event.event.monster_id - 1u] &&
                 !mp_sequence_newer(event.event.sequence,
                                    c->monster_event_sequence[event.event.monster_id - 1u])))
            {
                ++c->rejected;
                continue;
            }
            c->monster_event_seen[event.event.monster_id - 1u] = 1;
            c->monster_event_sequence[event.event.monster_id - 1u] = event.event.sequence;
            event.authority_id = c->monster_authority_id;
            event.authority_session = c->monster_authority_session;
            if (c->monster_count == 64u)
            {
                ++c->monster_dropped;
                continue;
            }
            c->monster_events[(c->monster_head + c->monster_count++) % 64u] = event;
        }
    }
    if (c->joined && !c->ready && now - c->requested_at >= 500)
    {
        uint8_t hello[5 + MP_MAX_NAME_LEN];
        int length = mp_hello_encode(hello, sizeof(hello), c->name);
        if (length)
            transport_send(c->transport, MPT_C_HELLO, hello, (size_t)length);
        c->requested_at = now;
    }
    if (c->ready && now - c->sent_at >= MP_STATE_SEND_MS)
    {
        MpState state = c->latest;
        /* Menus and stalled sampling invalidate the previous player position. */
        if (!c->published || now - c->published_at > 250)
            memset(&state, 0, sizeof(state));
        state.sequence = ++c->sequence;
        uint8_t packet[MP_STATE_SIZE];
        mp_state_encode(packet, &state);
        transport_send(c->transport, MPT_C_STATE, packet, sizeof(packet));
        ++c->sent;
        c->sent_at = now;
    }
    if (c->ready && c->monster_claim_requested && !c->monster_authority_known &&
        now - c->monster_claim_at >= 500u)
    {
        uint32_t id = transport_local_id(c->transport);
        if (id < MP_MAX_PEERS && c->roster_session[id])
        {
            uint8_t packet[8];
            mp_put_u32(packet, MP_MONSTER_VERSION);
            mp_put_u32(packet + 4, c->roster_session[id]);
            transport_send(c->transport, MPT_C_MONSTER_CLAIM, packet, sizeof(packet));
            c->monster_claim_at = now;
        }
    }
}

/* Map path from the server's welcome, or NULL until connected. */
const char* state_client_map(const StateClient* c)
{
    return c && c->ready ? c->map : NULL;
}
int state_client_connected(const StateClient* c)
{
    return c && c->ready;
}

/* Copies a peer's latest state; 0 when absent or older than one second. */
int state_client_peer(const StateClient* c, unsigned int id, unsigned long now, MpState* state)
{
    if (!c || !state || id >= MP_MAX_PEERS || !c->peers[id].present ||
        now - c->peers[id].received_at > 1000)
        return 0;
    *state = c->peers[id].state;
    return 1;
}
void state_client_stats(const StateClient* c)
{
    if (c && c->log)
        fprintf(c->log,
                "# NET_STATS ready=%d sent=%u received=%u rejected=%u shot_dropped=%u "
                "monster_dropped=%u\n",
                c->ready, c->sent, c->received, c->rejected, c->shot_dropped, c->monster_dropped);
}

/* Like state_client_peer, plus the session id and the name from the roster. */
int state_client_peer_info(const StateClient* c, unsigned int id, unsigned long now, PeerState* out)
{
    memset(out, 0, sizeof(*out));
    if (!state_client_peer(c, id, now, &out->state))
        return 0;
    out->session = c->peers[id].session;
    out->present = 1;
    if (c->roster_session[id] == out->session)
        memcpy(out->name, c->roster_name[id], sizeof(out->name));
    return 1;
}

/* Sends one local shot if the published state is fresh, alive and in the same world.
   Returns 0 when it was not sent (the caller drops it). */
int state_client_send_shot(StateClient* c, const MpShot* source, unsigned long now)
{
    if (!c || !c->ready || !source || !c->published || now - c->published_at > 250u ||
        !c->latest.active || c->latest.health <= 0 || !source->world_epoch ||
        source->world_epoch != c->latest.world_epoch || source->world_low != c->latest.world_low ||
        source->world_high != c->latest.world_high)
        return 0;
    MpShot shot = *source;
    shot.sequence = ++c->shot_sequence;
    uint8_t packet[MP_SHOT_SIZE];
    mp_shot_encode(packet, &shot);
    MpShot checked;
    if (!mp_shot_decode(packet, sizeof(packet), &checked))
        return 0;
    transport_send(c->transport, MPT_C_SHOT, packet, sizeof(packet));
    return 1;
}

/* Pops the oldest received shot from the 64-entry queue. */
int state_client_take_shot(StateClient* c, ShotEvent* out)
{
    if (!c || !out || !c->shot_count)
        return 0;
    *out = c->shots[c->shot_head];
    c->shot_head = (c->shot_head + 1) % 64;
    --c->shot_count;
    return 1;
}

/* Asks the server to make this client the monster authority. */
void state_client_monster_claim(StateClient* c)
{
    if (c)
        c->monster_claim_requested = 1;
}

/* Reports the known monster authority; *is_local says whether it is this client. */
int state_client_monster_authority(const StateClient* c, uint32_t* id, uint32_t* session,
                                   int* is_local)
{
    if (!c || !c->ready || !c->monster_authority_known)
        return 0;
    if (id)
        *id = c->monster_authority_id;
    if (session)
        *session = c->monster_authority_session;
    if (is_local)
        *is_local = c->monster_authority_id == transport_local_id(c->transport) &&
                    c->roster_session[c->monster_authority_id] == c->monster_authority_session;
    return 1;
}

/* Sends one monster state; only the authority may, and only for its published world. */
int state_client_publish_monster(StateClient* c, const MpMonsterState* state, unsigned long now)
{
    int local = 0;
    if (!state || !state_client_monster_authority(c, NULL, NULL, &local) || !local ||
        !monster_local_world_(c, state->world_low, state->world_high, state->world_epoch, now))
        return 0;
    uint8_t packet[MP_MONSTER_STATE_CLIENT_SIZE];
    MpMonsterState checked;
    mp_put_u32(packet, c->monster_authority_session);
    mp_monster_state_encode(packet + 4, state);
    if (!mp_monster_state_decode(packet + 4, MP_MONSTER_STATE_SIZE, &checked))
        return 0;
    transport_send(c->transport, MPT_C_MONSTER_STATE, packet, sizeof(packet));
    return 1;
}

/* Copies the received state of monster `slot` unless it is expired or from another world. */
int state_client_monster_at(const StateClient* c, unsigned int slot, unsigned long now,
                            MonsterState* out)
{
    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    if (!c || slot >= MP_MONSTER_CAPACITY || !c->monster_seen[slot] ||
        now - c->monster_received_at[slot] > MP_MONSTER_EXPIRY_MS ||
        !monster_remote_world_(c, c->monster[slot].state.world_low,
                               c->monster[slot].state.world_high,
                               c->monster[slot].state.world_epoch, now))
        return 0;
    *out = c->monster[slot];
    return 1;
}
int state_client_monster(const StateClient* c, unsigned long now, MonsterState* out)
{
    return state_client_monster_at(c, 0u, now, out);
}

/* Sends one monster event; only the authority may, and only for its published world. */
int state_client_send_monster_event(StateClient* c, const MpMonsterEvent* event, unsigned long now)
{
    int local = 0;
    if (!event || !state_client_monster_authority(c, NULL, NULL, &local) || !local ||
        !monster_local_world_(c, event->world_low, event->world_high, event->world_epoch, now))
        return 0;
    uint8_t packet[MP_MONSTER_EVENT_CLIENT_SIZE];
    MpMonsterEvent checked;
    mp_put_u32(packet, c->monster_authority_session);
    mp_monster_event_encode(packet + 4, event);
    if (!mp_monster_event_decode(packet + 4, MP_MONSTER_EVENT_SIZE, &checked))
        return 0;
    transport_send(c->transport, MPT_C_MONSTER_EVENT, packet, sizeof(packet));
    return 1;
}

/* Pops the next received event that still matches its monster's current state. */
int state_client_take_monster_event(StateClient* c, MonsterEvent* out)
{
    if (!c || !out)
        return 0;
    while (c->monster_count)
    {
        MonsterState current;
        MonsterEvent event = c->monster_events[c->monster_head];
        c->monster_head = (c->monster_head + 1u) % 64u;
        --c->monster_count;
        if (state_client_monster_at(c, event.event.monster_id - 1u, c->previous, &current) &&
            mp_monster_event_matches(&current.state, &event.event))
        {
            *out = event;
            return 1;
        }
    }
    return 0;
}

ClientLink state_client_link(const StateClient* c)
{
    return c ? c->link : LINK_NONE;
}

int state_client_lobby(const StateClient* c, MpLobby* out, uint32_t* local_id)
{
    if (!c || !c->ready || !c->lobby.known || c->link != LINK_READY)
        return 0;
    if (out)
        *out = c->lobby.snapshot;
    if (local_id)
        *local_id = transport_local_id(c->transport);
    return 1;
}

int state_client_send_lobby_ready(StateClient* c, const MpLobbyReady* request)
{
    if (!c || !c->ready || !request)
        return 0;
    uint8_t packet[MP_LOBBY_REQUEST_SIZE];
    mp_lobby_ready_encode(packet, request);
    transport_send(c->transport, MPT_C_LOBBY_READY, packet, sizeof(packet));
    return 1;
}

int state_client_send_lobby_settings(StateClient* c, const MpLobbySettings* request)
{
    if (!c || !c->ready || !request)
        return 0;
    uint8_t packet[12 + 1 + MP_MAX_MAP_NAME_LEN];
    int length = mp_lobby_settings_encode(packet, sizeof(packet), request);
    if (!length)
        return 0;
    transport_send(c->transport, MPT_C_LOBBY_SETTINGS, packet, (size_t)length);
    return 1;
}

int state_client_send_lobby_start(StateClient* c, const MpLobbyStart* request)
{
    if (!c || !c->ready || !request)
        return 0;
    uint8_t packet[MP_LOBBY_START_SIZE];
    mp_lobby_start_encode(packet, request);
    transport_send(c->transport, MPT_C_LOBBY_START, packet, sizeof(packet));
    return 1;
}

int state_client_send_lobby_loaded(StateClient* c, const MpLobbyLoaded* request)
{
    if (!c || !c->ready || !request)
        return 0;
    uint8_t packet[MP_LOBBY_LOADED_SIZE];
    mp_lobby_loaded_encode(packet, request);
    transport_send(c->transport, MPT_C_LOBBY_LOADED, packet, sizeof(packet));
    return 1;
}

int state_client_send_lobby_profile(StateClient* c, const MpLobbyProfile* request)
{
    if (!c || !c->ready || !request)
        return 0;
    uint8_t packet[MP_LOBBY_PROFILE_SIZE];
    mp_lobby_profile_encode(packet, request);
    transport_send(c->transport, MPT_C_LOBBY_PROFILE, packet, sizeof(packet));
    return 1;
}
