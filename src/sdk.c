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
/* Thread-local open-failure storage                                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    int  code;
    char message[256];
} open_failure;

#ifdef _WIN32

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

static open_failure *get_open_failure(void) {
    return (failure_slot != FLS_OUT_OF_INDEXES) ? FlsGetValue(failure_slot) : NULL;
}

static void set_open_failure(int code, const char *message) {
    if (failure_slot == FLS_OUT_OF_INDEXES) {
        return;
    }
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

#else /* POSIX pthread key */

static pthread_key_t failure_key;
static int           failure_key_ready;

__attribute__((constructor)) static void sdk_library_init(void) {
    if (pthread_key_create(&failure_key, free) == 0) {
        failure_key_ready = 1;
    }
}

__attribute__((destructor)) static void sdk_library_fini(void) {
    if (failure_key_ready) {
        void *current = pthread_getspecific(failure_key);
        if (current) {
            pthread_setspecific(failure_key, NULL);
            free(current);
        }
        pthread_key_delete(failure_key);
        failure_key_ready = 0;
    }
}

static open_failure *get_open_failure(void) {
    return failure_key_ready ? (open_failure *)pthread_getspecific(failure_key) : NULL;
}

static void set_open_failure(int code, const char *message) {
    if (!failure_key_ready) {
        return;
    }
    open_failure *failure = pthread_getspecific(failure_key);
    if (!failure) {
        failure = calloc(1, sizeof(*failure));
        if (!failure) {
            return;
        }
        if (pthread_setspecific(failure_key, failure) != 0) {
            free(failure);
            return;
        }
    }
    failure->code = code;
    snprintf(failure->message, sizeof(failure->message), "%s", message);
}

#endif

/* -------------------------------------------------------------------------- */
/* Error reporting and packet exchange helpers                                */
/* -------------------------------------------------------------------------- */

int sdk_failure(ktos_device *device, int error, const char *operation) {
    if (device) {
        sdk_mutex_lock(&device->errors);
        device->error = error;
        snprintf(device->message, sizeof(device->message), "%s: error %d", operation, error);
        sdk_mutex_unlock(&device->errors);
    }
    return error;
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
/* Keepalive heartbeat thread (KTOS backend only)                             */
/* -------------------------------------------------------------------------- */

static void heartbeat_tick(ktos_device *device) {
    /* Skip this tick if a foreground control call already holds the link. */
    if (!sdk_mutex_trylock(&device->control)) {
        return;
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
    sdk_mutex_unlock(&device->control);
}

#ifdef _WIN32

static DWORD WINAPI heartbeat_thread(void *context) {
    ktos_device *device = context;

    while (WaitForSingleObject(device->heartbeat.stop_event, HEARTBEAT_MS) == WAIT_TIMEOUT) {
        heartbeat_tick(device);
    }
    return 0;
}

static int heartbeat_start(ktos_device *device) {
    device->heartbeat.stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!device->heartbeat.stop_event) {
        return -ENOMEM;
    }
    device->heartbeat.thread = CreateThread(NULL, 0, heartbeat_thread, device, 0, NULL);
    if (!device->heartbeat.thread) {
        return -ENOMEM;
    }
    return 0;
}

static void heartbeat_stop(ktos_device *device) {
    if (device->heartbeat.stop_event) {
        SetEvent(device->heartbeat.stop_event);
    }
    if (device->heartbeat.thread) {
        WaitForSingleObject(device->heartbeat.thread, INFINITE);
        CloseHandle(device->heartbeat.thread);
        device->heartbeat.thread = NULL;
    }
    if (device->heartbeat.stop_event) {
        CloseHandle(device->heartbeat.stop_event);
        device->heartbeat.stop_event = NULL;
    }
}

#else /* POSIX pthread heartbeat */

static void *heartbeat_thread(void *context) {
    ktos_device *device = context;

    for (;;) {
        struct timespec until;
        clock_gettime(CLOCK_MONOTONIC, &until);
        until.tv_nsec += (long)HEARTBEAT_MS * 1000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec += until.tv_nsec / 1000000000L;
            until.tv_nsec %= 1000000000L;
        }

        pthread_mutex_lock(&device->heartbeat.mutex);
        while (!device->heartbeat.stop) {
            int rc = pthread_cond_timedwait(&device->heartbeat.cond, &device->heartbeat.mutex,
                                            &until);
            if (rc == ETIMEDOUT) {
                break;
            }
        }
        int should_stop = device->heartbeat.stop;
        pthread_mutex_unlock(&device->heartbeat.mutex);

        if (should_stop) {
            break;
        }
        heartbeat_tick(device);
    }
    return NULL;
}

