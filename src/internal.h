/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Internal device state, FTDI D3XX function table, and transport declarations.
 */

#ifndef KTOS_SDK_INTERNAL_H
#define KTOS_SDK_INTERNAL_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ktos_sdk.h"
#include "ktos_protocol.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* FT601 USB identity and bulk pipe endpoints. */
#define FT601_VID        0x0403u
#define FT601_PID        0x601fu
#define PIPE_CTRL_OUT    0x02u
#define PIPE_IQ_OUT      0x03u
#define PIPE_CTRL_IN     0x82u
#define PIPE_IQ_IN       0x83u

/* Relevant FT_STATUS values from FTD3XX.dll. */
#define FT_IO_PENDING    24u
#define FT_IO_INCOMPLETE 25u

/* Legacy 200-byte wire framing constants. */
#define WIRE_OUT_SIZE    200u
#define WIRE_IN_CHUNK    4096u
#define WIRE_MAGIC_0     0x89abcdefu
#define WIRE_MAGIC_1     0x10234567u

/* IQ receive ring geometry: 4 x 1 MiB overlapped requests. */
#define IQ_CHUNK         (1024u * 1024u)
#define IQ_SLOTS         4u

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

struct ktos_device {
    /* Public prefix compatible with Windows vendor kcsdr.dll 1.3.4.
     * Field offsets (sample_type at +100, iq_pairs at +104) are part of the ABI. */
    char     name[50];
    char     serial[50];
    int32_t  sample_type;
    uint16_t iq_pairs;
    uint16_t padding;
    uint8_t  reserved[840];

    /* FTDI D3XX handle and synchronization primitives. */
    ft_api           api;
    void            *handle;
    CRITICAL_SECTION control;
    CRITICAL_SECTION rx;
    CRITICAL_SECTION tx;
    CRITICAL_SECTION errors;

    /* Keepalive thread and negotiated session metadata. */
    HANDLE   stop_event;
    HANDLE   heartbeat;
    uint32_t next_id;
    uint32_t backend;
    uint32_t mode;
    uint32_t capabilities;

    /* Last device-scoped error. */
    int  error;
    char message[256];

    /* Overlapped IQ RX state. */
    iq_slot       slots[IQ_SLOTS];
    unsigned      slot;
    unsigned      streaming;
    volatile LONG stopping;

    /* Reusable overlapped control IN state (armed before control OUT). */
    OVERLAPPED control_overlap;
    uint8_t    control_buffer[8192];
    ULONG      control_bytes;
    int        control_initialized;
    int        control_pending;
};

int  sdk_failure(ktos_device *device, int error, const char *operation);
int  sdk_ft(ktos_device *device, ULONG status, const char *operation);
int  sdk_ft_load(ft_api *api);
int  sdk_wire(ktos_device *device, uint32_t op, const void *data, size_t size, uint8_t *reply,
              size_t capacity, size_t *bytes);
int  sdk_packet(ktos_device *device, const ktos_packet *request, uint8_t *buffer, size_t size,
                ktos_packet *reply);
void sdk_rx_release(ktos_device *device);
void sdk_control_release(ktos_device *device);

static inline uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void write32(uint8_t *p, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(value >> (i * 8));
    }
}

#endif /* KTOS_SDK_INTERNAL_H */
