/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Internal device state, platform synchronization primitives, and USB3
 * transport declarations.
 */

#ifndef KTOS_SDK_INTERNAL_H
#define KTOS_SDK_INTERNAL_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <libusb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#endif

#include "ktos_sdk.h"
#include "ktos_protocol.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* FT601 USB identity and bulk pipe endpoints. */
#define FT601_VID        0x0403u
#define FT601_PID        0x601fu
#define PIPE_CTRL_OUT    0x02u
#define PIPE_IQ_OUT      0x03u
#define PIPE_CTRL_IN     0x82u
#define PIPE_IQ_IN       0x83u

/* Legacy 200-byte wire framing constants. */
#define WIRE_OUT_SIZE    200u
#define WIRE_IN_CHUNK    4096u
#define WIRE_MAGIC_0     0x89abcdefu
#define WIRE_MAGIC_1     0x10234567u

/* IQ receive ring geometry: 4 x 1 MiB asynchronous requests. */
#define IQ_CHUNK         (1024u * 1024u)
#define IQ_SLOTS         4u

/* -------------------------------------------------------------------------- */
/* Platform synchronization, time, and transport state                        */
/* -------------------------------------------------------------------------- */

#ifdef _WIN32

/* Relevant FT_STATUS values from FTD3XX.dll. */
#define FT_IO_PENDING    24u
#define FT_IO_INCOMPLETE 25u

typedef CRITICAL_SECTION sdk_mutex;
typedef volatile LONG    sdk_atomic_int;

static inline int sdk_mutex_init(sdk_mutex *m) {
    InitializeCriticalSection(m);
    return 0;
}

static inline void sdk_mutex_destroy(sdk_mutex *m) {
    DeleteCriticalSection(m);
}

static inline void sdk_mutex_lock(sdk_mutex *m) {
    EnterCriticalSection(m);
}

static inline int sdk_mutex_trylock(sdk_mutex *m) {
    return TryEnterCriticalSection(m) != 0;
}

static inline void sdk_mutex_unlock(sdk_mutex *m) {
    LeaveCriticalSection(m);
}

static inline int sdk_atomic_load(const sdk_atomic_int *v) {
    return (int)InterlockedCompareExchange((volatile LONG *)v, 0, 0);
}

static inline void sdk_atomic_store(sdk_atomic_int *v, int value) {
    InterlockedExchange(v, (LONG)value);
}

static inline uint64_t sdk_monotonic_ms(void) {
    return (uint64_t)GetTickCount64();
}

static inline void sdk_sleep_ms(uint32_t ms) {
    Sleep(ms);
}

typedef ULONG(WINAPI *ft_pipe_fn)(void *, UCHAR, void *, ULONG, ULONG *, OVERLAPPED *);

typedef struct {
    HMODULE    module;
    ULONG(WINAPI *list)(ULONG *);
    ULONG(WINAPI *create)(void *, DWORD, void **);
    ULONG(WINAPI *close)(void *);
    ULONG(WINAPI *vidpid)(void *, USHORT *, USHORT *);
    ULONG(WINAPI *timeout)(void *, UCHAR, ULONG);
    ULONG(WINAPI *abort)(void *, UCHAR);
    ft_pipe_fn read;
    ft_pipe_fn write;
    ULONG(WINAPI *initialize)(void *, OVERLAPPED *);
    ULONG(WINAPI *release)(void *, OVERLAPPED *);
    ULONG(WINAPI *complete)(void *, OVERLAPPED *, ULONG *, BOOL);
    ULONG(WINAPI *stream)(void *, BOOL, BOOL, UCHAR, ULONG);
    ULONG(WINAPI *clear)(void *, BOOL, BOOL, UCHAR);
} ft_api;

typedef struct {
    OVERLAPPED overlap;
    uint8_t   *data;
    ULONG      bytes;
    ULONG      used;
    int        initialized;
    int        pending;
} iq_slot;

typedef struct {
    ft_api     api;
    void      *handle;
    iq_slot    slots[IQ_SLOTS];
    OVERLAPPED control_overlap;
    uint8_t    control_buffer[8192];
    ULONG      control_bytes;
    int        control_initialized;
    int        control_pending;
} sdk_transport;

typedef struct {
    HANDLE stop_event;
    HANDLE thread;
} sdk_heartbeat_state;

#else /* Linux / POSIX libusb-1.0 */

typedef pthread_mutex_t sdk_mutex;
typedef atomic_int      sdk_atomic_int;

static inline int sdk_mutex_init(sdk_mutex *m) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        return -ENOMEM;
    }
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    int rc = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    return rc ? -rc : 0;
}

static inline void sdk_mutex_destroy(sdk_mutex *m) {
    pthread_mutex_destroy(m);
}

static inline void sdk_mutex_lock(sdk_mutex *m) {
    pthread_mutex_lock(m);
}

