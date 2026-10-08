#ifndef ASMP_DIAG_TICK_H
#define ASMP_DIAG_TICK_H
#include <windows.h>
#include "probe.h"
#include "dummy_actor.h"
#include "../../asmp-dll/src/multiplayer/runtime.h"

#define FRAME_QUEUE_CAPACITY 1024u
typedef struct FrameSample {
    DWORD milliseconds;
    LONG tick;
    int session_result;
    enum ProbeResult result;
    Snapshot snapshot;
    ActorResult actor;
    MultiplayerFrame multiplayer;
    enum SessionState session;
    enum DisplayResult display;
} FrameSample;
typedef struct TickStats {
    LONG calls, captured, dropped, installed;
} TickStats;
enum DiagTickResult { DIAG_TICK_OK, DIAG_TICK_ALREADY_USED, DIAG_TICK_INVALID_SLOT,
    DIAG_TICK_PROTECT_FAILED, DIAG_TICK_SLOT_CHANGED, DIAG_TICK_PROTECTION_RESTORE_FAILED, DIAG_TICK_BUSY };

/* One active installation. The DLL must stay loaded even after stopping:
   another thread may already have fetched the old hook pointer from the vtable. */
enum DiagTickResult diag_tick_install(void* volatile* slot, void* expected, uintptr_t image_base);
enum DiagTickResult diag_tick_stop(void);
unsigned int diag_tick_drain(FrameSample* output, unsigned int capacity);
TickStats diag_tick_stats(void);
int diag_tick_enable_dummy(uintptr_t image_base);
void diag_tick_request_dummy_stop(void);
int diag_tick_dummy_stopped(void);
void diag_tick_prepare_observer(void);
void diag_tick_observe(const RuntimeFrame*);
#endif
