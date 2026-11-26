/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Windows FTDI D3XX transport layer: dynamic FTD3XX.dll loader, pre-armed
 * asynchronous control pipe framing, and overlapped 4-slot IQ streaming.
 */

#include "internal.h"

#include <stdlib.h>

static int sdk_ft(ktos_device *device, ULONG status, const char *operation) {
    return status ? sdk_failure(device, -1000 - (int)status, operation) : 0;
}

/* -------------------------------------------------------------------------- */
/* Dynamic FTD3XX.dll loader                                                  */
/* -------------------------------------------------------------------------- */

static int sdk_ft_load(ft_api *api) {
    WCHAR   path[MAX_PATH];
    HMODULE self = NULL;

    api->module = NULL;

    /* Prefer FTD3XX.dll located right next to kcsdr.dll. */
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(uintptr_t)&sdk_ft_load, &self)) {
        DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            WCHAR *slash = wcsrchr(path, L'\\');
            if (slash && (size_t)(slash - path) + 12 < MAX_PATH) {
                wcscpy(slash + 1, L"FTD3XX.dll");
                api->module = LoadLibraryExW(
                    path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            }
        }
    }

    /* Fall back to the host application directory and System32. */
    if (!api->module) {
        api->module = LoadLibraryExW(
            L"FTD3XX.dll", NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (!api->module) {
        return -(int)GetLastError();
    }

#define LOAD_FT_PROC(member, symbol)                                                               \
    do {                                                                                           \
        *(FARPROC *)&api->member = GetProcAddress(api->module, symbol);                            \
        if (!api->member) {                                                                        \
            goto fail;                                                                             \
        }                                                                                          \
    } while (0)

    LOAD_FT_PROC(list, "FT_CreateDeviceInfoList");
    LOAD_FT_PROC(create, "FT_Create");
    LOAD_FT_PROC(close, "FT_Close");
    LOAD_FT_PROC(vidpid, "FT_GetVIDPID");
    LOAD_FT_PROC(timeout, "FT_SetPipeTimeout");
    LOAD_FT_PROC(abort, "FT_AbortPipe");
    LOAD_FT_PROC(read, "FT_ReadPipe");
    LOAD_FT_PROC(write, "FT_WritePipe");
    LOAD_FT_PROC(initialize, "FT_InitializeOverlapped");
    LOAD_FT_PROC(release, "FT_ReleaseOverlapped");
    LOAD_FT_PROC(complete, "FT_GetOverlappedResult");
    LOAD_FT_PROC(stream, "FT_SetStreamPipe");
    LOAD_FT_PROC(clear, "FT_ClearStreamPipe");

#undef LOAD_FT_PROC
    return 0;

fail:
    FreeLibrary(api->module);
    memset(api, 0, sizeof(*api));
    return -ENOSYS;
}

/* -------------------------------------------------------------------------- */
/* Control pipe overlapped helpers                                            */
/* -------------------------------------------------------------------------- */

static void control_abort(ktos_device *device) {
    sdk_transport *t = &device->transport;
    t->api.abort(t->handle, PIPE_CTRL_IN);
    if (t->control_pending) {
        ULONG bytes  = 0;
        ULONG status = t->api.complete(t->handle, &t->control_overlap, &bytes, TRUE);
        if (status != FT_IO_PENDING && status != FT_IO_INCOMPLETE) {
            t->control_pending = 0;
        }
    }
}

static void control_release(ktos_device *device) {
    sdk_transport *t = &device->transport;
    if (t->control_pending) {
        control_abort(device);
    }
    if (t->control_initialized && !t->control_pending) {
        t->api.release(t->handle, &t->control_overlap);
        t->control_initialized = 0;
    }
}

static int control_submit(ktos_device *device, size_t offset) {
    sdk_transport *t = &device->transport;
    if (t->control_pending) {
        return sdk_failure(device, -EBUSY, "control read still pending");
    }
    if (!t->control_initialized) {
        int error = sdk_ft(device, t->api.initialize(t->handle, &t->control_overlap),
                           "control initialize");
        if (error) {
            return error;
        }
        t->control_initialized = 1;
    }

    t->control_bytes = 0;
    ULONG status     = t->api.read(t->handle, PIPE_CTRL_IN, t->control_buffer + offset,
                                   WIRE_IN_CHUNK, &t->control_bytes, &t->control_overlap);

    t->control_pending = (status == FT_IO_PENDING);
    if (status && status != FT_IO_PENDING) {
        control_abort(device);
        return sdk_ft(device, status, "control submit");
    }
    return 0;
}

static int control_complete(ktos_device *device, ULONG *actual) {
    sdk_transport *t = &device->transport;
    if (t->control_pending) {
        ULONG status = t->api.complete(t->handle, &t->control_overlap, &t->control_bytes, TRUE);
        if (status != FT_IO_PENDING && status != FT_IO_INCOMPLETE) {
            t->control_pending = 0;
        }
        if (status) {
            control_abort(device);
            return sdk_ft(device, status, "control completion");
        }
    }
    *actual = t->control_bytes;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Transport lifecycle                                                        */
/* -------------------------------------------------------------------------- */

int sdk_transport_open(ktos_device *device, uint32_t index) {
    sdk_transport *t = &device->transport;

    int error = sdk_ft_load(&t->api);
    if (error) {
        return error;
    }

    ULONG count = 0;
    error       = sdk_ft(device, t->api.list(&count), "enumerate FT601");
    if (error || index >= count) {
        if (!error) {
            error = sdk_failure(device, -ENODEV, "FT601 device index is not enumerated");
        }
        return error;
    }

    void *handle = NULL;
    error        = sdk_ft(device, t->api.create((void *)(uintptr_t)index, 0x10, &handle),
                          "open FT601");
    if (error) {
        return error;
    }
    t->handle = handle;

    USHORT vid = 0, pid = 0;
    error = sdk_ft(device, t->api.vidpid(t->handle, &vid, &pid), "USB identity");
    if (!error && (vid != FT601_VID || pid != FT601_PID)) {
        error = sdk_failure(device, -ENODEV, "unexpected USB VID/PID");
    }
    if (error) {
        return error;
    }

    const UCHAR pipes[4] = {PIPE_CTRL_OUT, PIPE_IQ_OUT, PIPE_CTRL_IN, PIPE_IQ_IN};
    for (unsigned i = 0; i < 4; i++) {
        ULONG timeout_ms = (pipes[i] == PIPE_IQ_IN) ? 500u : 3500u;
        error            = sdk_ft(device, t->api.timeout(t->handle, pipes[i], timeout_ms),
                                  "pipe timeout");
        if (error) {
            return error;
        }
    }
    return 0;
}

int sdk_transport_is_open(const ktos_device *device) {
    return device && device->transport.handle != NULL;
}

void sdk_transport_abort_rx(ktos_device *device) {
    if (device && device->transport.handle) {
        device->transport.api.abort(device->transport.handle, PIPE_IQ_IN);
    }
}

void sdk_transport_close(ktos_device *device) {
    sdk_transport *t = &device->transport;
    if (t->handle) {
        control_release(device);
        t->api.close(t->handle);
        t->handle = NULL;
    }
    /* Keep FTD3XX.dll resident: the Windows driver may still unwind aborted
     * requests briefly after FT_Close(). The OS reclaims it at process exit. */
}

int sdk_transport_can_free(const ktos_device *device) {
    const sdk_transport *t = &device->transport;
    if (t->control_pending) {
        return 0;
    }
    for (unsigned i = 0; i < IQ_SLOTS; i++) {
        if (t->slots[i].pending) {
            return 0;
        }
    }
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Control wire framing                                                       */
/* -------------------------------------------------------------------------- */

int sdk_wire(ktos_device *device, uint32_t op, const void *data, size_t size, uint8_t *reply,
             size_t capacity, size_t *bytes) {
    sdk_transport *t = &device->transport;
    if (size > 188 || (size && !data)) {
        return sdk_failure(device, -EINVAL, "command size");
    }

    uint8_t packet[WIRE_OUT_SIZE] = {0};
    write32(packet + 4, 0x80000000u + (uint32_t)size + 12u);
    write32(packet + 8, op);
    if (size) {
        memcpy(packet + 12, data, size);
    }

    /* Arm the asynchronous IN read before writing OUT so host thread scheduling
     * cannot miss the FPGA's short TX-ready window. */
    int error = reply ? control_submit(device, 0) : 0;
    if (error) {
        return error;
    }

    ULONG actual = 0;
    error = sdk_ft(device,
                   t->api.write(t->handle, PIPE_CTRL_OUT, packet, sizeof(packet), &actual, NULL),
                   "control write");
    if (error || actual != sizeof(packet)) {
        if (reply) {
            control_abort(device);
        }
        return error ? error : sdk_failure(device, -EIO, "short control write");
    }

    if (!reply) {
        sdk_sleep_ms(5);
        return 0;
    }

    uint8_t *block       = t->control_buffer;
    size_t   used        = 0;
    uint32_t expected_op = (op == 0x42u) ? 0x41u : op;
    uint64_t deadline    = sdk_monotonic_ms() + 1500u;

    while (sdk_monotonic_ms() < deadline) {
        actual = 0;
        error  = control_complete(device, &actual);
        if (error) {
            return error;
        }
        if (!actual || actual > WIRE_IN_CHUNK) {
            return sdk_failure(device, -EIO, "short control read");
        }
        used += actual;

        for (size_t at = 0; at + 20 <= used; at += 4) {
            if (read32(block + at) != WIRE_MAGIC_0 || read32(block + at + 4) != WIRE_MAGIC_1) {
                continue;
            }
            uint32_t length = read32(block + at + 12) & 0xffffu;
            if (length < 12 || length > 4088) {
                return sdk_failure(device, -EPROTO, "reply length");
            }

            size_t count = length - 12;
            if (at + 20 + count > used) {
                memmove(block, block + at, used - at);
                used -= at;
                break;
            }
            if (read32(block + at + 16) != expected_op) {
                continue;
            }
            if (count > capacity) {
                return sdk_failure(device, -ENOSPC, "reply capacity");
            }

            memcpy(reply, block + at + 20, count);
            *bytes = count;
            return 0;
        }

        if (used >= WIRE_IN_CHUNK) {
            memcpy(block, block + used - 16, 16);
            used = 16;
        }

        error = control_submit(device, used);
        if (error) {
            return error;
        }
    }

    control_abort(device);
    return sdk_failure(device, -ETIMEDOUT, "control deadline");
}

/* -------------------------------------------------------------------------- */
/* Overlapped IQ RX streaming and synchronous IQ TX                           */
/* -------------------------------------------------------------------------- */

static int submit_iq_slot(ktos_device *device, iq_slot *slot) {
    sdk_transport *t      = &device->transport;
    ULONG          actual = 0;
    ULONG status = t->api.read(t->handle, PIPE_IQ_IN, slot->data, IQ_CHUNK, &actual,
                               &slot->overlap);
    if (status && status != FT_IO_PENDING) {
        return sdk_ft(device, status, "IQ submit");
    }

    slot->pending = (status == FT_IO_PENDING);
    slot->bytes   = status ? 0 : actual;
    slot->used    = 0;
    return 0;
}

void sdk_rx_release(ktos_device *device) {
    sdk_transport *t         = &device->transport;
    unsigned       allocated = device->streaming;
    for (unsigned i = 0; i < IQ_SLOTS; i++) {
        allocated |= t->slots[i].initialized || t->slots[i].data || t->slots[i].pending;
    }
    if (!allocated) {
        return;
    }

    t->api.abort(t->handle, PIPE_IQ_IN);

    for (unsigned i = 0; i < IQ_SLOTS; i++) {
        iq_slot *slot = &t->slots[i];
        if (slot->pending) {
            ULONG bytes  = 0;
            ULONG status = t->api.complete(t->handle, &slot->overlap, &bytes, TRUE);
            /* Never free a buffer still owned by an incomplete kernel request. */
            if (status == FT_IO_PENDING || status == FT_IO_INCOMPLETE) {
                continue;
            }
            slot->pending = 0;
        }
        if (slot->initialized) {
            t->api.release(t->handle, &slot->overlap);
        }
        free(slot->data);
        memset(slot, 0, sizeof(*slot));
    }

    t->api.clear(t->handle, FALSE, FALSE, PIPE_IQ_IN);
    device->streaming = 0;
    device->slot      = 0;
}

int KTOS_CALL ktos_iq_read(ktos_device *device, void *data, uint32_t bytes, uint32_t *actual) {
    if (!device || !data || !actual || !bytes || (bytes % 4) != 0) {
        return -EINVAL;
    }

    *actual = 0;
    sdk_mutex_lock(&device->rx);

    sdk_transport *t     = &device->transport;
    int            error = 0;
    if (device->mode != KTOS_MODE_IQ || sdk_atomic_load(&device->stopping)) {
        error = -EBUSY;
        goto done;
    }

    if (!device->streaming) {
        error = sdk_ft(device, t->api.stream(t->handle, FALSE, FALSE, PIPE_IQ_IN, IQ_CHUNK),
                       "IQ stream");
        if (error) {
            goto done;
        }

        for (unsigned i = 0; i < IQ_SLOTS; i++) {
            iq_slot *slot = &t->slots[i];
            if (slot->pending) {
                error = -EBUSY;
                goto done;
            }
            slot->data = malloc(IQ_CHUNK);
            if (!slot->data) {
                error = -ENOMEM;
                goto done;
            }
            error = sdk_ft(device, t->api.initialize(t->handle, &slot->overlap), "IQ initialize");
            if (error) {
                goto done;
            }
            slot->initialized = 1;
            error             = submit_iq_slot(device, slot);
            if (error) {
                goto done;
            }
        }
        device->streaming = 1;
    }

    unsigned empty_transfers = 0;
    while (*actual < bytes && !sdk_atomic_load(&device->stopping)) {
        iq_slot *slot = &t->slots[device->slot];
        if (slot->pending) {
            error = sdk_ft(device,
                           t->api.complete(t->handle, &slot->overlap, &slot->bytes, TRUE),
                           "IQ completion");
            if (error) {
                goto done;
            }
            slot->pending = 0;
        }

        /* FT601 can finish a previously stopped stream with a zero-length
         * completion. Drain a bounded number of empty transfers so a stalled
         * endpoint still fails instead of spinning forever. */
        if (slot->bytes == 0 && ++empty_transfers <= IQ_SLOTS * 2) {
            error = submit_iq_slot(device, slot);
            if (error) {
                goto done;
            }
            device->slot = (device->slot + 1) % IQ_SLOTS;
            continue;
        }

        if (slot->bytes == 0 || slot->bytes > IQ_CHUNK || (slot->bytes % 4) != 0) {
            char description[80];
            snprintf(description, sizeof(description), "IQ completion length %lu", slot->bytes);
            error = sdk_failure(device, -EPROTO, description);
            goto done;
        }

        ULONG copy_bytes = slot->bytes - slot->used;
        if (copy_bytes > bytes - *actual) {
            copy_bytes = bytes - *actual;
        }
        memcpy((uint8_t *)data + *actual, slot->data + slot->used, copy_bytes);
        slot->used += copy_bytes;
        *actual += copy_bytes;

        if (slot->used == slot->bytes) {
            error = submit_iq_slot(device, slot);
            if (error) {
                goto done;
            }
            device->slot = (device->slot + 1) % IQ_SLOTS;
        }
    }

done:
    if (error) {
        sdk_rx_release(device);
    }
    sdk_mutex_unlock(&device->rx);
    return error;
}

int KTOS_CALL ktos_iq_write(ktos_device *device, const void *data, uint32_t bytes,
                            uint32_t *actual) {
    if (!device || !data || !actual || !bytes || (bytes % 4) != 0) {
        return -EINVAL;
    }

    sdk_mutex_lock(&device->tx);
    sdk_transport *t     = &device->transport;
    ULONG          count = 0;
    int            error = (device->mode != KTOS_MODE_IQ)
                               ? -EPERM
                               : sdk_ft(device,
                                        t->api.write(t->handle, PIPE_IQ_OUT, (void *)data, bytes,
                                                     &count, NULL),
                                        "IQ write");
    *actual              = count;
    sdk_mutex_unlock(&device->tx);
    return error;
}
