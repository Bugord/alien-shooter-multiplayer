/* Starts and stops the relay server for a hosting player. The server runs hidden in a
   job object tied to the game process, reports readiness on its first output line
   (see asmp-server/src/main.c) and is stopped only through the handle created here.
   Layer: multiplayer/ (production orchestration). Worker thread only. */

#include <string.h>
#include <wchar.h>
#include "server_process.h"

#define SERVER_EXE_NAME       L"asmp-server.exe"
#define SERVER_STOP_WAIT_MS   2000u
#define SERVER_COMMAND_LENGTH (MAX_PATH + 16)
#define SERVER_READY_PREFIX   "ASMP_SERVER_READY "
#define SERVER_IN_USE_PREFIX  "ASMP_SERVER_PORT_IN_USE "

/* Matches asmp-server's exit code for a port held by another socket. */
#define SERVER_EXIT_PORT_IN_USE 2u

int server_process_default_path(wchar_t* out, DWORD capacity)
{
    HMODULE module = NULL;
    DWORD flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (!GetModuleHandleExW(flags, (LPCWSTR)(void*)server_process_default_path, &module))
        return 0;

    DWORD length = GetModuleFileNameW(module, out, capacity);
    if (!length || length >= capacity)
        return 0;

    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash)
        return 0;

    size_t directory = (size_t)(slash - out) + 1;
    if (directory + wcslen(SERVER_EXE_NAME) + 1 > capacity)
        return 0;
    wcscpy_s(slash + 1, capacity - directory, SERVER_EXE_NAME);
    return 1;
}

/* A job that kills its processes when its last handle closes, so the server cannot
   outlive the game. Returns NULL when jobs are unavailable. */
static HANDLE create_kill_on_close_job(void)
{
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job)
        return NULL;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    memset(&limits, 0, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    {
        CloseHandle(job);
        return NULL;
    }
    return job;
}

/* Creates the output pipe; only the write end is inheritable. */
static int create_output_pipe(HANDLE* read_end, HANDLE* write_end)
{
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
    if (!CreatePipe(read_end, write_end, &inherit, 0))
        return 0;
    SetHandleInformation(*read_end, HANDLE_FLAG_INHERIT, 0);
    return 1;
}

/* Launches the child suspended with exactly one inherited handle (the pipe), so no
   other game handle leaks into it. Fills `info` on success. */
static int launch_suspended(const wchar_t* path, unsigned short port, HANDLE pipe,
                            PROCESS_INFORMATION* info)
{
    wchar_t command[SERVER_COMMAND_LENGTH];
    if (swprintf(command, SERVER_COMMAND_LENGTH, L"\"%s\" %u", path, (unsigned int)port) < 0)
        return 0;

    SIZE_T size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = HeapAlloc(GetProcessHeap(), 0, size);
    if (!attributes)
        return 0;

    int launched = 0;
    if (InitializeProcThreadAttributeList(attributes, 1, 0, &size))
    {
        HANDLE inherited[1] = {pipe};
        STARTUPINFOEXW startup;
        memset(&startup, 0, sizeof(startup));
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = pipe;
        startup.StartupInfo.hStdError = pipe;
        startup.lpAttributeList = attributes;

        if (UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                      sizeof(inherited), NULL, NULL))
        {
            DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
            launched = CreateProcessW(path, command, NULL, NULL, TRUE, flags, NULL, NULL,
                                      &startup.StartupInfo, info) != 0;
        }
        DeleteProcThreadAttributeList(attributes);
    }
    HeapFree(GetProcessHeap(), 0, attributes);
    return launched;
}

/* Releases every handle without terminating anything. */
static void close_handles(ServerProcess* server)
{
    if (server->output)
        CloseHandle(server->output);
    if (server->process)
        CloseHandle(server->process);
    if (server->job)
        CloseHandle(server->job);
    server->output = NULL;
    server->process = NULL;
    server->job = NULL;
}

