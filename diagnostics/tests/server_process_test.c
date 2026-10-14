/* Checks for asmp-dll/src/multiplayer/server_process.c with a stand-in child
   (server-process-child.exe) and, when its path is given as the first argument, the
   real asmp-server.exe: readiness, port conflict, exits, timeout, output draining and
   stopping only our own process. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../asmp-dll/src/multiplayer/server_process.h"

#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define TEST_PORT     27999
#define POLL_LIMIT_MS (SERVER_READY_TIMEOUT_MS + 3000u)
#define PATH_CAPACITY (MAX_PATH + 1)

static wchar_t child_path[PATH_CAPACITY];

/* Polls until the state leaves `from` or the limit passes; returns the last state. */
static enum ServerProcessState poll_while(ServerProcess* server, enum ServerProcessState from)
{
    DWORD start = GetTickCount();
    enum ServerProcessState state = server->state;
    while (state == from && GetTickCount() - start < POLL_LIMIT_MS)
    {
        Sleep(5);
        state = server_process_poll(server, GetTickCount(), NULL);
    }
    return state;
}

static enum ServerProcessState run_child(const char* mode, ServerProcess* server)
{
    SetEnvironmentVariableA("ASMP_FAKE_SERVER_MODE", mode);
    memset(server, 0, sizeof(*server));
    CHECK(server_process_start(server, child_path, TEST_PORT, GetTickCount(), NULL));
    CHECK(server->state == SERVER_STARTING && server->process != NULL);
    return poll_while(server, SERVER_STARTING);
}

/* Opens a separate handle so the test can see the process end after stop. */
static HANDLE watch(const ServerProcess* server)
{
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, server->pid);
    CHECK(handle != NULL);
    return handle;
}

static void check_stand_in(void)
{
    ServerProcess server;
    memset(&server, 0, sizeof(server));
    CHECK(!server_process_start(&server, L"Z:\\missing\\asmp-server.exe", TEST_PORT, 0, NULL));
    CHECK(server.state == SERVER_MISSING && !server.process && !server.output);
    server_process_stop(&server, NULL);
    CHECK(server.state == SERVER_IDLE);

    CHECK(run_child("ready", &server) == SERVER_READY);
    CHECK(!server_process_start(&server, child_path, TEST_PORT, 0, NULL)); /* Still owned. */
    HANDLE ready = watch(&server);
    server_process_stop(&server, NULL);
    CHECK(server.state == SERVER_IDLE && !server.process && !server.job);
    CHECK(WaitForSingleObject(ready, 1000) == WAIT_OBJECT_0);
    CloseHandle(ready);

    CHECK(run_child("noisy", &server) == SERVER_READY);
    CHECK(poll_while(&server, SERVER_READY) == SERVER_EXITED); /* Drained to the end. */
    server_process_stop(&server, NULL);

    CHECK(run_child("inuse", &server) == SERVER_PORT_IN_USE);
    server_process_stop(&server, NULL);
    CHECK(run_child("exitcode", &server) == SERVER_PORT_IN_USE);
    server_process_stop(&server, NULL);
    CHECK(run_child("crash", &server) == SERVER_EXITED);
    server_process_stop(&server, NULL);

    CHECK(run_child("silent", &server) == SERVER_READY_TIMEOUT);
    CHECK(!server.process && !server.output && !server.job);
}

/* Two real servers on one port: the second reports the conflict and stopping it
   leaves the first one running. */
static void check_real_server(const wchar_t* path)
{
    ServerProcess first;
    ServerProcess second;
    unsigned short port = (unsigned short)(41000u + GetCurrentProcessId() % 8000u);
    enum ServerProcessState state = SERVER_IDLE;

    for (unsigned int attempt = 0; attempt < 16u && state != SERVER_READY; ++attempt)
    {
        port = (unsigned short)(port + 101u);
        memset(&first, 0, sizeof(first));
        CHECK(server_process_start(&first, path, port, GetTickCount(), NULL));
        state = poll_while(&first, SERVER_STARTING);
        if (state != SERVER_READY)
            server_process_stop(&first, NULL);
    }
    CHECK(state == SERVER_READY);

    memset(&second, 0, sizeof(second));
    CHECK(server_process_start(&second, path, port, GetTickCount(), NULL));
    CHECK(poll_while(&second, SERVER_STARTING) == SERVER_PORT_IN_USE);
    server_process_stop(&second, NULL);

    Sleep(50);
    CHECK(server_process_poll(&first, GetTickCount(), NULL) == SERVER_READY);
    HANDLE running = watch(&first);
    CHECK(WaitForSingleObject(running, 0) == WAIT_TIMEOUT);
    server_process_stop(&first, NULL);
    CHECK(WaitForSingleObject(running, 1000) == WAIT_OBJECT_0);
    CloseHandle(running);
}

int main(int argc, char* argv[])
{
    DWORD length = GetModuleFileNameW(NULL, child_path, PATH_CAPACITY);
    CHECK(length && length < PATH_CAPACITY);
    wchar_t* slash = wcsrchr(child_path, L'\\');
    CHECK(slash != NULL);
    wcscpy_s(slash + 1, PATH_CAPACITY - (size_t)(slash + 1 - child_path),
             L"server-process-child.exe");

    wchar_t default_path[PATH_CAPACITY];
    CHECK(server_process_default_path(default_path, PATH_CAPACITY));
    CHECK(wcsstr(default_path, L"\\asmp-server.exe") != NULL);
    CHECK(!server_process_default_path(default_path, 4));

    check_stand_in();
    if (argc > 1)
    {
        wchar_t real_path[PATH_CAPACITY];
        CHECK(MultiByteToWideChar(CP_ACP, 0, argv[1], -1, real_path, PATH_CAPACITY) > 0);
        check_real_server(real_path);
        puts("Server process checks passed: stand-in and real server (port conflict keeps the "
             "first server).");
        return 0;
    }
    puts("Server process checks passed: missing, ready, drained output, port in use, exit, "
         "timeout and stop of our own process.");
    return 0;
}
