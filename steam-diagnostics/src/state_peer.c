#include <windows.h>
#include <stdlib.h>
#include "state_client.h"
int main(int argc, char** argv) {
    if (argc != 4) { fprintf(stderr, "Usage: state-peer <host> <port> <seconds>\n"); return 1; }
    char* end;
    unsigned long port = strtoul(argv[2], &end, 10);
    if (*end || !port || port > 65535) return 1;
    unsigned long seconds = strtoul(argv[3], &end, 10);
    if (*end || !seconds || seconds > 3600) return 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    StateClient* c = state_client_create(argv[1], (unsigned short)port, "Observer", stdout);
    if (!c) return 2;
    DWORD started = GetTickCount();
    while (GetTickCount() - started < seconds * 1000) {
        state_client_update(c, GetTickCount());
        Sleep(5);
    }
    state_client_stats(c);
    state_client_destroy(c);
    return 0;
}
