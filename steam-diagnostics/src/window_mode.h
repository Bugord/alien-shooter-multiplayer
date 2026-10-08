#ifndef ASMP_WINDOW_MODE_H
#define ASMP_WINDOW_MODE_H
#include <windows.h>
#include <stdint.h>
/* Only call before the new process executes its render constructor. */
int window_mode_limit_width(HANDLE process, uintptr_t image, unsigned int width);
int window_mode_render_size(HANDLE process, uintptr_t image, int* width, int* height);
#endif
