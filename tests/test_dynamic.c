/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Verifies dynamic library load/unload cycles with preexisting worker threads
 * and isolated per-thread open-failure reporting (FLS on Windows, pthread key
 * on Linux).
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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
        FARPROC open_proc  = GetProcAddress(library, "ktos_open");
        FARPROC error_proc = GetProcAddress(library, "ktos_error");
        assert(open_proc && error_proc);
        memcpy(&open_device, &open_proc, sizeof(open_device));
        memcpy(&last_error, &error_proc, sizeof(last_error));

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

#else /* Linux / POSIX */

#include <dlfcn.h>
#include <pthread.h>

typedef int (*open_fn)(uint32_t, void **);
typedef int (*error_fn)(void *, char *, uint32_t);

static open_fn         open_device;
static error_fn        last_error;
static pthread_mutex_t ready_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  ready_cond = PTHREAD_COND_INITIALIZER;
static int             is_ready;

static void *worker_thread(void *context) {
    (void)context;
    pthread_mutex_lock(&ready_lock);
    while (!is_ready) {
        pthread_cond_wait(&ready_cond, &ready_lock);
    }
    pthread_mutex_unlock(&ready_lock);

    char message[512];
    assert(last_error(NULL, message, sizeof(message)) == 0 && !message[0]);

    void *device = (void *)1;
    int   error  = open_device(1000, &device);
    assert(error < 0 && !device);
    assert(last_error(NULL, message, sizeof(message)) == error && message[0]);
    return NULL;
}

int main(void) {
    for (unsigned i = 0; i < 4; ++i) {
        pthread_mutex_lock(&ready_lock);
        is_ready = 0;
        pthread_mutex_unlock(&ready_lock);

        pthread_t thread;
        assert(pthread_create(&thread, NULL, worker_thread, NULL) == 0);

        void *library = dlopen("./libkcsdr.so", RTLD_NOW | RTLD_LOCAL);
        assert(library);
        void *open_sym  = dlsym(library, "ktos_open");
        void *error_sym = dlsym(library, "ktos_error");
        assert(open_sym && error_sym);
        memcpy(&open_device, &open_sym, sizeof(open_device));
        memcpy(&last_error, &error_sym, sizeof(last_error));

        pthread_mutex_lock(&ready_lock);
        is_ready = 1;
        pthread_cond_signal(&ready_cond);
        pthread_mutex_unlock(&ready_lock);

        assert(pthread_join(thread, NULL) == 0);

        char message[512];
        assert(last_error(NULL, message, sizeof(message)) == 0 && !message[0]);

        assert(dlclose(library) == 0);
    }

    puts("Dynamic SDK load, preexisting thread, isolated errors and unload PASS");
    return 0;
}

#endif
