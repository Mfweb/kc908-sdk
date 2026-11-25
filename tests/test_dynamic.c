/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Verifies dynamic LoadLibrary/FreeLibrary cycles with preexisting worker
 * threads and isolated per-thread open-failure reporting (FLS).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef int(__cdecl *open_fn)(uint32_t, void **);
typedef int(__cdecl *error_fn)(void *, char *, uint32_t);

static open_fn  open_device;
static error_fn last_error;
static HANDLE   ready;

static DWORD WINAPI worker_thread(void *context) {
    (void)context;
    assert(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0);

    char message[512];
    assert(last_error(NULL, message, sizeof(message)) == 0 && !message[0]);

    void *device = (void *)1;
    int   error  = open_device(1000, &device);
    assert(error < 0 && !device);
    assert(last_error(NULL, message, sizeof(message)) == error && message[0]);
    return 0;
}

int main(void) {
    for (unsigned i = 0; i < 4; ++i) {
        ready         = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE thread = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
        assert(ready && thread);

        HMODULE library = LoadLibraryW(L"kcsdr.dll");
        assert(library);
        *(FARPROC *)&open_device = GetProcAddress(library, "ktos_open");
        *(FARPROC *)&last_error  = GetProcAddress(library, "ktos_error");
        assert(open_device && last_error);

        SetEvent(ready);
        assert(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);

        char message[512];
        assert(last_error(NULL, message, sizeof(message)) == 0 && !message[0]);

        CloseHandle(thread);
        CloseHandle(ready);
        assert(FreeLibrary(library));
    }

    puts("Dynamic SDK load, preexisting thread, isolated errors and unload PASS");
    return 0;
}