static int heartbeat_start(ktos_device *device) {
    pthread_condattr_t attr;
    if (pthread_mutex_init(&device->heartbeat.mutex, NULL) != 0) {
        return -ENOMEM;
    }
    if (pthread_condattr_init(&attr) != 0) {
        pthread_mutex_destroy(&device->heartbeat.mutex);
        return -ENOMEM;
    }
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    int rc = pthread_cond_init(&device->heartbeat.cond, &attr);
    pthread_condattr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_destroy(&device->heartbeat.mutex);
        return -ENOMEM;
    }
    device->heartbeat.initialized = 1;
    device->heartbeat.stop        = 0;

    if (pthread_create(&device->heartbeat.thread, NULL, heartbeat_thread, device) != 0) {
        return -ENOMEM;
    }
    device->heartbeat.started = 1;
    return 0;
}

static void heartbeat_stop(ktos_device *device) {
    if (!device->heartbeat.initialized) {
        return;
    }
    if (device->heartbeat.started) {
        pthread_mutex_lock(&device->heartbeat.mutex);
        device->heartbeat.stop = 1;
        pthread_cond_signal(&device->heartbeat.cond);
        pthread_mutex_unlock(&device->heartbeat.mutex);

        pthread_join(device->heartbeat.thread, NULL);
        device->heartbeat.started = 0;
    }
    pthread_cond_destroy(&device->heartbeat.cond);
    pthread_mutex_destroy(&device->heartbeat.mutex);
    device->heartbeat.initialized = 0;
}

#endif

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

    int error = sdk_mutex_init(&device->control);
    if (!error) {
        error = sdk_mutex_init(&device->rx);
    }
    if (!error) {
        error = sdk_mutex_init(&device->tx);
    }
    if (!error) {
        error = sdk_mutex_init(&device->errors);
    }
    if (error) {
        free(device);
        return error;
    }
    device->mutexes_initialized = 1;

    error = sdk_transport_open(device, index);
    if (error) {
        goto fail;
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
        error = heartbeat_start(device);
        if (error) {
            goto fail;
        }
    }

    *out = device;
    return 0;

fail:
    set_open_failure(
        error,
        device->message[0]
            ? device->message
#ifdef _WIN32
            : "load FTD3XX.dll 1.3.0.4 or later and check USB3 connection"
#else
            : "check libusb-1.0, USB3 connection, and udev permissions for FT601 (0403:601f)"
#endif
    );
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
        open_failure *failure = get_open_failure();
        snprintf(text, capacity, "%s", failure ? failure->message : "");
        return failure ? failure->code : 0;
    }

    sdk_mutex_lock(&device->errors);
    snprintf(text, capacity, "%s", device->message);
    int error = device->error;
    sdk_mutex_unlock(&device->errors);
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

    sdk_mutex_lock(&device->control);
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
        sdk_atomic_store(&device->stopping, 0);
    }

    sdk_mutex_unlock(&device->control);
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

    sdk_mutex_lock(&device->control);

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

    uint64_t deadline = sdk_monotonic_ms() + timeout_ms;
    request.operation = KTOS_READ;
    uint32_t total    = 0;

    while (!error) {
        if (sdk_monotonic_ms() > deadline) {
            error = -ETIMEDOUT;
            break;
        }

        error = sdk_packet(device, &request, buffer, sizeof(buffer), &reply);
        if (error == LINUX_EINPROGRESS) {
            error = 0;
            sdk_sleep_ms(5);
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
    sdk_mutex_unlock(&device->control);
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
    if (!device || !sdk_transport_is_open(device)) {
        return -EINVAL;
    }

    sdk_atomic_store(&device->stopping, 1);
    if (device->streaming) {
        sdk_transport_abort_rx(device);
    }

    int rx_error = ktos_iq_command(device, OP_IQ_RX_STOP, NULL, 0);
    int tx_error = ktos_iq_command(device, OP_IQ_TX_STOP, NULL, 0);

    sdk_mutex_lock(&device->rx);
    sdk_rx_release(device);
    sdk_mutex_unlock(&device->rx);

    return rx_error ? rx_error : tx_error;
}

void KTOS_CALL ktos_close(ktos_device *device) {
    if (!device) {
        return;
    }

    heartbeat_stop(device);

    if (sdk_transport_is_open(device)) {
        if (device->mode == KTOS_MODE_IQ) {
            ktos_iq_stop(device);
            ktos_iq_command(device, OP_IQ_CLOSE, NULL, 0);
        } else {
            char     reply[256];
            uint32_t needed = 0;
            ktos_rpc(device, "action stop", reply, sizeof(reply), &needed);
        }
    }

    sdk_transport_close(device);

    if (device->mutexes_initialized) {
        sdk_mutex_destroy(&device->control);
        sdk_mutex_destroy(&device->rx);
        sdk_mutex_destroy(&device->tx);
        sdk_mutex_destroy(&device->errors);
        device->mutexes_initialized = 0;
    }

    /* Never free the device struct if a kernel I/O request still references an
     * embedded overlapped structure or transfer buffer. */
    if (!sdk_transport_can_free(device)) {
        return;
    }

    free(device);
}
