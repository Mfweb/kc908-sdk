/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Core KTOS SDK lifecycle, protocol negotiation, keepalive heartbeat,
 * fragmented control RPC, and IQ command dispatch.
 */

#include "internal.h"

#include <stdlib.h>

/* Vendor wire opcodes used during open and IQ session management. */
#define OP_SERIAL         0x41u
#define OP_CAPABILITIES   0x42u
#define OP_IQ_RX_START    6u
#define OP_IQ_RX_STOP     7u
#define OP_IQ_OPEN        9u
#define OP_IQ_CLOSE       10u
#define OP_IQ_TX_START    0x86u
#define OP_IQ_TX_STOP     0x87u

/* Timeout and protocol constants. */
#define HEARTBEAT_MS      250u
#define RPC_TIMEOUT_MS    4000u
#define IMAGE_TIMEOUT_MS  35000u
#define LINUX_EINPROGRESS (-115)

/* -------------------------------------------------------------------------- */
/* Thread-local open-failure storage (Fiber Local Storage)                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    int  code;
    char message[256];
} open_failure;

static DWORD failure_slot = FLS_OUT_OF_INDEXES;

static VOID WINAPI release_failure(void *value) {
    free(value);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    (void)module;
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH) {
        failure_slot = FlsAlloc(release_failure);
        return failure_slot != FLS_OUT_OF_INDEXES;
    }
    if (reason == DLL_PROCESS_DETACH && failure_slot != FLS_OUT_OF_INDEXES) {
        FlsFree(failure_slot);
    }
    return TRUE;
}

static void set_open_failure(int code, const char *message) {
    open_failure *failure = FlsGetValue(failure_slot);
    if (!failure) {
        failure = calloc(1, sizeof(*failure));
        if (!failure) {
            return;
        }
        if (!FlsSetValue(failure_slot, failure)) {
            free(failure);
            return;
        }
    }
    failure->code = code;
    snprintf(failure->message, sizeof(failure->message), "%s", message);
}

/* -------------------------------------------------------------------------- */
/* Error reporting and helpers                                                */
/* -------------------------------------------------------------------------- */

int sdk_failure(ktos_device *device, int error, const char *operation) {
    if (device) {
        EnterCriticalSection(&device->errors);
        device->error = error;
        snprintf(device->message, sizeof(device->message), "%s: error %d", operation, error);
        LeaveCriticalSection(&device->errors);
    }
    return error;
}

int sdk_ft(ktos_device *device, ULONG status, const char *operation) {
    return status ? sdk_failure(device, -1000 - (int)status, operation) : 0;
}

uint32_t KTOS_CALL ktos_sdk_version(void) {
    return KTOS_SDK_VERSION_NUMBER;
}

static uint32_t next_transaction_id(ktos_device *device) {
    if (!++device->next_id) {
        ++device->next_id;
    }
    return device->next_id;
}

/* -------------------------------------------------------------------------- */
/* Keepalive heartbeat thread (KTOS backend only)                             */
/* -------------------------------------------------------------------------- */

