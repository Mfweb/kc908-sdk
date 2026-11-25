/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Windows FTDI D3XX transport layer: dynamic FTD3XX.dll loader, pre-armed
 * asynchronous control pipe framing, and overlapped 4-slot IQ streaming.
 */

#include "internal.h"

#include <stdlib.h>

/* -------------------------------------------------------------------------- */
/* Dynamic FTD3XX.dll loader                                                  */
/* -------------------------------------------------------------------------- */

int sdk_ft_load(ft_api *api) {
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
    device->api.abort(device->handle, PIPE_CTRL_IN);
    if (device->control_pending) {
        ULONG bytes  = 0;
        ULONG status = device->api.complete(device->handle, &device->control_overlap, &bytes, TRUE);
        if (status != FT_IO_PENDING && status != FT_IO_INCOMPLETE) {
            device->control_pending = 0;
        }
    }
}

void sdk_control_release(ktos_device *device) {
    if (device->control_pending) {
        control_abort(device);
    }
    if (device->control_initialized && !device->control_pending) {
        device->api.release(device->handle, &device->control_overlap);
        device->control_initialized = 0;
    }
}

static int control_submit(ktos_device *device, size_t offset) {
    if (device->control_pending) {
        return sdk_failure(device, -EBUSY, "control read still pending");
    }
    if (!device->control_initialized) {
        int error = sdk_ft(device, device->api.initialize(device->handle, &device->control_overlap),
                           "control initialize");
        if (error) {
            return error;
        }
        device->control_initialized = 1;
    }

    device->control_bytes = 0;
    ULONG status = device->api.read(device->handle, PIPE_CTRL_IN, device->control_buffer + offset,
                                    WIRE_IN_CHUNK, &device->control_bytes,
                                    &device->control_overlap);

    device->control_pending = (status == FT_IO_PENDING);
    if (status && status != FT_IO_PENDING) {
        control_abort(device);
        return sdk_ft(device, status, "control submit");
    }
    return 0;
}

