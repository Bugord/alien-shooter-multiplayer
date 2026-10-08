#include "mem.h"
#include <stdlib.h>
#include <string.h>

#if defined DEBUG_
static int _allocs;
static int _reallocs;
static int _frees;
#endif /* DEBUG_ */

void* mem_alloc(unsigned int size)
{
#if defined DEBUG_
    ++_allocs;
#endif /* DEBUG_ */
    return malloc(size);
}

void* mem_realloc(void* address, unsigned int new_size)
{
#if defined DEBUG_
    ++_reallocs;
    if (address == 0)
    {
        ++_allocs;
    }
#endif /* DEBUG_ */
    return realloc(address, new_size);
}

void mem_free(void* address)
{
#if defined DEBUG_
    ++_frees;
#endif /* DEBUG_ */
    free(address);
}

void mem_copy(void* dst, const void* src, unsigned int size)
{
    memcpy(dst, src, size);
}

void mem_set(void* dst, int value, unsigned int size)
{
    memset(dst, value, size);
}

#if defined DEBUG_

int mem_debug_get_diff(void)
{
    return _allocs - _frees;
}
#endif /* DEBUG_ */
