#ifndef ASMP_CLOCK_H
#define ASMP_CLOCK_H
#include <windows.h>

/* Millisecond clock shared by the game thread and the worker. GetTickCount
   only advances in ~15.6 ms steps, which turns evenly spaced packets and game
   ticks into bursts; the performance counter keeps both threads on one
   precise timeline. Only differences are meaningful. */
static __inline DWORD clock_ms(void)
{
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&counter);
    return (DWORD)((unsigned long long)counter.QuadPart * 1000ull / (unsigned long long)frequency.QuadPart);
}
#endif
