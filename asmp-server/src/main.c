#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include "utils/time/time.h"

#ifndef EZTEST

int main(int argc, char* argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2)
    {
        MpServer* server = mp_server_create(atoi(argv[1]), 4);
        if (server)
        {
            while (1)
            {
                mp_server_tick(server);
                time_sleep_ms(1);
            }
            mp_server_destroy(server);
        }
    }
    else
    {
        printf("Usage: %s <port>\n", argv[0]);
    }
    return 0;
}

#endif /* EZTEST */