static DWORD WINAPI heartbeat_thread(void *context) {
    ktos_device *device = context;

    while (WaitForSingleObject(device->stop_event, HEARTBEAT_MS) == WAIT_TIMEOUT) {
        /* Skip this tick if a foreground control call already holds the link. */
        if (!TryEnterCriticalSection(&device->control)) {
            continue;
        }

        ktos_packet request = {
            .operation = KTOS_PING,
            .id        = next_transaction_id(device),
        };
        int error;

        if (device->capabilities & KTOS_CAP_KEEPALIVE) {
            /* One-way keepalive avoids stalling high-rate IQ transfers with IN replies. */
            uint8_t packet[KTOS_HEADER_BYTES];
            size_t  bytes = 0;
            error         = ktos_packet_encode(packet, sizeof(packet), &request, &bytes);
            if (!error) {
                error = sdk_wire(device, KTOS_USB_KEEPALIVE, packet, bytes, NULL, 0, NULL);
            }
        } else {
            uint8_t     buffer[256];
            ktos_packet reply;
            error = sdk_packet(device, &request, buffer, sizeof(buffer), &reply);
        }

        if (error) {
            sdk_failure(device, error, "keepalive");
        }
        LeaveCriticalSection(&device->control);
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Device open, identity, and diagnostics                                     */
/* -------------------------------------------------------------------------- */

int KTOS_CALL ktos_open(uint32_t index, ktos_device **out) {
    if (!out) {
        return -EINVAL;
    }
    *out = NULL;

    ktos_device *device = calloc(1, sizeof(*device));
    if (!device) {
        return -ENOMEM;
    }

    InitializeCriticalSection(&device->control);
    InitializeCriticalSection(&device->rx);
    InitializeCriticalSection(&device->tx);
    InitializeCriticalSection(&device->errors);

    int error = sdk_ft_load(&device->api);
    if (error) {
        goto fail;
    }

    ULONG count = 0;
    error       = sdk_ft(device, device->api.list(&count), "enumerate FT601");
    if (error || index >= count) {
        if (!error) {
            error = sdk_failure(device, -ENODEV, "FT601 device index is not enumerated");
        }
        goto fail;
    }

    void *handle = NULL;
    error = sdk_ft(device, device->api.create((void *)(uintptr_t)index, 0x10, &handle),
                   "open FT601");
    if (error) {
        goto fail;
    }
    device->handle = handle;

    USHORT vid = 0, pid = 0;
    error = sdk_ft(device, device->api.vidpid(device->handle, &vid, &pid), "USB identity");
    if (!error && (vid != FT601_VID || pid != FT601_PID)) {
        error = sdk_failure(device, -ENODEV, "unexpected USB VID/PID");
    }
    if (error) {
        goto fail;
    }

    const UCHAR pipes[4] = {PIPE_CTRL_OUT, PIPE_IQ_OUT, PIPE_CTRL_IN, PIPE_IQ_IN};
    for (unsigned i = 0; i < 4; i++) {
        ULONG timeout_ms = (pipes[i] == PIPE_IQ_IN) ? 500u : 3500u;
        error = sdk_ft(device, device->api.timeout(device->handle, pipes[i], timeout_ms),
                       "pipe timeout");
        if (error) {
            goto fail;
        }
    }

    /* Probe device capabilities to detect original firmware vs. KTOS backend. */
    uint8_t caps[128], serial[64];
    size_t  n = 0;
    error     = sdk_wire(device, OP_CAPABILITIES, NULL, 0, caps, sizeof(caps), &n);
    if (error) {
        goto fail;
    }
    if (n < 34 || caps[0] != 0xf5 || caps[1] != 0x12 || memcmp(caps + 2, "KC908", 5) != 0) {
        error = sdk_failure(device, -ENODEV, "invalid KC908 capability reply");
        goto fail;
    }
    if (n >= 48 && memcmp(caps + 34, "KTOS", 4) == 0) {
        if (caps[38] != KTOS_PROTOCOL_VERSION || caps[40] > KTOS_MODE_MCP) {
            error = -EPROTONOSUPPORT;
            goto fail;
        }
        device->backend      = KTOS_BACKEND_CUSTOM;
        device->mode         = caps[40];
        device->capabilities = caps[41];
    }

    /* Query the 20-byte ASCII serial number and sanitize it for JSON output. */
    error = sdk_wire(device, OP_SERIAL, NULL, 0, serial, sizeof(serial), &n);
    if (error || n != 20) {
        if (!error) {
            error = -EPROTO;
        }
        goto fail;
    }

    strcpy(device->name, "KC908V");
    for (size_t i = 0; i < n && serial[i]; i++) {
        uint8_t ch = serial[i];
        device->serial[i] =
            (ch >= 32 && ch < 127 && ch != '"' && ch != '\\') ? (char)ch : '_';
    }
    device->iq_pairs = 4096;

    if (device->mode == KTOS_MODE_IQ) {
        error = ktos_iq_command(device, OP_IQ_OPEN, NULL, 0);
        if (error) {
            goto fail;
        }
    }

    if (device->backend == KTOS_BACKEND_CUSTOM) {
        device->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!device->stop_event) {
            error = -ENOMEM;
            goto fail;
        }
        device->heartbeat = CreateThread(NULL, 0, heartbeat_thread, device, 0, NULL);
        if (!device->heartbeat) {
            error = -ENOMEM;
            goto fail;
        }
    }

    *out = device;
    return 0;

fail:
    set_open_failure(error, device->message[0]
                                ? device->message
                                : "load FTD3XX.dll 1.3.0.4 or later and check USB3 connection");
    ktos_close(device);
    return error;
}

int KTOS_CALL ktos_identity(ktos_device *device, char *json, uint32_t capacity) {
    if (!device || !json || !capacity) {
        return -EINVAL;
    }

    int written = snprintf(
        json, capacity,
        "{\"name\":\"%s\",\"serial\":\"%s\",\"backend\":\"%s\",\"mode\":\"%s\","
        "\"sdk\":\"" KTOS_SDK_VERSION_TEXT "\"}",
        device->name, device->serial,
        device->backend ? "KTOS" : "original",
        device->mode ? "MCP" : "IQ");

    return (written < 0 || (unsigned)written >= capacity) ? -ENOSPC : 0;
}

int KTOS_CALL ktos_error(ktos_device *device, char *text, uint32_t capacity) {
    if (!text || !capacity) {
        return -EINVAL;
    }

    if (!device) {
        open_failure *failure = FlsGetValue(failure_slot);
        snprintf(text, capacity, "%s", failure ? failure->message : "");
        return failure ? failure->code : 0;
    }

    EnterCriticalSection(&device->errors);
    snprintf(text, capacity, "%s", device->message);
    int error = device->error;
    LeaveCriticalSection(&device->errors);
    return error;
}

/* -------------------------------------------------------------------------- */
/* IQ commands and fragmented KTOS RPC / binary requests                      */
/* -------------------------------------------------------------------------- */

int KTOS_CALL ktos_iq_command(ktos_device *device, uint32_t opcode, const void *data,
                              uint32_t bytes) {
    if (!device || (bytes && !data) || bytes > 144) {
        return -EINVAL;
    }
    if (device->mode != KTOS_MODE_IQ) {
        return sdk_failure(device, -EPERM, "select IQ mode on device");
    }

    EnterCriticalSection(&device->control);
    int error;

    if (device->backend == KTOS_BACKEND_ORIGINAL) {
        error = sdk_wire(device, opcode, data, bytes, NULL, 0, NULL);
    } else {
        uint8_t     payload[148];
        uint8_t     buffer[256];
        ktos_packet reply;

        write32(payload, opcode);
        if (bytes) {
            memcpy(payload + 4, data, bytes);
        }

        ktos_packet request = {
            .operation = KTOS_IQ,
            .id        = next_transaction_id(device),
            .data      = payload,
            .bytes     = bytes + 4,
        };
        error = sdk_packet(device, &request, buffer, sizeof(buffer), &reply);
    }

    if (!error && (opcode == OP_IQ_RX_START || opcode == OP_IQ_TX_START)) {
        InterlockedExchange(&device->stopping, 0);
    }

    LeaveCriticalSection(&device->control);
    return error ? sdk_failure(device, error, "IQ command") : 0;
}

/* Executes a fragmented BEGIN -> WRITE* -> EXECUTE -> READ* transaction. */
static int send_fragmented_request(ktos_device *device, const void *payload, size_t length,
                                   void *output, uint32_t capacity, uint32_t *needed,
                                   unsigned timeout_ms) {
    if (!device || !payload || !output || !capacity || !needed || !length ||
        length > KTOS_REQUEST_BYTES) {
        return -EINVAL;
    }

    char *out = output;
    *needed   = 0;
    out[0]    = '\0';

    if (device->backend != KTOS_BACKEND_CUSTOM || device->mode != KTOS_MODE_MCP) {
        return sdk_failure(device, -EPERM, "select KTOS MCP mode on device");
    }

    EnterCriticalSection(&device->control);

    uint8_t     buffer[4096];
    ktos_packet reply;
    ktos_packet request = {
        .operation = KTOS_BEGIN,
        .id        = next_transaction_id(device),
        .total     = (uint32_t)length,
    };

    int error = sdk_packet(device, &request, buffer, sizeof(buffer), &reply);

    for (size_t offset = 0; !error && offset < length;) {
        size_t chunk = length - offset;
        if (chunk > KTOS_FRAGMENT_BYTES) {
            chunk = KTOS_FRAGMENT_BYTES;
        }
        request.operation = KTOS_WRITE;
        request.offset    = (uint32_t)offset;
        request.data      = (const uint8_t *)payload + offset;
        request.bytes     = chunk;

        error = sdk_packet(device, &request, buffer, sizeof(buffer), &reply);
        offset += chunk;
    }

    request.operation = KTOS_EXECUTE;
    request.offset    = 0;
    request.data      = NULL;
    request.bytes     = 0;
    if (!error) {
        error = sdk_packet(device, &request, buffer, sizeof(buffer), &reply);
    }

    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    request.operation  = KTOS_READ;
    uint32_t total     = 0;

    while (!error) {
        if (GetTickCount64() > deadline) {
            error = -ETIMEDOUT;
            break;
        }

        error = sdk_packet(device, &request, buffer, sizeof(buffer), &reply);
        if (error == LINUX_EINPROGRESS) {
            error = 0;
            Sleep(5);
            continue;
        }
        if (error) {
            break;
        }

        if (reply.offset != request.offset || !reply.total || reply.total > KTOS_RESPONSE_BYTES ||
            (total && reply.total != total) || reply.offset > reply.total ||
            reply.bytes > reply.total - reply.offset) {
            error = -EPROTO;
            break;
        }

        total   = reply.total;
        *needed = total + 1;
        if (capacity <= total) {
            error = -ENOSPC;
            break;
        }

        memcpy(out + request.offset, reply.data, reply.bytes);
        request.offset += (uint32_t)reply.bytes;

        if (request.offset == total) {
            out[total] = '\0';
            break;
        }
        if (!reply.bytes) {
            error = -EPROTO;
            break;
        }
    }

    if (error) {
        sdk_failure(device, error, "agent RPC");
    }
    LeaveCriticalSection(&device->control);
    return error;
}

int KTOS_CALL ktos_rpc(ktos_device *device, const char *command, char *json, uint32_t capacity,
                       uint32_t *needed) {
    if (!command) {
        return -EINVAL;
    }

    size_t length = strlen(command);
    if (!length || length + 6 > KTOS_REQUEST_BYTES || strpbrk(command, "\r\n")) {
        return -EINVAL;
    }

    char request[KTOS_REQUEST_BYTES];
    memcpy(request, "agent ", 6);
    memcpy(request + 6, command, length);
    return send_fragmented_request(device, request, length + 6, json, capacity, needed,
                                   RPC_TIMEOUT_MS);
}

int KTOS_CALL ktos_image_request(ktos_device *device, const void *request, uint32_t bytes,
                                 void *response, uint32_t capacity, uint32_t *actual) {
    if (!request || bytes < 16 || bytes > KTOS_REQUEST_BYTES - 6 || !actual) {
        return -EINVAL;
    }

    uint8_t frame[KTOS_REQUEST_BYTES];
    memcpy(frame, "image ", 6);
    memcpy(frame + 6, request, bytes);

    uint32_t needed = 0;
    int      error  = send_fragmented_request(device, frame, bytes + 6, response, capacity, &needed,
                                              IMAGE_TIMEOUT_MS);
    *actual = needed ? needed - 1 : 0;
    return error;
}

/* -------------------------------------------------------------------------- */
/* Stop and close                                                             */
/* -------------------------------------------------------------------------- */

int KTOS_CALL ktos_iq_stop(ktos_device *device) {
    if (!device || !device->handle) {
        return -EINVAL;
    }

    InterlockedExchange(&device->stopping, 1);
    if (device->streaming) {
        device->api.abort(device->handle, PIPE_IQ_IN);
    }

    int rx_error = ktos_iq_command(device, OP_IQ_RX_STOP, NULL, 0);
    int tx_error = ktos_iq_command(device, OP_IQ_TX_STOP, NULL, 0);

    EnterCriticalSection(&device->rx);
    sdk_rx_release(device);
    LeaveCriticalSection(&device->rx);

    return rx_error ? rx_error : tx_error;
}

void KTOS_CALL ktos_close(ktos_device *device) {
    if (!device) {
        return;
    }

    if (device->stop_event) {
        SetEvent(device->stop_event);
    }
    if (device->heartbeat) {
        WaitForSingleObject(device->heartbeat, INFINITE);
        CloseHandle(device->heartbeat);
    }
    if (device->stop_event) {
        CloseHandle(device->stop_event);
    }

    if (device->handle) {
        if (device->mode == KTOS_MODE_IQ) {
            ktos_iq_stop(device);
            ktos_iq_command(device, OP_IQ_CLOSE, NULL, 0);
        } else {
            char     reply[256];
            uint32_t needed = 0;
            ktos_rpc(device, "action stop", reply, sizeof(reply), &needed);
        }
        sdk_control_release(device);
        device->api.close(device->handle);
    }

    /* Keep FTD3XX.dll resident: the Windows driver may still unwind aborted
     * requests briefly after FT_Close(). The OS reclaims it at process exit. */
    DeleteCriticalSection(&device->control);
    DeleteCriticalSection(&device->rx);
    DeleteCriticalSection(&device->tx);
    DeleteCriticalSection(&device->errors);

    /* Never free the device struct if a kernel I/O request still references an
     * embedded OVERLAPPED or buffer. */
    if (device->control_pending) {
        return;
    }
    for (unsigned i = 0; i < IQ_SLOTS; i++) {
        if (device->slots[i].pending) {
            return;
        }
    }

    free(device);
}