static int control_complete(ktos_device *device, ULONG *actual) {
    if (device->control_pending) {
        ULONG status = device->api.complete(device->handle, &device->control_overlap,
                                            &device->control_bytes, TRUE);
        if (status != FT_IO_PENDING && status != FT_IO_INCOMPLETE) {
            device->control_pending = 0;
        }
        if (status) {
            control_abort(device);
            return sdk_ft(device, status, "control completion");
        }
    }
    *actual = device->control_bytes;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Control wire framing and KTOS packet exchange                              */
/* -------------------------------------------------------------------------- */

int sdk_wire(ktos_device *device, uint32_t op, const void *data, size_t size, uint8_t *reply,
             size_t capacity, size_t *bytes) {
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
                   device->api.write(device->handle, PIPE_CTRL_OUT, packet, sizeof(packet), &actual,
                                     NULL),
                   "control write");
    if (error || actual != sizeof(packet)) {
        if (reply) {
            control_abort(device);
        }
        return error ? error : sdk_failure(device, -EIO, "short control write");
    }

    if (!reply) {
        Sleep(5);
        return 0;
    }

    uint8_t  *block       = device->control_buffer;
    size_t    used        = 0;
    uint32_t  expected_op = (op == 0x42u) ? 0x41u : op;
    ULONGLONG deadline    = GetTickCount64() + 1500u;

    while (GetTickCount64() < deadline) {
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

int sdk_packet(ktos_device *device, const ktos_packet *request, uint8_t *buffer, size_t size,
               ktos_packet *reply) {
    uint8_t encoded[180];
    size_t  written = 0;
    size_t  count   = 0;

    int error = ktos_packet_encode(encoded, sizeof(encoded), request, &written);
    if (!error) {
        error = sdk_wire(device, KTOS_USB_COMMAND, encoded, written, buffer, size, &count);
    }
    if (!error) {
        error = ktos_packet_decode(reply, buffer, count);
    }
    if (!error && (reply->operation != request->operation || reply->id != request->id)) {
        error = -EPROTO;
    }

    return error ? sdk_failure(device, error, "KTOS packet") : reply->status;
}

/* -------------------------------------------------------------------------- */
/* Overlapped IQ RX streaming and synchronous IQ TX                           */
/* -------------------------------------------------------------------------- */

static int submit_iq_slot(ktos_device *device, iq_slot *slot) {
    ULONG actual = 0;
    ULONG status = device->api.read(device->handle, PIPE_IQ_IN, slot->data, IQ_CHUNK, &actual,
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
    unsigned allocated = device->streaming;
    for (unsigned i = 0; i < IQ_SLOTS; i++) {
        allocated |= device->slots[i].initialized || device->slots[i].data ||
                     device->slots[i].pending;
    }
    if (!allocated) {
        return;
    }

    device->api.abort(device->handle, PIPE_IQ_IN);

    for (unsigned i = 0; i < IQ_SLOTS; i++) {
        iq_slot *slot = &device->slots[i];
        if (slot->pending) {
            ULONG bytes  = 0;
            ULONG status = device->api.complete(device->handle, &slot->overlap, &bytes, TRUE);
            /* Never free a buffer still owned by an incomplete kernel request. */
            if (status == FT_IO_PENDING || status == FT_IO_INCOMPLETE) {
                continue;
            }
            slot->pending = 0;
        }
        if (slot->initialized) {
            device->api.release(device->handle, &slot->overlap);
        }
        free(slot->data);
        memset(slot, 0, sizeof(*slot));
    }

    device->api.clear(device->handle, FALSE, FALSE, PIPE_IQ_IN);
    device->streaming = 0;
    device->slot      = 0;
}

int KTOS_CALL ktos_iq_read(ktos_device *device, void *data, uint32_t bytes, uint32_t *actual) {
    if (!device || !data || !actual || !bytes || (bytes % 4) != 0) {
        return -EINVAL;
    }

    *actual = 0;
    EnterCriticalSection(&device->rx);

    int error = 0;
    if (device->mode != KTOS_MODE_IQ || device->stopping) {
        error = -EBUSY;
        goto done;
    }

    if (!device->streaming) {
        error = sdk_ft(device, device->api.stream(device->handle, FALSE, FALSE, PIPE_IQ_IN,
                                                  IQ_CHUNK),
                       "IQ stream");
        if (error) {
            goto done;
        }

        for (unsigned i = 0; i < IQ_SLOTS; i++) {
            iq_slot *slot = &device->slots[i];
            if (slot->pending) {
                error = -EBUSY;
                goto done;
            }
            slot->data = malloc(IQ_CHUNK);
            if (!slot->data) {
                error = -ENOMEM;
                goto done;
            }
            error = sdk_ft(device, device->api.initialize(device->handle, &slot->overlap),
                           "IQ initialize");
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
    while (*actual < bytes && !device->stopping) {
        iq_slot *slot = &device->slots[device->slot];
        if (slot->pending) {
            error = sdk_ft(device,
                           device->api.complete(device->handle, &slot->overlap, &slot->bytes, TRUE),
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
    LeaveCriticalSection(&device->rx);
    return error;
}

int KTOS_CALL ktos_iq_write(ktos_device *device, const void *data, uint32_t bytes,
                            uint32_t *actual) {
    if (!device || !data || !actual || !bytes || (bytes % 4) != 0) {
        return -EINVAL;
    }

    EnterCriticalSection(&device->tx);
    ULONG count = 0;
    int   error = (device->mode != KTOS_MODE_IQ)
                      ? -EPERM
                      : sdk_ft(device,
                               device->api.write(device->handle, PIPE_IQ_OUT, (void *)data, bytes,
                                                 &count, NULL),
                               "IQ write");
    *actual     = count;
    LeaveCriticalSection(&device->tx);
    return error;
}