int server_process_start(ServerProcess* server, const wchar_t* path, unsigned short port, DWORD now,
                         FILE* log)
{
    if (server->process || server->output || server->job)
        return 0; /* Still owns a process; stop it first. */

    memset(server, 0, sizeof(*server));
    server->state = SERVER_MISSING;
    server->port = port;
    if (!path || !port)
        return 0;

    HANDLE pipe_write = NULL;
    if (!create_output_pipe(&server->output, &pipe_write))
        return 0;

    PROCESS_INFORMATION info;
    memset(&info, 0, sizeof(info));
    int launched = launch_suspended(path, port, pipe_write, &info);
    CloseHandle(pipe_write); /* The child holds its own copy. */
    if (!launched)
    {
        close_handles(server);
        if (log)
            fprintf(log, "# SERVER_PROCESS missing error=%lu\n", GetLastError());
        return 0;
    }

    server->process = info.hProcess;
    server->pid = info.dwProcessId;
    server->job = create_kill_on_close_job();
    int in_job = server->job && AssignProcessToJobObject(server->job, info.hProcess);
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);

    FILETIME exited, kernel, user;
    GetProcessTimes(info.hProcess, &server->created, &exited, &kernel, &user);
    server->started_at = now;
    server->state = SERVER_STARTING;
    if (log)
        fprintf(log, "# SERVER_PROCESS started pid=%lu port=%u job=%d created=%08lx%08lx\n",
                server->pid, port, in_job, server->created.dwHighDateTime,
                server->created.dwLowDateTime);
    return 1;
}

/* Handles one complete output line: the first status line decides readiness;
   later lines are forwarded to the log. */
static void handle_line(ServerProcess* server, const char* line, FILE* log)
{
    if (server->state == SERVER_STARTING)
    {
        if (!strncmp(line, SERVER_READY_PREFIX, strlen(SERVER_READY_PREFIX)))
            server->state = SERVER_READY;
        else if (!strncmp(line, SERVER_IN_USE_PREFIX, strlen(SERVER_IN_USE_PREFIX)))
            server->state = SERVER_PORT_IN_USE;
    }
    if (log)
        fprintf(log, "# SERVER %s\n", line);
}

/* Reads whatever output is available without blocking. The server writes unbuffered,
   so an undrained pipe would eventually block it. */
static void drain_output(ServerProcess* server, FILE* log)
{
    char buffer[256];
    DWORD available = 0;
    while (server->output && PeekNamedPipe(server->output, NULL, 0, NULL, &available, NULL) &&
           available)
    {
        DWORD wanted = available < sizeof(buffer) ? available : (DWORD)sizeof(buffer);
        DWORD received = 0;
        if (!ReadFile(server->output, buffer, wanted, &received, NULL) || !received)
            return;

        for (DWORD i = 0; i < received; ++i)
        {
            char c = buffer[i];
            if (c == '\r')
                continue;
            if (c == '\n' || server->line_length + 1 >= sizeof(server->line))
            {
                server->line[server->line_length] = 0;
                handle_line(server, server->line, log);
                server->line_length = 0;
                continue;
            }
            server->line[server->line_length++] = c;
        }
    }
}

/* Reports how the process ended; a port conflict may be known only from the exit code. */
static enum ServerProcessState exited_state(const ServerProcess* server)
{
    DWORD code = 0;
    if (server->state == SERVER_PORT_IN_USE)
        return SERVER_PORT_IN_USE;
    if (GetExitCodeProcess(server->process, &code) && code == SERVER_EXIT_PORT_IN_USE)
        return SERVER_PORT_IN_USE;
    return SERVER_EXITED;
}

enum ServerProcessState server_process_poll(ServerProcess* server, DWORD now, FILE* log)
{
    if (server->state != SERVER_STARTING && server->state != SERVER_READY)
        return server->state;

    drain_output(server, log);

    if (WaitForSingleObject(server->process, 0) == WAIT_OBJECT_0)
    {
        drain_output(server, log);
        server->state = exited_state(server);
        if (log)
            fprintf(log, "# SERVER_PROCESS ended pid=%lu state=%d\n", server->pid, server->state);
        return server->state;
    }

    if (server->state == SERVER_STARTING && now - server->started_at >= SERVER_READY_TIMEOUT_MS)
    {
        server_process_stop(server, log);
        server->state = SERVER_READY_TIMEOUT;
    }
    return server->state;
}

void server_process_stop(ServerProcess* server, FILE* log)
{
    if (server->process && WaitForSingleObject(server->process, 0) == WAIT_TIMEOUT)
    {
        /* Our own handle, so this can never reach another server process. */
        TerminateProcess(server->process, 0);
        WaitForSingleObject(server->process, SERVER_STOP_WAIT_MS);
        if (log)
            fprintf(log, "# SERVER_PROCESS stopped pid=%lu\n", server->pid);
    }
    close_handles(server);
    server->state = SERVER_IDLE;
    server->line_length = 0;
}
