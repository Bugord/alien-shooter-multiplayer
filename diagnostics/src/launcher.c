#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "profile.h"
#include "window_mode.h"

/* Resolve the address in the target process, including forwarded kernel exports. */
static uintptr_t remote_load_library(DWORD pid)
{
    FARPROC procedure = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE owner = NULL;
    wchar_t path[MAX_PATH];
    uintptr_t result = 0;
    if (!procedure || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                         (LPCWSTR)(uintptr_t)procedure, &owner)) return 0;
    if (!GetModuleFileNameW(owner, path, MAX_PATH)) return 0;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (unsigned int attempt = 0; attempt < 10; ++attempt) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
        Sleep(50);
    }
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W module = {0};
    module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot, &module)) do {
        if (!_wcsicmp(module.szModule, name)) {
            result = (uintptr_t)module.modBaseAddr + (uintptr_t)procedure - (uintptr_t)owner;
            break;
        }
    } while (Module32NextW(snapshot, &module));
    CloseHandle(snapshot);
    return result;
}

typedef struct TestWindow { DWORD pid; HWND window; } TestWindow;
static BOOL CALLBACK find_test_window(HWND window, LPARAM parameter) {
    TestWindow* test = (TestWindow*)parameter;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == test->pid && IsWindowVisible(window) && !GetWindow(window, GW_OWNER)) {
        test->window = window;
        return FALSE;
    }
    return TRUE;
}
static int size_test_window(DWORD pid, int width, int height) {
    TestWindow test = {pid, NULL};
    for (unsigned int attempt = 0; attempt < 100 && !test.window; ++attempt) {
        EnumWindows(find_test_window, (LPARAM)&test);
        if (!test.window) Sleep(50);
    }
    if (!test.window) return 0;
    LONG style = GetWindowLongW(test.window, GWL_STYLE);
    if (!(style & WS_CAPTION)) return 0; /* Fullscreen must be disabled by the engine first. */
    RECT rect = {0, 0, width, height};
    if (!AdjustWindowRectEx(&rect, (DWORD)style, GetMenu(test.window) != NULL,
        (DWORD)GetWindowLongW(test.window, GWL_EXSTYLE))) return 0;
    if (!SetWindowPos(test.window, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) return 0;
    if (!GetClientRect(test.window, &rect)) return 0;
    printf("Test window client size: %ld x %ld\n", rect.right - rect.left, rect.bottom - rect.top);
    return rect.right - rect.left == width && rect.bottom - rect.top == height;
}

int wmain(int argc, wchar_t** argv)
{
    wchar_t game[MAX_PATH], dll[MAX_PATH], directory[MAX_PATH], marker[MAX_PATH], command[MAX_PATH + 3];
    char digest[65];
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    HANDLE thread = NULL;
    void* remote = NULL;
    int result = 1;
    int debugging = 0;
    uintptr_t image = 0;
    if (argc != 3 && argc != 5) { fwprintf(stderr, L"Usage: asmp-diag-launch.exe <test-game\\AlienShooter.exe> <asmp-diag.dll> [window-width window-height]\n"); return 2; }
    int width = 0, height = 0;
    if (argc == 5) {
        wchar_t* end;
        width = (int)wcstol(argv[3], &end, 10);
        if (*end || width < 640 || width > 1920) return 2;
        height = (int)wcstol(argv[4], &end, 10);
        if (*end || height < 480 || height > 1080) return 2;
    }
    DWORD game_length = GetFullPathNameW(argv[1], MAX_PATH, game, NULL);
    DWORD dll_length = GetFullPathNameW(argv[2], MAX_PATH, dll, NULL);
    if (!game_length || game_length >= MAX_PATH || !dll_length || dll_length >= MAX_PATH) return 2;
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) { fwprintf(stderr, L"Diagnostic DLL missing.\n"); return 2; }
    wcscpy_s(directory, MAX_PATH, game);
    wchar_t* slash = wcsrchr(directory, L'\\');
    if (!slash || _wcsicmp(slash + 1, L"AlienShooter.exe")) return 2;
    *slash = 0;
    if (swprintf_s(marker, MAX_PATH, L"%s\\asmp-diag-test.marker", directory) < 0) return 2;
    if (GetFileAttributesW(marker) == INVALID_FILE_ATTRIBUTES) { fwprintf(stderr, L"Use prepare-test-game.ps1 first: test-copy marker missing.\n"); return 2; }
    if (!hash_file_sha256(game, digest) || strcmp(digest, STEAM_EXE_SHA256)) { fwprintf(stderr, L"Unsupported executable hash.\n"); return 2; }
    if (swprintf_s(command, MAX_PATH + 3, L"\"%s\"", game) < 0) return 2;
    startup.cb = sizeof(startup);
    if (!CreateProcessW(game, command, NULL, NULL, FALSE, width ? DEBUG_ONLY_THIS_PROCESS : 0,
        NULL, directory, &startup, &process)) goto done;
    printf("Started test game PID %lu\n", process.dwProcessId);
    if (width) {
        debugging = 1;
        if (!DebugSetProcessKillOnExit(FALSE)) goto done;
        DEBUG_EVENT event;
        if (!WaitForDebugEvent(&event, 10000)) goto done;
        int patched = 0;
        if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT && event.dwProcessId == process.dwProcessId) {
            image = (uintptr_t)event.u.CreateProcessInfo.lpBaseOfImage;
            patched = window_mode_limit_width(process.hProcess, image, (unsigned int)width);
            if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
        }
        if (!patched) {
            fprintf(stderr, "Render startup signature or memory update failed; test process stopped.\n");
            TerminateProcess(process.hProcess, 1);
        }
        if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE)) goto done;
        if (!DebugActiveProcessStop(process.dwProcessId)) goto done;
        debugging = 0;
        if (!patched) goto done;
        printf("Render mode width cap: %d (process memory only)\n", width);
    }
    WaitForInputIdle(process.hProcess, 30000);
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process.hProcess, &exit_code) || exit_code != STILL_ACTIVE) {
        fprintf(stderr, "Game exited before DLL loading (code %lu). Check Steam and the game's logs.\n", exit_code);
        goto done;
    }
    SIZE_T size = (wcslen(dll) + 1) * sizeof(wchar_t);
    remote = VirtualAllocEx(process.hProcess, NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(process.hProcess, remote, dll, size, NULL)) goto done;
    uintptr_t load = remote_load_library(process.dwProcessId);
    if (!load) goto done;
    thread = CreateRemoteThread(process.hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)load, remote, 0, NULL);
    if (!thread) goto done;
    DWORD wait = WaitForSingleObject(thread, 15000);
    if (wait != WAIT_OBJECT_0) {
        fprintf(stderr, "DLL loading has not completed. The game remains open.\n");
        remote = NULL; /* The remote thread may still be reading the path. */
        goto done;
    }
    DWORD module = 0;
    if (!GetExitCodeThread(thread, &module) || !module) { fprintf(stderr, "LoadLibraryW failed.\n"); goto done; }
    printf("Diagnostic DLL loaded at %08lX. Logs are beside the DLL in logs/.\n", module);
    if (width) {
        int render_width = 0, render_height = 0;
        if (window_mode_render_size(process.hProcess, image, &render_width, &render_height)) {
            printf("Engine render size: %d x %d\n", render_width, render_height);
            if (render_width > width || render_height > height)
                fprintf(stderr, "Adapter chose a larger supported mode; preserving the complete frame.\n");
            if (!size_test_window(process.dwProcessId, render_width, render_height))
                fprintf(stderr, "Window could not be matched to the render size.\n");
        } else fprintf(stderr, "Render size unavailable; window left at the engine's size.\n");
    }
    result = 0;
done:
    if (debugging && process.dwProcessId) {
        TerminateProcess(process.hProcess, 1); /* Never leave this new test process paused. */
        DebugActiveProcessStop(process.dwProcessId);
    }
    if (result) fprintf(stderr, "Launcher failed (Win32 error %lu).\n", GetLastError());
    if (remote) VirtualFreeEx(process.hProcess, remote, 0, MEM_RELEASE);
    if (thread) CloseHandle(thread);
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) CloseHandle(process.hProcess);
    return result;
}
