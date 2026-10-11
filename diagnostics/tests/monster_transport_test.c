#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/state_client.h"
#include "../../asmp-server/src/server.h"
#include "epnet_client.h"
#define CHECK(c)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(c))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static MpState player;
static uint32_t raw_session;
static int raw_publish_player;
static uint32_t raw_player_sequence;
static DWORD raw_player_at;
static void pump(MpServer* server, StateClient* a, StateClient* b, StateClient* late,
                 epnet_client_t* raw, unsigned long duration)
{
    DWORD start = GetTickCount();
    do
    {
        DWORD now = GetTickCount();
        StateClient* clients[] = {a, b, late};
        for (unsigned int i = 0; i < 3u; ++i)
            if (clients[i])
            {
                state_client_publish(clients[i], &player, now);
                state_client_update(clients[i], now);
            }
        if (raw)
        {
            epnet_client_update(raw, 0.001);
            epnet_event_t event;
            while (epnet_client_poll_events(raw, &event))
                if (event.type == EPNET_EVENT_PACKET && event.data.packet.pkt_type == MPT_S_ROSTER)
                {
                    MpRosterEntry entries[MP_ROSTER_MAX_ENTRIES];
                    unsigned int count;
                    if (mp_roster_decode(event.data.packet.data, event.data.packet.len, entries,
                                         &count))
                        for (unsigned int i = 0; i < count; ++i)
                            if (entries[i].id == epnet_client_get_id(raw))
                                raw_session = entries[i].session;
                }
            /* A real game host continually publishes player world presence.
               Keep the raw owner equivalent, including when an observer joins
               after its first packet; the relay does not cache player states. */
            if (raw_publish_player && raw_session && now - raw_player_at >= MP_STATE_SEND_MS)
            {
                MpState raw_player = player;
                raw_player.sequence = ++raw_player_sequence;
                uint8_t packet[MP_STATE_SIZE];
                mp_state_encode(packet, &raw_player);
                epnet_client_send(raw, MPT_C_STATE, packet, sizeof(packet));
                raw_player_at = now;
            }
        }
        mp_server_tick(server);
        Sleep(1);
    } while (GetTickCount() - start < duration);
}
static epnet_client_t* raw_join(MpServer* server, StateClient* a, StateClient* b,
                                unsigned short port)
{
    epnet_client_t* raw = epnet_client_create();
    CHECK(raw);
    CHECK(!epnet_client_connect(raw, "127.0.0.1", port));
    raw_session = 0;
    pump(server, a, b, NULL, raw, 200);
    CHECK(epnet_client_get_state(raw) == EPNET_STATE_CONNECTED);
    uint8_t packet[32];
    int length = mp_hello_encode(packet, sizeof(packet), "Raw");
    epnet_client_send(raw, MPT_C_HELLO, packet, (size_t)length);
    pump(server, a, b, NULL, raw, 100);
    CHECK(raw_session);
    return raw;
}
static void raw_state(epnet_client_t* raw, uint32_t session, const MpMonsterState* state)
{
    uint8_t packet[MP_MONSTER_STATE_CLIENT_SIZE];
    mp_put_u32(packet, session);
    mp_monster_state_encode(packet + 4, state);
    epnet_client_send(raw, MPT_C_MONSTER_STATE, packet, sizeof(packet));
}
static void raw_event(epnet_client_t* raw, const MpMonsterEvent* event)
{
    uint8_t packet[MP_MONSTER_EVENT_CLIENT_SIZE];
    mp_put_u32(packet, raw_session);
    mp_monster_event_encode(packet + 4, event);
    epnet_client_send(raw, MPT_C_MONSTER_EVENT, packet, sizeof(packet));
}
static void accept_raw_state(MpServer* server, StateClient* client, epnet_client_t* raw,
                             const MpMonsterState* state)
{
    DWORD start = GetTickCount();
    MonsterState observed;
    raw_state(raw, raw_session, state);
    do
    {
        pump(server, NULL, client, NULL, raw, 10);
        if (state_client_monster_at(client, state->monster_id - 1u, GetTickCount(), &observed) &&
            observed.state.sequence == state->sequence && observed.state.vid == state->vid &&
            observed.state.form_revision == state->form_revision &&
            observed.state.form_phase == state->form_phase)
            return;
    } while (GetTickCount() - start < 1000u);
    fprintf(stderr, "FAIL: accepted form snapshot did not reach observer\n");
    exit(1);
}
static void reject_raw_state(MpServer* server, StateClient* client, epnet_client_t* raw,
                             const MpMonsterState* bad, const MpMonsterState* expected)
{
    MonsterState observed;
    raw_state(raw, raw_session, bad);
    pump(server, NULL, client, NULL, raw, 60);
    CHECK(state_client_monster_at(client, expected->monster_id - 1u, GetTickCount(), &observed));
    CHECK(observed.state.sequence == expected->sequence && observed.state.vid == expected->vid &&
          observed.state.form_revision == expected->form_revision &&
          observed.state.alive == expected->alive);
}
static void codec_forms(const MpMonsterState* baseline, const MpMonsterEvent* attack)
{
    uint8_t packet[MP_MONSTER_STATE_SIZE], event_packet[MP_MONSTER_EVENT_SIZE];
    MpMonsterState s = *baseline, decoded, previous;
    MpMonsterEvent e = *attack, event_decoded;
    const uint32_t origins[] = {40u, 50u, 60u, 70u, 80u, 90u};
    for (unsigned int i = 0; i < 6u; ++i)
    {
        s.vid = s.origin_vid = origins[i];
        s.form_revision = 1;
        mp_monster_state_encode(packet, &s);
        CHECK(mp_monster_state_decode(packet, sizeof(packet), &decoded));
        s.vid += 9;
        mp_monster_state_encode(packet, &s);
        CHECK(mp_monster_state_decode(packet, sizeof(packet), &decoded));
        e.vid = s.vid;
        e.origin_vid = s.origin_vid;
        e.kind = MP_MONSTER_ATTACK;
        mp_monster_event_encode(event_packet, &e);
        CHECK(!mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded));
        e.kind = MP_MONSTER_DEATH;
        e.death_animation = 15;
        mp_monster_event_encode(event_packet, &e);
        CHECK(mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded));
        e.kind = MP_MONSTER_FORM;
        e.from_vid = s.origin_vid;
        e.death_animation = 0;
        e.form_revision = 2;
        mp_monster_event_encode(event_packet, &e);
        CHECK(mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded));
        e = *attack;
    }
    s = *baseline;
    mp_monster_state_encode(packet, &s);
    mp_put_u32(packet + 72, 49);
    CHECK(!mp_monster_state_decode(packet, sizeof(packet), &decoded));
    mp_monster_state_encode(packet, &s);
    mp_put_u32(packet + 76, 0);
    CHECK(!mp_monster_state_decode(packet, sizeof(packet), &decoded));
    mp_monster_state_encode(packet, &s);
    mp_put_u32(packet + 80, 2);
    CHECK(!mp_monster_state_decode(packet, sizeof(packet), &decoded));
    mp_monster_state_encode(packet, &s);
    mp_put_u32(packet, 3);
    CHECK(!mp_monster_state_decode(packet, sizeof(packet), &decoded));
    CHECK(!mp_monster_reachable(70, 80) && !mp_monster_reachable(90, 69));
    for (unsigned int i = 0; i < 2u; ++i)
    {
        s = *baseline;
        s.vid = s.origin_vid = i ? 90u : 70u;
        previous = s;
        s.sequence++;
        s.form_revision++;
        s.form_phase = MP_MONSTER_FORM_ARMOR;
        s.health = 1;
        s.moving = 0;
        s.velocity = 0;
        mp_monster_state_encode(packet, &s);
        CHECK(mp_monster_state_decode(packet, sizeof(packet), &decoded));
        CHECK(mp_monster_state_progresses(&previous, &s));
        e = *attack;
        e.vid = e.origin_vid = e.from_vid = s.vid;
        e.form_revision = 2;
        e.kind = MP_MONSTER_FORM;
        e.form_phase = MP_MONSTER_FORM_ARMOR;
        e.death_animation = 15;
        mp_monster_event_encode(event_packet, &e);
        CHECK(mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded));
        CHECK(mp_monster_event_matches(&previous, &e)); /* Armor event may precede its snapshot. */
        s.moving = 1;
        mp_monster_state_encode(packet, &s);
        CHECK(!mp_monster_state_decode(packet, sizeof(packet), &decoded));
        s.moving = 0;
        s.velocity = 1;
        mp_monster_state_encode(packet, &s);
        CHECK(!mp_monster_state_decode(packet, sizeof(packet), &decoded));
        s.velocity = 0;
        s.alive = 0;
        s.health = 0;
        s.death_animation = 15;
        mp_monster_state_encode(packet, &s);
        CHECK(!mp_monster_state_decode(packet, sizeof(packet), &decoded));
        s.alive = 1;
        s.health = 1;
        s.death_animation = 0;
        previous = s;
        s.sequence++;
        s.form_revision++;
        s.form_phase = MP_MONSTER_FORM_ACTIVE;
        CHECK(!mp_monster_state_progresses(
            &previous, &s)); /* Pending armor cannot revert to the armored actor. */
        s.vid -= 10;
        CHECK(mp_monster_state_progresses(&previous, &s));
        e.vid = s.vid;
        e.form_revision = s.form_revision;
        e.form_phase = MP_MONSTER_FORM_ACTIVE;
        e.death_animation = 0;
        mp_monster_event_encode(event_packet, &e);
        CHECK(mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded));
        previous = s;
        s.sequence++;
        s.form_revision++;
        s.vid += 9;
        CHECK(mp_monster_state_progresses(&previous, &s));
        previous = s;
        s.sequence++;
        s.form_revision++;
        s.vid -= 9;
        CHECK(!mp_monster_state_progresses(&previous, &s));
        previous = *baseline;
        previous.vid = previous.origin_vid = i ? 90u : 70u;
        s = previous;
        s.sequence++;
        s.form_revision += 3;
        s.vid -= 1;
        CHECK(mp_monster_state_progresses(&previous, &s)); /* Lost armor and freeze packets. */
        e.vid = s.vid;
        e.from_vid = previous.vid;
        e.form_revision = s.form_revision;
        mp_monster_event_encode(event_packet, &e);
        CHECK(mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded));
        CHECK(mp_monster_event_matches(&previous,
                                       &e)); /* Armor finish and freeze coalesce in one tick. */
        previous.vid += 9;
        CHECK(!mp_monster_state_progresses(&previous, &s)); /* Frozen armor remains terminal. */
    }
    previous = *baseline;
    s = previous;
    s.sequence++;
    s.form_revision = 2;
    s.vid = 59;
    CHECK(mp_monster_state_progresses(&previous, &s));
    previous = s;
    s.sequence++;
    s.form_revision = 1;
    CHECK(!mp_monster_state_progresses(&previous, &s));
    s.form_revision = 2;
    s.vid = 50;
    CHECK(!mp_monster_state_progresses(&previous, &s));
    s.form_revision = 3;
    CHECK(!mp_monster_state_progresses(&previous, &s));
    s = previous;
    s.sequence++;
    s.alive = 0;
    s.health = 0;
    s.death_animation = 16;
    CHECK(mp_monster_state_progresses(&previous, &s));
    previous = s;
    s.sequence++;
    s.alive = 1;
    s.health = 90;
    s.death_animation = 0;
    CHECK(!mp_monster_state_progresses(&previous, &s));
    s.world_epoch++;
    CHECK(mp_monster_state_progresses(&previous, &s));
    e = *attack;
    e.form_revision = 2;
    CHECK(!mp_monster_event_matches(baseline, &e)); /* Future attacks wait for snapshot context. */
    e.kind = MP_MONSTER_DEATH;
    e.death_animation = 15;
    CHECK(!mp_monster_event_matches(baseline, &e));
    e.death_animation = 0;
    e.kind = MP_MONSTER_FORM;
    e.vid = 59;
    e.from_vid = 50;
    CHECK(mp_monster_event_matches(baseline, &e));
    e.vid = 50;
    e.from_vid = 59;
    mp_monster_event_encode(event_packet, &e);
    CHECK(!mp_monster_event_decode(event_packet, sizeof(event_packet),
                                   &event_decoded)); /* No thaw. */
    e = *attack;
    e.from_vid = 50;
    mp_monster_event_encode(event_packet, &e);
    CHECK(!mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded));
    const unsigned int offsets[] = {0u, 52u, 56u, 60u, 64u};
    const uint32_t values[] = {3u, 290u, 49u, 0u, 2u};
    for (unsigned int i = 0; i < 5u; ++i)
    {
        mp_monster_event_encode(event_packet, attack);
        mp_put_u32(event_packet + offsets[i], values[i]);
        CHECK(!mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded));
    }
}
int main(void)
{
    memset(&player, 0, sizeof(player));
    player.active = 1;
    player.health = 100;
    player.weapon_slot = -1;
    player.world_epoch = 1;
    mp_world_key("maps\\survive_01.map", &player.world_low, &player.world_high);
    MpMonsterState monster = {0}, decoded;
    monster.sequence = 1;
    monster.world_low = player.world_low;
    monster.world_high = player.world_high;
    monster.world_epoch = 1;
    monster.monster_id = 1;
    monster.vid = 50;
    monster.tick = 100;
    monster.origin_vid = 50;
    monster.form_revision = 1;
    monster.alive = 1;
    monster.health = 90;
    monster.x = -123.5f;
    monster.y = 200.25f;
    monster.direction = 64;
    monster.moving = 1;
    monster.velocity = 0.02f;
    uint8_t packet[MP_MONSTER_STATE_SIZE];
    mp_monster_state_encode(packet, &monster);
    CHECK(packet[3] == MP_MONSTER_VERSION && packet[7] == 1 && packet[27] == 50);
    CHECK(mp_monster_state_decode(packet, sizeof(packet), &decoded) &&
          !memcmp(&decoded, &monster, sizeof(monster)));
    CHECK(!mp_monster_state_decode(packet, sizeof(packet) - 1, &decoded));
    packet[3] = 1;
    CHECK(!mp_monster_state_decode(packet, sizeof(packet),
                                   &decoded)); /* Reject the old wire version. */
    mp_monster_state_encode(packet, &monster);
    CHECK(MP_MONSTER_STATE_SIZE == 84 && !mp_monster_state_decode(packet, 72, &decoded));
    CHECK(packet[75] == 50 && packet[79] == 1 && !packet[83]);
    mp_put_u32(packet + 68, 15);
    CHECK(!mp_monster_state_decode(packet, sizeof(packet),
                                   &decoded)); /* Live state cannot request death. */
    MpMonsterState corpse = monster;
    corpse.alive = 0;
    corpse.health = 0;
    corpse.death_animation = 15;
    mp_monster_state_encode(packet, &corpse);
    CHECK(packet[71] == 15 && mp_monster_state_decode(packet, sizeof(packet), &decoded) &&
          !memcmp(&decoded, &corpse, sizeof(corpse)));
    corpse.death_animation = 16;
    mp_monster_state_encode(packet, &corpse);
    CHECK(packet[71] == 16 && mp_monster_state_decode(packet, sizeof(packet), &decoded) &&
          decoded.death_animation == 16);
    const uint32_t invalid_death[] = {0u, 14u, 17u, UINT32_MAX};
    for (unsigned int i = 0; i < sizeof(invalid_death) / sizeof(invalid_death[0]); ++i)
    {
        mp_put_u32(packet + 68, invalid_death[i]);
        CHECK(!mp_monster_state_decode(packet, sizeof(packet), &decoded));
    }
    mp_monster_state_encode(packet, &monster);
    mp_put_u32(packet + 24, 290);
    CHECK(!mp_monster_state_decode(packet, sizeof(packet), &decoded));
    mp_monster_state_encode(packet, &monster);
    mp_put_u32(packet + 36, 0x7FC00000u);
    CHECK(!mp_monster_state_decode(packet, sizeof(packet), &decoded));
    mp_monster_state_encode(packet, &monster);
    mp_put_u32(packet + 60, 0);
    CHECK(!mp_monster_state_decode(packet, sizeof(packet), &decoded));
    MpMonsterEvent event = {0}, event_decoded;
    event.sequence = 1;
    event.world_low = monster.world_low;
    event.world_high = monster.world_high;
    event.world_epoch = 1;
    event.monster_id = 1;
    event.tick = 200;
    event.kind = MP_MONSTER_ATTACK;
    event.vid = 50;
    event.origin_vid = 50;
    event.form_revision = 1;
    event.x = monster.x;
    event.y = monster.y;
    event.z = 3.0f;
    event.direction = 192;
    uint8_t event_packet[MP_MONSTER_EVENT_SIZE];
    mp_monster_event_encode(event_packet, &event);
    CHECK(mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded) &&
          !memcmp(&event, &event_decoded, sizeof(event)));
    CHECK(MP_MONSTER_EVENT_SIZE == 72 && event_packet[32] == 0xC2 && event_packet[33] == 0xF7 &&
          event_packet[47] == 192);
    CHECK(event_packet[55] == 50 && event_packet[59] == 50 && event_packet[63] == 1 &&
          !event_packet[71]);
    CHECK(!mp_monster_event_decode(event_packet, 32, &event_decoded) &&
          !mp_monster_event_decode(event_packet, sizeof(event_packet) - 1, &event_decoded));
    event_packet[3] = 1;
    CHECK(!mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded));
    mp_monster_event_encode(event_packet, &event);
    mp_put_u32(event_packet + 48, 15);
    CHECK(!mp_monster_event_decode(event_packet, sizeof(event_packet),
                                   &event_decoded)); /* Attacks cannot select death art. */
    MpMonsterEvent death = event;
    death.kind = MP_MONSTER_DEATH;
    death.death_animation = 15;
    mp_monster_event_encode(event_packet, &death);
    CHECK(event_packet[51] == 15 &&
          mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded) &&
          !memcmp(&death, &event_decoded, sizeof(death)));
    death.death_animation = 16;
    mp_monster_event_encode(event_packet, &death);
    CHECK(mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded) &&
          event_decoded.death_animation == 16);
    for (unsigned int i = 0; i < sizeof(invalid_death) / sizeof(invalid_death[0]); ++i)
    {
        mp_put_u32(event_packet + 48, invalid_death[i]);
        CHECK(!mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded));
    }
    const unsigned int pose_offsets[] = {32u, 36u, 40u};
    for (unsigned int i = 0; i < sizeof(pose_offsets) / sizeof(pose_offsets[0]); ++i)
    {
        mp_monster_event_encode(event_packet, &death);
        mp_put_u32(event_packet + pose_offsets[i], 0x7FC00000u);
        CHECK(!mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded));
        mp_monster_event_encode(event_packet, &death);
        mp_put_u32(event_packet + pose_offsets[i], 0x7F800000u);
        CHECK(!mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded));
    }
    death.x = 1000001.0f;
    mp_monster_event_encode(event_packet, &death);
    CHECK(!mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded));
    death.x = event.x;
    death.direction = 256;
    mp_monster_event_encode(event_packet, &death);
    CHECK(!mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded));
    death.direction = event.direction;
    mp_monster_event_encode(event_packet, &event);
    mp_put_u32(event_packet + 28, 4);
    CHECK(!mp_monster_event_decode(event_packet, sizeof(event_packet), &event_decoded));
    codec_forms(&monster, &event);

    unsigned short port = (unsigned short)(51000 + GetCurrentProcessId() % 3000);
    CHECK(!_putenv_s("ASMP_MAP", "maps\\survive_01.map"));
    MpServer* server = mp_server_create(port, 4);
    CHECK(server);
    CHECK(!_putenv_s("ASMP_MAP", ""));
    StateClient* host = state_client_create("127.0.0.1", port, "Host", NULL);
    CHECK(host);
    StateClient* client = state_client_create("127.0.0.1", port, "Client", NULL);
    CHECK(client);
    state_client_monster_claim(host);
    pump(server, host, client, NULL, NULL, 1200);
    CHECK(!strcmp(state_client_map(host), "maps\\survive_01.map"));
    uint32_t authority_id, authority_session;
    int is_local;
    CHECK(state_client_monster_authority(host, &authority_id, &authority_session, &is_local) &&
          is_local);
    CHECK(state_client_monster_authority(client, NULL, NULL, &is_local) && !is_local);
    state_client_monster_claim(client); /* Existing authority cannot be stolen. */
    CHECK(!state_client_publish_monster(client, &monster, GetTickCount()));
    CHECK(!state_client_send_monster_event(client, &event, GetTickCount()));
    CHECK(state_client_publish_monster(host, &monster, GetTickCount()));
    pump(server, host, client, NULL, NULL, 100);
    MonsterState observed;
    MonsterEvent received;
    CHECK(state_client_monster(client, GetTickCount(), &observed) && observed.present &&
          observed.state.x == monster.x);
    CHECK(observed.authority_id == authority_id && observed.authority_session == authority_session);
    CHECK(
        !state_client_take_monster_event(client, &received)); /* Never infer attacks from states. */
    monster.x = 999;
    CHECK(state_client_publish_monster(host, &monster, GetTickCount()));
    pump(server, host, client, NULL, NULL, 100);
    CHECK(state_client_monster(client, GetTickCount(), &observed) && observed.state.x == -123.5f);
    ++monster.sequence;
    CHECK(state_client_publish_monster(host, &monster, GetTickCount()));
    pump(server, host, client, NULL, NULL, 100);
    CHECK(state_client_monster(client, GetTickCount(), &observed) && observed.state.x == 999);
    CHECK(state_client_send_monster_event(host, &event, GetTickCount()));
    pump(server, host, client, NULL, NULL, 100);
    CHECK(state_client_take_monster_event(client, &received) &&
          received.event.kind == MP_MONSTER_ATTACK && received.event.sequence == 1);
    CHECK(received.event.x == event.x && received.event.y == event.y &&
          received.event.z == event.z && received.event.direction == event.direction &&
          !received.event.death_animation);
    CHECK(state_client_send_monster_event(host, &event, GetTickCount()));
    pump(server, host, client, NULL, NULL, 100);
    CHECK(!state_client_take_monster_event(client, &received));

    epnet_client_t* raw = raw_join(server, host, client, port);
    ++monster.sequence;
    monster.x = 333;
    raw_state(raw, authority_session, &monster);
    pump(server, host, client, NULL, raw, 100);
    CHECK(state_client_monster(client, GetTickCount(), &observed) &&
          observed.state.x == 999); /* Server rejects forged non-host. */
    epnet_client_disconnect(raw);
    epnet_client_destroy(raw);
    ++monster.sequence;
    monster.x = 444;
    CHECK(state_client_publish_monster(host, &monster, GetTickCount()));
    pump(server, host, client, NULL, NULL, 50);
    StateClient* late = state_client_create("127.0.0.1", port, "Late", NULL);
    CHECK(late);
    pump(server, host, client, late, NULL, 500);
    CHECK(state_client_monster(late, GetTickCount(), &observed) && observed.state.x == 444);
    CHECK(!state_client_take_monster_event(late, &received)); /* History is not an attack event. */
    CHECK(!state_client_monster(client, GetTickCount() + MP_MONSTER_EXPIRY_MS + 1u, &observed));
    player.active = 0;
    player.health = 0;
    pump(server, host, client, late, NULL, 50);
    ++monster.sequence;
    CHECK(state_client_publish_monster(host, &monster, GetTickCount()));
    pump(server, host, client, late, NULL, 50);
    CHECK(state_client_monster(client, GetTickCount(), &observed) && observed.state.alive);
    monster.alive = 0;
    monster.health = 0;
    monster.event_sequence = 2;
    monster.death_animation = 16;
    monster.x = 555.5f;
    monster.y = 777.25f;
    monster.z = 3.0f;
    monster.direction = 192;
    ++monster.sequence;
    CHECK(state_client_publish_monster(host, &monster, GetTickCount()));
    pump(server, host, client, late, NULL, 100);
    CHECK(state_client_monster(client, GetTickCount(), &observed) && !observed.state.alive);
    CHECK(observed.state.death_animation == 16 && observed.state.x == monster.x &&
          observed.state.y == monster.y && observed.state.z == monster.z &&
          observed.state.direction ==
              monster.direction); /* Lost event retains death pose and variant. */
    StateClient* dead_late = state_client_create("127.0.0.1", port, "DeadLate", NULL);
    CHECK(dead_late);
    pump(server, host, client, dead_late, NULL, 500);
    CHECK(state_client_monster(dead_late, GetTickCount(), &observed) && !observed.state.alive &&
          observed.state.death_animation == 16 && observed.state.x == monster.x &&
          observed.state.direction == monster.direction);
    CHECK(!state_client_take_monster_event(
        dead_late, &received)); /* Dead late join uses persistent state, no event history. */
    state_client_destroy(dead_late);
    event.sequence = 2;
    event.kind = MP_MONSTER_DEATH;
    event.death_animation = 16;
    event.x = monster.x;
    event.y = monster.y;
    event.z = monster.z;
    event.direction = monster.direction;
    CHECK(state_client_send_monster_event(host, &event, GetTickCount()));
    pump(server, host, client, late, NULL, 50);
    CHECK(state_client_take_monster_event(client, &received) &&
          received.event.kind == MP_MONSTER_DEATH && received.event.death_animation == 16 &&
          received.event.x == monster.x && received.event.y == monster.y &&
          received.event.z == monster.z && received.event.direction == monster.direction);
    CHECK(state_client_send_monster_event(host, &event, GetTickCount()));
    pump(server, host, client, late, NULL, 50);
    CHECK(!state_client_take_monster_event(client, &received));
    player.active = 1;
    player.health = 100;
    ++player.world_epoch;
    pump(server, host, client, late, NULL, 100);
    CHECK(!state_client_monster(client, GetTickCount(), &observed));
    CHECK(!state_client_publish_monster(host, &monster, GetTickCount()));
    CHECK(!state_client_send_monster_event(host, &event, GetTickCount()));
    monster.world_epoch = player.world_epoch;
    monster.alive = 1;
    monster.health = 90;
    monster.sequence = 1;
    monster.event_sequence = 0;
    monster.death_animation = 0;
    CHECK(state_client_publish_monster(host, &monster, GetTickCount()));
    pump(server, host, client, late, NULL, 100);
    CHECK(state_client_monster(client, GetTickCount(), &observed) &&
          observed.state.world_epoch == 2);
    MpMonsterState rows[4];
    const unsigned int row_ids[] = {2u, 9u, 17u, 32u}, row_vids[] = {40u, 50u, 60u, 80u};
    for (unsigned int i = 0; i < 4u; ++i)
    {
        rows[i] = monster;
        rows[i].monster_id = row_ids[i];
        rows[i].vid = row_vids[i];
        rows[i].x = 100.0f + i;
        rows[i].origin_vid = row_vids[i];
        CHECK(state_client_publish_monster(host, &rows[i], GetTickCount()));
    }
    pump(server, host, client, late, NULL, 100);
    for (unsigned int i = 0; i < 4u; ++i)
    {
        CHECK(state_client_monster_at(client, row_ids[i] - 1u, GetTickCount(), &observed));
        CHECK(observed.state.vid == row_vids[i] && observed.state.x == 100.0f + i);
        MpMonsterEvent row_event = event;
        row_event.sequence = 1;
        row_event.monster_id = row_ids[i];
        row_event.world_epoch = player.world_epoch;
        row_event.kind = MP_MONSTER_ATTACK;
        row_event.death_animation = 0;
        row_event.vid = row_vids[i];
        row_event.origin_vid = row_vids[i];
        CHECK(state_client_send_monster_event(host, &row_event, GetTickCount()));
    }
    pump(server, host, client, late, NULL, 100);
    for (unsigned int i = 0; i < 4u; ++i)
    {
        CHECK(state_client_take_monster_event(client, &received));
        CHECK(received.event.monster_id == row_ids[i] && received.event.sequence == 1u);
    }
    CHECK(!state_client_take_monster_event(client, &received));
    StateClient* rows_late = state_client_create("127.0.0.1", port, "RowsLate", NULL);
    CHECK(rows_late);
    pump(server, host, client, rows_late, NULL, 300);
    for (unsigned int i = 0; i < 4u; ++i)
    {
        CHECK(state_client_monster_at(rows_late, row_ids[i] - 1u, GetTickCount(), &observed));
        CHECK(observed.state.vid == row_vids[i]);
    }
    state_client_destroy(rows_late);
    rows[0].alive = 0;
    rows[0].health = 0;
    rows[0].death_animation = 15;
    ++rows[0].sequence;
    CHECK(state_client_publish_monster(host, &rows[0], GetTickCount()));
    pump(server, host, client, late, NULL, 50);
    CHECK(state_client_monster_at(client, row_ids[0] - 1u, GetTickCount(), &observed) &&
          !observed.state.alive);
    CHECK(state_client_monster_at(client, row_ids[1] - 1u, GetTickCount(), &observed) &&
          observed.state.alive);
    state_client_destroy(host);
    host = NULL;
    pump(server, NULL, client, late, NULL, 1100);
    CHECK(!state_client_monster(client, GetTickCount(), &observed));
    host = state_client_create("127.0.0.1", port, "Rejoined", NULL);
    CHECK(host);
    state_client_monster_claim(host);
    pump(server, host, client, late, NULL, 800);
    CHECK(state_client_monster_authority(host, NULL, NULL, &is_local) && !is_local);
    CHECK(!state_client_publish_monster(
        host, &monster, GetTickCount())); /* Same slot, new session has no authority. */
    state_client_destroy(host);
    state_client_destroy(client);
    state_client_destroy(late);
    pump(server, NULL, NULL, NULL, NULL, 150);

    /* Once the room empties a new raw owner proves the server's checks directly,
       including packets that a production client deliberately refuses to send. */
    raw = raw_join(server, NULL, NULL, port);
    raw_publish_player = 1;
    raw_player_sequence = raw_player_at = 0;
    uint8_t claim[8];
    mp_put_u32(claim, MP_MONSTER_VERSION);
    mp_put_u32(claim + 4, raw_session);
    epnet_client_send(raw, MPT_C_MONSTER_CLAIM, claim, sizeof(claim));
    client = state_client_create("127.0.0.1", port, "Observer", NULL);
    CHECK(client);
    DWORD wait_started = GetTickCount();
    PeerState raw_owner;
    do
    {
        pump(server, NULL, client, NULL, raw, 10);
        if (state_client_monster_authority(client, &authority_id, &authority_session, &is_local) &&
            authority_id == epnet_client_get_id(raw) && authority_session == raw_session &&
            !is_local && state_client_peer_info(client, authority_id, GetTickCount(), &raw_owner) &&
            raw_owner.session == raw_session && raw_owner.state.world_epoch == player.world_epoch)
            break;
    } while (GetTickCount() - wait_started < 2000u);
    CHECK(state_client_monster_authority(client, &authority_id, &authority_session, &is_local) &&
          authority_id == epnet_client_get_id(raw) && authority_session == raw_session &&
          !is_local);
    CHECK(state_client_peer_info(client, authority_id, GetTickCount(), &raw_owner) &&
          raw_owner.session == raw_session);
    raw_state(raw, raw_session + 1u, &monster);
    pump(server, NULL, client, NULL, raw, 100);
    CHECK(!state_client_monster(client, GetTickCount(), &observed));
    --monster.world_epoch;
    raw_state(raw, raw_session, &monster);
    pump(server, NULL, client, NULL, raw, 100);
    CHECK(!state_client_monster(client, GetTickCount(), &observed));
    ++monster.world_epoch;
    raw_state(raw, raw_session, &monster);
    wait_started = GetTickCount();
    do
    {
        pump(server, NULL, client, NULL, raw, 10);
        if (state_client_monster(client, GetTickCount(), &observed))
            break;
    } while (GetTickCount() - wait_started < 2000u);
    CHECK(state_client_monster(client, GetTickCount(), &observed) &&
          observed.authority_session == raw_session);

    /* Real relay progression: a newer packet is not permission to undo a form. */
    MpMonsterState form = monster, bad;
    form.monster_id = 62;
    form.vid = form.origin_vid = 50;
    form.form_revision = 1;
    form.sequence = 1;
    form.form_phase = MP_MONSTER_FORM_ACTIVE;
    accept_raw_state(server, client, raw, &form);
    MpMonsterEvent form_event = {0};
    form_event.world_low = form.world_low;
    form_event.world_high = form.world_high;
    form_event.world_epoch = form.world_epoch;
    form_event.monster_id = form.monster_id;
    form_event.sequence = 1;
    form_event.kind = MP_MONSTER_FORM;
    form_event.vid = 59;
    form_event.origin_vid = 50;
    form_event.form_revision = 2;
    form_event.from_vid = 50;
    raw_event(raw, &form_event);
    memset(&received, 0, sizeof(received));
    wait_started = GetTickCount();
    do
    {
        pump(server, NULL, client, NULL, raw, 10);
        if (state_client_take_monster_event(client, &received))
            break;
    } while (GetTickCount() - wait_started < 1000u);
    CHECK(received.event.monster_id == 62 && received.event.kind == MP_MONSTER_FORM &&
          received.event.form_revision == 2);
    /* An attack already queued is discarded when a later snapshot freezes its actor. */
    form_event.sequence = 2;
    form_event.kind = MP_MONSTER_ATTACK;
    form_event.vid = 50;
    form_event.form_revision = 1;
    form_event.from_vid = 0;
    raw_event(raw, &form_event);
    pump(server, NULL, client, NULL, raw, 60);
    form.vid = 59;
    form.form_revision = 2;
    ++form.sequence;
    accept_raw_state(server, client, raw, &form);
    CHECK(!state_client_take_monster_event(client, &received));
    ++form_event.sequence;
    raw_event(raw, &form_event);
    pump(server, NULL, client, NULL, raw, 60);
    CHECK(!state_client_take_monster_event(client, &received));
    bad = form;
    ++bad.sequence;
    bad.form_revision = 1;
    reject_raw_state(server, client, raw, &bad, &form);
    bad.form_revision = 2;
    bad.vid = 50;
    reject_raw_state(server, client, raw, &bad, &form);
    bad.form_revision = 3;
    reject_raw_state(server, client, raw, &bad, &form);
    bad = form;
    ++bad.sequence;
    bad.origin_vid = 60;
    reject_raw_state(server, client, raw, &bad, &form);
    form.alive = 0;
    form.health = 0;
    form.death_animation = 16;
    ++form.sequence;
    accept_raw_state(server, client, raw, &form);
    bad = form;
    ++bad.sequence;
    bad.alive = 1;
    bad.health = 90;
    bad.death_animation = 0;
    reject_raw_state(server, client, raw, &bad, &form);

    form = monster;
    form.monster_id = 63;
    form.vid = form.origin_vid = 70;
    form.sequence = 1;
    form.form_revision = 1;
    form.form_phase = MP_MONSTER_FORM_ACTIVE;
    accept_raw_state(server, client, raw, &form);
    form_event.monster_id = 63;
    form_event.sequence = 1;
    form_event.kind = MP_MONSTER_FORM;
    form_event.vid = form_event.origin_vid = form_event.from_vid = 70;
    form_event.form_revision = 2;
    form_event.form_phase = MP_MONSTER_FORM_ARMOR;
    form_event.death_animation = 15;
    raw_event(raw, &form_event);
    memset(&received, 0, sizeof(received));
    wait_started = GetTickCount();
    do
    {
        pump(server, NULL, client, NULL, raw, 10);
        if (state_client_take_monster_event(client, &received))
            break;
    } while (GetTickCount() - wait_started < 1000u);
    CHECK(received.event.monster_id == 63 && received.event.form_phase == MP_MONSTER_FORM_ARMOR);
    form.form_revision = 2;
    form.form_phase = MP_MONSTER_FORM_ARMOR;
    form.health = 1;
    form.moving = 0;
    form.velocity = 0;
    ++form.sequence;
    accept_raw_state(server, client, raw, &form);
    StateClient* armor_late = state_client_create("127.0.0.1", port, "ArmorLate", NULL);
    CHECK(armor_late);
    wait_started = GetTickCount();
    do
    {
        pump(server, NULL, client, armor_late, raw, 10);
        if (state_client_monster_at(armor_late, 62, GetTickCount(), &observed))
            break;
    } while (GetTickCount() - wait_started < 1000u);
    CHECK(state_client_monster_at(armor_late, 62, GetTickCount(), &observed) &&
          observed.state.form_phase == MP_MONSTER_FORM_ARMOR && observed.state.form_revision == 2);
    CHECK(!state_client_take_monster_event(
        armor_late, &received)); /* Pending armor survives lost events/late join. */
    state_client_destroy(armor_late);
    form_event.sequence = 2;
    form_event.kind = MP_MONSTER_ATTACK;
    form_event.from_vid = 0;
    form_event.form_phase = MP_MONSTER_FORM_ACTIVE;
    form_event.death_animation = 0;
    raw_event(raw, &form_event);
    pump(server, NULL, client, NULL, raw, 60);
    CHECK(!state_client_take_monster_event(client, &received)); /* Same revision, wrong phase. */
    form_event.sequence = 3;
    form_event.vid = 60;
    form_event.form_revision = 3;
    raw_event(raw, &form_event);
    pump(server, NULL, client, NULL, raw, 60);
    CHECK(!state_client_take_monster_event(client,
                                           &received)); /* Future attacks need matching snapshot. */
    bad = form;
    bad.sequence++;
    bad.form_revision++;
    bad.form_phase = MP_MONSTER_FORM_ACTIVE;
    reject_raw_state(server, client, raw, &bad, &form);
    form.sequence++;
    form.form_revision++;
    form.form_phase = MP_MONSTER_FORM_ACTIVE;
    form.vid = 60;
    form.health = 90;
    accept_raw_state(server, client, raw, &form);
    bad = form;
    ++bad.sequence;
    bad.origin_vid = 60;
    reject_raw_state(server, client, raw, &bad,
                     &form); /* Both forms decode, but origin is immutable. */
    raw_event(raw, &form_event);
    memset(&received, 0, sizeof(received));
    wait_started = GetTickCount();
    do
    {
        pump(server, NULL, client, NULL, raw, 10);
        if (state_client_take_monster_event(client, &received))
            break;
    } while (GetTickCount() - wait_started < 1000u);
    CHECK(received.event.monster_id == 63 && received.event.kind == MP_MONSTER_ATTACK &&
          received.event.sequence == 3);
    /* Rejecting future/stale form context must not consume the event sequence. */
    bad = form;
    ++bad.sequence;
    ++bad.form_revision;
    bad.vid = 70;
    bad.form_phase = MP_MONSTER_FORM_ARMOR;
    bad.health = 1;
    reject_raw_state(server, client, raw, &bad, &form);
    form.sequence++;
    form.form_revision++;
    form.vid = 69;
    accept_raw_state(server, client, raw, &form);
    bad = form;
    ++bad.sequence;
    ++bad.form_revision;
    bad.vid = 79;
    reject_raw_state(server, client, raw, &bad, &form);
    form = monster;
    form.monster_id = 64;
    form.vid = form.origin_vid = 90;
    form.sequence = 1;
    form.form_revision = 1;
    accept_raw_state(server, client, raw, &form);
    form_event.monster_id = 64;
    form_event.sequence = 1;
    form_event.kind = MP_MONSTER_FORM;
    form_event.vid = 89;
    form_event.origin_vid = form_event.from_vid = 90;
    form_event.form_revision = 4;
    form_event.form_phase = MP_MONSTER_FORM_ACTIVE;
    form_event.death_animation = 0;
    raw_event(raw, &form_event);
    memset(&received, 0, sizeof(received));
    wait_started = GetTickCount();
    do
    {
        pump(server, NULL, client, NULL, raw, 10);
        if (state_client_take_monster_event(client, &received))
            break;
    } while (GetTickCount() - wait_started < 1000u);
    CHECK(received.event.monster_id == 64 && received.event.kind == MP_MONSTER_FORM &&
          received.event.vid == 89);
    form.vid = 89;
    form.sequence++;
    form.form_revision = 4;
    accept_raw_state(server, client, raw, &form); /* Lost armor, unarmored and freeze snapshots. */
    epnet_client_disconnect(raw);
    epnet_client_destroy(raw);
    state_client_destroy(client);
    mp_server_destroy(server);
    puts("Monster transport authority, forms, session/world validation, explicit events, dedup, "
         "late join and expiry checks passed.");
    return 0;
}
