/* Entry point of the relay server: `asmp-server <port>` runs mp_server_tick every
   millisecond until the process is killed. Layer: asmp-server (transport and relay).
   The first stdout line tells a launching game whether the server is usable:
   "ASMP_SERVER_READY port=N", "ASMP_SERVER_PORT_IN_USE port=N" or
   "ASMP_SERVER_FAILED port=N" (server_process.c in the DLL reads it). */

#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include "utils/time/time.h"

#define SERVER_MAX_CLIENTS 4

enum ServerExitCode
{
    SERVER_EXIT_OK = 0,
    SERVER_EXIT_FAILED = 1,
    SERVER_EXIT_PORT_IN_USE = 2,
    SERVER_EXIT_USAGE = 3
};

/* Parses a port 1..65535; returns 0 for anything else. */
static unsigned short parse_port(const char* text)
{
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end || value < 1 || value > 65535)
        return 0;
    return (unsigned short)value;
}

int main(int argc, char* argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 2 || !parse_port(argv[1]))
    {
        printf("Usage: %s <port>\n", argv[0]);
        return SERVER_EXIT_USAGE;
    }

    unsigned short port = parse_port(argv[1]);
    if (!mp_server_port_free(port))
    {
        printf("ASMP_SERVER_PORT_IN_USE port=%u\n", port);
        return SERVER_EXIT_PORT_IN_USE;
    }

    MpServer* server = mp_server_create(port, SERVER_MAX_CLIENTS);
    if (!server)
    {
        printf("ASMP_SERVER_FAILED port=%u\n", port);
        return SERVER_EXIT_FAILED;
    }

    printf("ASMP_SERVER_READY port=%u\n", port);
    while (1)
    {
        mp_server_tick(server);
        time_sleep_ms(1);
    }
    mp_server_destroy(server);
    return SERVER_EXIT_OK;
}