static inline int sdk_mutex_trylock(sdk_mutex *m) {
    return pthread_mutex_trylock(m) == 0;
}

static inline void sdk_mutex_unlock(sdk_mutex *m) {
    pthread_mutex_unlock(m);
}

static inline int sdk_atomic_load(const sdk_atomic_int *v) {
    return atomic_load(v);
}

static inline void sdk_atomic_store(sdk_atomic_int *v, int value) {
    atomic_store(v, value);
}

static inline uint64_t sdk_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

static inline void sdk_sleep_ms(uint32_t ms) {
    struct timespec req = {
        .tv_sec  = (time_t)(ms / 1000u),
        .tv_nsec = (long)(ms % 1000u) * 1000000L,
    };
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
}

typedef struct {
    void *module;
    int (*init)(libusb_context **);
    void (*exit)(libusb_context *);
    ssize_t (*get_device_list)(libusb_context *, libusb_device ***);
    void (*free_device_list)(libusb_device **, int);
    int (*get_device_descriptor)(libusb_device *, struct libusb_device_descriptor *);
    int (*open)(libusb_device *, libusb_device_handle **);
    void (*close)(libusb_device_handle *);
    int (*kernel_driver_active)(libusb_device_handle *, int);
    int (*detach_kernel_driver)(libusb_device_handle *, int);
    int (*claim_interface)(libusb_device_handle *, int);
    int (*release_interface)(libusb_device_handle *, int);
    int (*bulk_transfer)(libusb_device_handle *, unsigned char, unsigned char *, int, int *,
                         unsigned int);
    struct libusb_transfer *(*alloc_transfer)(int);
    void (*free_transfer)(struct libusb_transfer *);
    int (*submit_transfer)(struct libusb_transfer *);
    int (*cancel_transfer)(struct libusb_transfer *);
    int (*handle_events_timeout_completed)(libusb_context *, struct timeval *, int *);
    const char *(*strerror)(int);
} libusb_api;

typedef struct {
    struct libusb_transfer *transfer;
    uint8_t                *data;
    uint32_t                bytes;
    uint32_t                used;
    int                     status;
    int                     completed;
    int                     pending;
} linux_iq_slot;

typedef struct {
    libusb_api              api;
    libusb_context         *ctx;
    libusb_device_handle   *handle;
    int                     comm_claimed;
    int                     data_claimed;
    sdk_mutex               session_lock;
    sdk_mutex               abort_lock;
    int                     locks_initialized;
    uint32_t                session_counter;
    linux_iq_slot           slots[IQ_SLOTS];
    struct libusb_transfer *control_transfer;
    uint8_t                 control_buffer[8192];
    uint32_t                control_bytes;
    int                     control_status;
    int                     control_completed;
    int                     control_pending;
} sdk_transport;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    pthread_t       thread;
    int             initialized;
    int             started;
    int             stop;
} sdk_heartbeat_state;

#endif /* _WIN32 */

struct ktos_device {
    /* Public prefix compatible with Windows vendor kcsdr.dll 1.3.4.
     * Field offsets (sample_type at +100, iq_pairs at +104) are part of the ABI. */
    char     name[50];
    char     serial[50];
    int32_t  sample_type;
    uint16_t iq_pairs;
    uint16_t padding;
    uint8_t  reserved[840];

    /* Platform USB3 transport handle and synchronization primitives. */
    sdk_transport       transport;
    sdk_mutex           control;
    sdk_mutex           rx;
    sdk_mutex           tx;
    sdk_mutex           errors;
    int                 mutexes_initialized;

    /* Keepalive thread and negotiated session metadata. */
    sdk_heartbeat_state heartbeat;
    uint32_t            next_id;
    uint32_t            backend;
    uint32_t            mode;
    uint32_t            capabilities;

    /* Last device-scoped error. */
    int  error;
    char message[256];

    /* Active IQ RX ring cursor and stop flag. */
    unsigned       slot;
    unsigned       streaming;
    sdk_atomic_int stopping;
};

int  sdk_failure(ktos_device *device, int error, const char *operation);
int  sdk_transport_open(ktos_device *device, uint32_t index);
int  sdk_transport_is_open(const ktos_device *device);
void sdk_transport_abort_rx(ktos_device *device);
void sdk_transport_close(ktos_device *device);
int  sdk_transport_can_free(const ktos_device *device);
int  sdk_wire(ktos_device *device, uint32_t op, const void *data, size_t size, uint8_t *reply,
              size_t capacity, size_t *bytes);
int  sdk_packet(ktos_device *device, const ktos_packet *request, uint8_t *buffer, size_t size,
                ktos_packet *reply);
void sdk_rx_release(ktos_device *device);

static inline uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void write32(uint8_t *p, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(value >> (i * 8));
    }
}

#endif /* KTOS_SDK_INTERNAL_H */
