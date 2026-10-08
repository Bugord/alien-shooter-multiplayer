#include <stdlib.h>
#include <string.h>
#include "state_client.h"
#include "epnet_client.h"

typedef struct Peer {
    MpState state;
    uint32_t session;
    unsigned long received_at;
    int present;
    uint32_t shot_session, shot_sequence;
    int shot_received;
} Peer;
struct StateClient {
    epnet_client_t* transport;
    FILE* log;
    char name[MP_MAX_NAME_LEN + 1];
    char map[MP_MAX_MAP_NAME_LEN];
    int ready, joined, published;
    unsigned long previous, requested_at, sent_at, published_at;
    uint32_t sequence, sent, received, rejected;
    MpState latest;
    Peer peers[EPNET_MAX_CLIENTS];
    /* Latest roster: names are trusted only for the session that the server lists. */
    char roster_name[EPNET_MAX_CLIENTS][MP_MAX_NAME_LEN + 1];
    uint32_t roster_session[EPNET_MAX_CLIENTS];
    uint32_t server_capacity;
    uint32_t shot_sequence, shot_dropped;
    ShotEvent shots[64];
    unsigned int shot_head, shot_count;
};

StateClient* state_client_create(const char* host, unsigned short port, const char* name, FILE* log) {
    if (!host || !port || !mp_is_player_name(name) || epnet_init()) return NULL;
    StateClient* c = calloc(1, sizeof(*c));
    if (c) {
        c->log = log;
        strcpy_s(c->name, sizeof(c->name), name);
        c->transport = epnet_client_create();
        if (c->transport && !epnet_client_connect(c->transport, host, port)) {
            if (log) fprintf(log, "# NET_CONNECT host=%s port=%u name=%s\n", host, port, name);
            return c;
        }
        if (c->transport) epnet_client_destroy(c->transport);
        free(c);
    }
    epnet_shutdown();
    return NULL;
}
void state_client_destroy(StateClient* c) {
    if (!c) return;
    epnet_client_disconnect(c->transport);
    epnet_client_destroy(c->transport);
    epnet_shutdown();
    free(c);
}
void state_client_publish(StateClient* c, const MpState* state, unsigned long now) {
    if (!c || !state) return;
    c->latest = *state; c->published_at = now; c->published = 1;
}
void state_client_update(StateClient* c, unsigned long now) {
    if (!c) return;
    double dt = c->previous ? (now - c->previous) / 1000.0 : 0;
    c->previous = now;
    epnet_client_update(c->transport, dt);
    epnet_event_t ev;
    while (epnet_client_poll_events(c->transport, &ev)) {
        if (ev.type == EPNET_EVENT_CONNECTED) { c->joined = 1; c->requested_at = now - 500; }
        if (ev.type == EPNET_EVENT_DISCONNECTED) {
            c->ready = c->joined = 0;
            c->shot_count = c->shot_head = 0;
            memset(c->peers, 0, sizeof(c->peers));
            memset(c->roster_name, 0, sizeof(c->roster_name)); memset(c->roster_session, 0, sizeof(c->roster_session));
            if (c->log) fprintf(c->log, "# NET_DISCONNECTED reason=%u\n", ev.data.disconnect_reason);
        }
        if (ev.type != EPNET_EVENT_PACKET) continue;
        if (ev.data.packet.pkt_type == MPT_S_WELCOME && c->joined) {
            uint32_t id, capacity; char map[MP_MAX_MAP_NAME_LEN];
            if (!mp_welcome_decode(ev.data.packet.data, ev.data.packet.len, &id, &capacity, map) ||
                capacity > EPNET_MAX_CLIENTS || id != epnet_client_get_id(c->transport)) { ++c->rejected; continue; }
            memcpy(c->map, map, sizeof(c->map)); c->server_capacity = capacity;
            if (!c->ready && c->log) fprintf(c->log, "# NET_READY id=%u\n", id);
            c->ready = 1;
        }
        if (ev.data.packet.pkt_type == MPT_S_STATE && c->ready) {
            MpState state;
            const uint8_t* data = ev.data.packet.data;
            if (ev.data.packet.len != MP_STATE_RELAY_SIZE) { ++c->rejected; continue; }
            uint32_t id = mp_get_u32(data), session = mp_get_u32(data + 4);
            if (id >= EPNET_MAX_CLIENTS || id == epnet_client_get_id(c->transport) || !session ||
                !mp_state_decode(data + 8, MP_STATE_SIZE, &state)) { ++c->rejected; continue; }
            Peer* p = &c->peers[id];
            if (p->present && ((session == p->session && !mp_sequence_newer(state.sequence, p->state.sequence)) ||
                (session != p->session && !mp_sequence_newer(session, p->session)))) { ++c->rejected; continue; }
            p->state = state; p->session = session; p->received_at = now; p->present = 1;
            ++c->received;
            if (c->log) fprintf(c->log, "# NET_REMOTE id=%u session=%u seq=%u active=%u tick=%u x=%.3f y=%.3f z=%.3f health=%d weapon=%d ammo=%d animation=%u direction=%u velocity=%.3f moving=%u torso_present=%u torso_direction=%u\n",
                id, session, state.sequence, state.active, state.tick, state.x, state.y, state.z,
                state.health, state.weapon_slot, state.current_ammo, state.animation, state.direction,
                state.velocity, state.moving, state.torso_present, state.torso_direction);
        }
        if (ev.data.packet.pkt_type == MPT_S_ROSTER && c->ready) {
            MpRosterEntry entries[MP_ROSTER_MAX_ENTRIES]; unsigned int count;
            if (!mp_roster_decode(ev.data.packet.data, ev.data.packet.len, entries, &count)) { ++c->rejected; continue; }
            /* The roster replaces the whole table, so departed peers lose their names. */
            memset(c->roster_name, 0, sizeof(c->roster_name)); memset(c->roster_session, 0, sizeof(c->roster_session));
            for (unsigned int i = 0; i < count; ++i) {
                if (entries[i].id >= EPNET_MAX_CLIENTS) continue;
                memcpy(c->roster_name[entries[i].id], entries[i].name, sizeof(entries[i].name));
                c->roster_session[entries[i].id] = entries[i].session;
            }
        }
        if (ev.data.packet.pkt_type == MPT_S_SHOT && c->ready) {
            const uint8_t* data = ev.data.packet.data;
            ShotEvent event = {0};
            if (ev.data.packet.len != MP_SHOT_RELAY_SIZE) { ++c->rejected; continue; }
            event.id = mp_get_u32(data); event.session = mp_get_u32(data + 4);
            if (event.id >= EPNET_MAX_CLIENTS || event.id == epnet_client_get_id(c->transport) ||
                !event.session || !mp_shot_decode(data + 8, MP_SHOT_SIZE, &event.shot)) {
                ++c->rejected; continue;
            }
            Peer* p = &c->peers[event.id];
            if ((p->present && event.session != p->session && !mp_sequence_newer(event.session, p->session)) ||
                (p->shot_received && ((event.session == p->shot_session &&
                  !mp_sequence_newer(event.shot.sequence, p->shot_sequence)) ||
                  (event.session != p->shot_session && !mp_sequence_newer(event.session, p->shot_session))))) {
                ++c->rejected; continue;
            }
            p->shot_received = 1; p->shot_session = event.session; p->shot_sequence = event.shot.sequence;
            if (c->shot_count == 64) { ++c->shot_dropped; continue; }
            c->shots[(c->shot_head + c->shot_count++) % 64] = event;
        }
    }
    if (c->joined && !c->ready && now - c->requested_at >= 500) {
        uint8_t hello[5 + MP_MAX_NAME_LEN];
        int length = mp_hello_encode(hello, sizeof(hello), c->name);
        if (length) epnet_client_send(c->transport, MPT_C_HELLO, hello, (size_t)length);
        c->requested_at = now;
    }
    if (c->ready && now - c->sent_at >= MP_STATE_SEND_MS) {
        MpState state = c->latest;
        /* Menus and stalled sampling invalidate the previous player position. */
        if (!c->published || now - c->published_at > 250) memset(&state, 0, sizeof(state));
        state.sequence = ++c->sequence;
        uint8_t packet[MP_STATE_SIZE];
        mp_state_encode(packet, &state);
        epnet_client_send(c->transport, MPT_C_STATE, packet, sizeof(packet));
        ++c->sent; c->sent_at = now;
    }
}
const char* state_client_map(const StateClient* c) { return c && c->ready ? c->map : NULL; }
int state_client_connected(const StateClient* c) { return c && c->ready; }
int state_client_peer(const StateClient* c, unsigned int id, unsigned long now, MpState* state) {
    if (!c || !state || id >= EPNET_MAX_CLIENTS || !c->peers[id].present ||
        now - c->peers[id].received_at > 1000) return 0;
    *state = c->peers[id].state;
    return 1;
}
void state_client_stats(const StateClient* c) {
    if (c && c->log) fprintf(c->log, "# NET_STATS ready=%d sent=%u received=%u rejected=%u shot_dropped=%u\n",
        c->ready, c->sent, c->received, c->rejected, c->shot_dropped);
}
int state_client_peer_info(const StateClient* c, unsigned int id, unsigned long now, PeerState* out) {
    memset(out, 0, sizeof(*out));
    if (!state_client_peer(c, id, now, &out->state)) return 0;
    out->session = c->peers[id].session; out->present = 1;
    if (c->roster_session[id] == out->session) memcpy(out->name, c->roster_name[id], sizeof(out->name));
    return 1;
}
int state_client_send_shot(StateClient* c, const MpShot* source, unsigned long now) {
    if (!c || !c->ready || !source || !c->published || now - c->published_at > 250u ||
        !c->latest.active || c->latest.health <= 0 || !source->world_epoch ||
        source->world_epoch != c->latest.world_epoch || source->world_low != c->latest.world_low ||
        source->world_high != c->latest.world_high) return 0;
    MpShot shot = *source; shot.sequence = ++c->shot_sequence;
    uint8_t packet[MP_SHOT_SIZE]; mp_shot_encode(packet, &shot);
    MpShot checked;
    if (!mp_shot_decode(packet, sizeof(packet), &checked)) return 0;
    epnet_client_send(c->transport, MPT_C_SHOT, packet, sizeof(packet));
    return 1;
}
int state_client_take_shot(StateClient* c, ShotEvent* out) {
    if (!c || !out || !c->shot_count) return 0;
    *out = c->shots[c->shot_head]; c->shot_head = (c->shot_head + 1) % 64; --c->shot_count;
    return 1;
}
