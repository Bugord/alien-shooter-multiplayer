/* The relay server process a hosting player starts: launch hidden, wait for its
   readiness line, keep its output drained, and stop it. Layer: multiplayer/
   (production orchestration). Worker thread only.

   Ownership: the struct owns the process, its job object and the read end of its
   output pipe. Only the process started here is ever terminated, through its own
   handle; a server someone else started on the same port is never touched (it makes
   ours report SERVER_PORT_IN_USE instead). The job object kills the server when
   the game process ends, even after a crash. */

#ifndef ASMP_SERVER_PROCESS_H
#define ASMP_SERVER_PROCESS_H

#include <windows.h>
#include <stdio.h>

/* Readiness must arrive within this time after the launch. */
#define SERVER_READY_TIMEOUT_MS 5000u

enum ServerProcessState
{
    SERVER_IDLE = 0,     /* Nothing started, or stopped. */
    SERVER_STARTING,     /* Launched; waiting for the readiness line. */
    SERVER_READY,        /* Listening on the port. */
    SERVER_MISSING,      /* The executable could not be started. */
    SERVER_PORT_IN_USE,  /* Another socket holds the port. */
    SERVER_EXITED,       /* Ended before or after becoming ready. */
    SERVER_READY_TIMEOUT /* No readiness line in time; the process was stopped. */
};

typedef struct ServerProcess
{
    enum ServerProcessState state;
    HANDLE process;
    HANDLE job;
    HANDLE output; /* Read end of the child's stdout/stderr pipe. */
    DWORD pid;
    FILETIME created;
    DWORD started_at;
    unsigned short port;
    char line[128]; /* Partial output line being assembled. */
    unsigned int line_length;
} ServerProcess;

/* Writes "<directory of this module>\asmp-server.exe". Returns 0 if it does not fit. */
int server_process_default_path(wchar_t* out, DWORD capacity);

/* Launches `path <port>`. Returns 1 when the process started (state SERVER_STARTING),
   0 with state SERVER_MISSING otherwise. `server` must be idle. */
int server_process_start(ServerProcess* server, const wchar_t* path, unsigned short port, DWORD now,
                         FILE* log);

/* Drains output and advances the state; call every worker step. */
enum ServerProcessState server_process_poll(ServerProcess* server, DWORD now, FILE* log);

/* Terminates our process if it still runs and releases every handle. Safe to call in
   any state; afterwards the state is SERVER_IDLE. */
void server_process_stop(ServerProcess* server, FILE* log);

#endif
