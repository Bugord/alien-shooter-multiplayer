#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "state_client.h"
int main(int argc, char** argv) {
    if (argc != 4 && (argc < 5 || argc > 6 || strcmp(argv[4], "--mirror") || (argc == 6 && strcmp(argv[5], "--fire-once")))) { fprintf(stderr, "Usage: state-peer <host> <port> <seconds> [--mirror [--fire-once]]\n"); return 1; }
    char* end;
    unsigned long port = strtoul(argv[2], &end, 10);
    if (*end || !port || port > 65535) return 1;
    unsigned long seconds = strtoul(argv[3], &end, 10);
    if (*end || !seconds || seconds > 3600) return 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    int mirror = argc >= 5, fired = 0;
    DWORD active_since = 0;
    StateClient* c = state_client_create(argv[1], (unsigned short)port, mirror ? "Mirror" : "Observer", stdout);
    if (!c) return 2;
    DWORD started = GetTickCount();
    while (GetTickCount() - started < seconds * 1000) {
        DWORD now = GetTickCount();
        state_client_update(c, now);
        if (mirror) {
            MpSteamState state = {0};
            for (unsigned int id = 0; id < STEAM_MAX_PEERS; ++id) {
                if (state_client_peer(c, id, now, &state)) { state.x += 80.0f; break; }
            }
            state_client_publish(c, &state, now);
            if (state.active && state.world_epoch) {
                if (!active_since) active_since = now;
                if (argc == 6 && !fired && now - active_since >= 6000u) {
                    MpSteamShot test = {0}; test.world_low = state.world_low; test.world_high = state.world_high;
                    test.world_epoch = state.world_epoch; test.weapon = 1;
                    test.x = (int)state.x + 200; test.y = (int)state.y;
                    fired = state_client_send_shot(c, &test, now);
                    if (fired) puts("# MIRROR native shot requested once");
                }
            }
            SteamShotEvent event;
            while (state_client_take_shot(c, &event)) state_client_send_shot(c, &event.shot, now);
        }
        Sleep(5);
    }
    state_client_stats(c);
    state_client_destroy(c);
    return 0;
}
