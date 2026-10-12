/* Stand-in relay server for server_process_test: behaves according to the
   ASMP_FAKE_SERVER_MODE environment variable and the port argument. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOISE_LINES 4000

int main(int argc, char* argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char* port = argc > 1 ? argv[1] : "0";
    char mode[32] = "";
    size_t length = 0;
    getenv_s(&length, mode, sizeof(mode), "ASMP_FAKE_SERVER_MODE");

    if (!strcmp(mode, "ready"))
    {
        printf("ASMP_SERVER_READY port=%s\n", port);
        Sleep(INFINITE);
    }
    if (!strcmp(mode, "noisy"))
    {
        /* Far more than a pipe buffer: finishes only if the parent keeps reading. */
        printf("ASMP_SERVER_READY port=%s\n", port);
        for (int i = 0; i < NOISE_LINES; ++i)
            printf("Client %d joined (noise line to fill the pipe)\n", i);
        return 0;
    }
    if (!strcmp(mode, "inuse"))
    {
        printf("ASMP_SERVER_PORT_IN_USE port=%s\n", port);
        return 2;
    }
    if (!strcmp(mode, "exitcode"))
        return 2;
    if (!strcmp(mode, "silent"))
        Sleep(INFINITE);
    return 1;
}
