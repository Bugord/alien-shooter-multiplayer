#ifndef ASMP_DIAG_TICK_HOOK_H
#define ASMP_DIAG_TICK_HOOK_H
#include <windows.h>
#include "probe.h"

#define FRAME_QUEUE_CAPACITY 1024u
typedef struct FrameSample {
    DWORD milliseconds;
    LONG tick;
    enum ProbeResult result;
    Snapshot snapshot;
} FrameSample;
typedef struct TickStats {
    LONG calls, captured, dropped, installed;
} TickStats;
enum TickHookResult { TICK_HOOK_OK, TICK_HOOK_ALREADY_USED, TICK_HOOK_INVALID_SLOT,
    TICK_HOOK_PROTECT_FAILED, TICK_HOOK_SLOT_CHANGED, TICK_HOOK_PROTECTION_RESTORE_FAILED };

/* One installation per process. The DLL must stay loaded even after stopping:
   another thread may already have fetched the old hook pointer from the vtable. */
enum TickHookResult tick_hook_install(void* volatile* slot, void* expected, uintptr_t image_base);
enum TickHookResult tick_hook_stop(void);
unsigned int tick_hook_drain(FrameSample* output, unsigned int capacity);
TickStats tick_hook_stats(void);
#endif
