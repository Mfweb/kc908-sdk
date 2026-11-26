/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Linux libusb-1.0 FT601 transport layer: dynamic libusb-1.0 loader, FT601
 * Multi-Channel FIFO session commands, pre-armed asynchronous control pipe
 * framing, and 4-slot asynchronous IQ streaming.
 */

#include "internal.h"

#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/time.h>

/* FT601 Multi-Channel FIFO (600-mode) USB interfaces and session endpoint. */
#define FT601_IF_COMM        0
#define FT601_IF_DATA        1
#define FT601_EP_SESSION_OUT 0x01u

/* FT601 20-byte session command opcodes on endpoint 0x01. */
#define FT601_CMD_CLEAR      0x00u
#define FT601_CMD_READ       0x01u
#define FT601_CMD_SET_STREAM 0x02u
#define FT601_CMD_FLUSH      0x03u

/* Transfer timeouts matching the Windows D3XX transport. */
#define SESSION_TIMEOUT_MS   1000u
#define CTRL_TIMEOUT_MS      3500u
#define CTRL_DEADLINE_MS     1500u
#define IQ_READ_TIMEOUT_MS   500u
#define IQ_WRITE_TIMEOUT_MS  3500u
#define ABORT_REAP_MS        500u

/* -------------------------------------------------------------------------- */
/* Error mapping                                                              */
/* -------------------------------------------------------------------------- */

static int libusb_error_to_errno(int rc) {
    switch (rc) {
        case LIBUSB_SUCCESS:
            return 0;
        case LIBUSB_ERROR_INVALID_PARAM:
            return -EINVAL;
        case LIBUSB_ERROR_ACCESS:
            return -EACCES;
        case LIBUSB_ERROR_NO_DEVICE:
            return -ENODEV;
        case LIBUSB_ERROR_NOT_FOUND:
            return -ENOENT;
        case LIBUSB_ERROR_BUSY:
            return -EBUSY;
        case LIBUSB_ERROR_TIMEOUT:
            return -ETIMEDOUT;
        case LIBUSB_ERROR_OVERFLOW:
            return -EOVERFLOW;
        case LIBUSB_ERROR_PIPE:
            return -EPIPE;
        case LIBUSB_ERROR_INTERRUPTED:
            return -EINTR;
        case LIBUSB_ERROR_NO_MEM:
            return -ENOMEM;
        case LIBUSB_ERROR_NOT_SUPPORTED:
            return -ENOSYS;
        default:
            return -EIO;
    }
}

static int transfer_status_to_errno(int status) {
    switch (status) {
        case LIBUSB_TRANSFER_COMPLETED:
            return 0;
        case LIBUSB_TRANSFER_TIMED_OUT:
            return -ETIMEDOUT;
        case LIBUSB_TRANSFER_CANCELLED:
            return -ECANCELED;
        case LIBUSB_TRANSFER_STALL:
            return -EPIPE;
        case LIBUSB_TRANSFER_NO_DEVICE:
            return -ENODEV;
        case LIBUSB_TRANSFER_OVERFLOW:
            return -EOVERFLOW;
        default:
            return -EIO;
    }
}

static int sdk_libusb_error(ktos_device *device, int rc, const char *operation) {
    int  err = libusb_error_to_errno(rc);
    char description[192];

    if (rc == LIBUSB_ERROR_ACCESS) {
        snprintf(description, sizeof(description),
                 "%s (permission denied; install udev rule for 0403:601f)", operation);
        return sdk_failure(device, err, description);
    }
    if (device && device->transport.api.strerror) {
        snprintf(description, sizeof(description), "%s (%s)", operation,
                 device->transport.api.strerror(rc));
        return sdk_failure(device, err, description);
    }
    return sdk_failure(device, err, operation);
}

static int sdk_transfer_error(ktos_device *device, int status, const char *operation) {
    return sdk_failure(device, transfer_status_to_errno(status), operation);
}

/* -------------------------------------------------------------------------- */
/* Dynamic libusb-1.0 loader                                                  */
/* -------------------------------------------------------------------------- */

static int sdk_libusb_load(libusb_api *api) {
    Dl_info info;
    memset(api, 0, sizeof(*api));

    /* Prefer libusb-1.0.so.0 placed right next to libkcsdr.so (also used by
     * offline unit tests with the fake FT601 emulator). */
    if (dladdr((void *)(uintptr_t)&sdk_libusb_load, &info) && info.dli_fname) {
        const char *slash = strrchr(info.dli_fname, '/');
        if (slash) {
            size_t dir_len = (size_t)(slash - info.dli_fname) + 1u;
            char   candidate[PATH_MAX];
            if (dir_len + sizeof("libusb-1.0.so.0") <= sizeof(candidate)) {
                memcpy(candidate, info.dli_fname, dir_len);
                memcpy(candidate + dir_len, "libusb-1.0.so.0", sizeof("libusb-1.0.so.0"));
                api->module = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
            }
        }
    }

    if (!api->module) {
        api->module = dlopen("libusb-1.0.so.0", RTLD_NOW | RTLD_LOCAL);
    }
    if (!api->module) {
        api->module = dlopen("libusb-1.0.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!api->module) {
        return -ENOENT;
    }

#define LOAD_USB_PROC(member, symbol)                                                              \
    do {                                                                                           \
        void *sym_ = dlsym(api->module, symbol);                                                   \
        if (!sym_) {                                                                               \
            goto fail;                                                                             \
        }                                                                                          \
        memcpy(&api->member, &sym_, sizeof(api->member));                                          \
    } while (0)

    LOAD_USB_PROC(init, "libusb_init");
    LOAD_USB_PROC(exit, "libusb_exit");
    LOAD_USB_PROC(get_device_list, "libusb_get_device_list");
    LOAD_USB_PROC(free_device_list, "libusb_free_device_list");
    LOAD_USB_PROC(get_device_descriptor, "libusb_get_device_descriptor");
    LOAD_USB_PROC(open, "libusb_open");
    LOAD_USB_PROC(close, "libusb_close");
    LOAD_USB_PROC(kernel_driver_active, "libusb_kernel_driver_active");
    LOAD_USB_PROC(detach_kernel_driver, "libusb_detach_kernel_driver");
    LOAD_USB_PROC(claim_interface, "libusb_claim_interface");
    LOAD_USB_PROC(release_interface, "libusb_release_interface");
    LOAD_USB_PROC(bulk_transfer, "libusb_bulk_transfer");
    LOAD_USB_PROC(alloc_transfer, "libusb_alloc_transfer");
    LOAD_USB_PROC(free_transfer, "libusb_free_transfer");
    LOAD_USB_PROC(submit_transfer, "libusb_submit_transfer");
    LOAD_USB_PROC(cancel_transfer, "libusb_cancel_transfer");
    LOAD_USB_PROC(handle_events_timeout_completed, "libusb_handle_events_timeout_completed");

#undef LOAD_USB_PROC

    void *strerror_sym = dlsym(api->module, "libusb_strerror");
    memcpy(&api->strerror, &strerror_sym, sizeof(api->strerror));
    return 0;

fail:
    dlclose(api->module);
    memset(api, 0, sizeof(*api));
    return -ENOSYS;
}

/* -------------------------------------------------------------------------- */
/* FT601 20-byte session command helpers (endpoint 0x01)                      */
/* -------------------------------------------------------------------------- */

static int ft601_send_session(ktos_device *device, uint8_t pipe, uint8_t cmd, uint32_t length) {
    sdk_transport *t          = &device->transport;
    uint8_t        packet[20] = {0};

    sdk_mutex_lock(&t->session_lock);
    uint32_t seq = ++t->session_counter;
    if (seq == 0) {
        seq = ++t->session_counter;
    }
    write32(packet + 0, seq);
    packet[4] = pipe;
    packet[5] = cmd;
    write32(packet + 8, length);

    int actual = 0;
    int rc     = t->api.bulk_transfer(t->handle, FT601_EP_SESSION_OUT, packet, (int)sizeof(packet),
                                      &actual, SESSION_TIMEOUT_MS);
    sdk_mutex_unlock(&t->session_lock);

    if (rc != LIBUSB_SUCCESS) {
        return sdk_libusb_error(device, rc, "FT601 session command");
    }
    if (actual != (int)sizeof(packet)) {
        return sdk_failure(device, -EIO, "short FT601 session command");
    }
    return 0;
}

static int ft601_flush_pipe(ktos_device *device, uint8_t pipe) {
    int error = ft601_send_session(device, pipe, FT601_CMD_CLEAR, 0);
    if (!error) {
        error = ft601_send_session(device, pipe, FT601_CMD_FLUSH, 0);
    }
    return error;
}

static int ft601_set_stream_pipe(ktos_device *device, uint8_t pipe, uint32_t size) {
    int error = ft601_send_session(device, pipe, FT601_CMD_CLEAR, 0);
    if (!error) {
        error = ft601_send_session(device, pipe, FT601_CMD_SET_STREAM, size);
    }
    return error;
}

/* -------------------------------------------------------------------------- */
/* Asynchronous transfer callbacks and event pumping                          */
/* -------------------------------------------------------------------------- */

static void LIBUSB_CALL control_callback(struct libusb_transfer *transfer) {
    ktos_device   *device = transfer->user_data;
    sdk_transport *t      = &device->transport;

    t->control_status = (int)transfer->status;
    t->control_bytes  = (transfer->actual_length > 0) ? (uint32_t)transfer->actual_length : 0u;
    __atomic_store_n(&t->control_completed, 1, __ATOMIC_RELEASE);
}

static void LIBUSB_CALL iq_slot_callback(struct libusb_transfer *transfer) {
    linux_iq_slot *slot = transfer->user_data;

    slot->status = (int)transfer->status;
    slot->bytes  = (transfer->actual_length > 0) ? (uint32_t)transfer->actual_length : 0u;
    __atomic_store_n(&slot->completed, 1, __ATOMIC_RELEASE);
}

static int wait_completed(ktos_device *device, int *completed, uint64_t deadline_ms) {
    sdk_transport *t = &device->transport;

    while (!__atomic_load_n(completed, __ATOMIC_ACQUIRE)) {
        uint64_t now = sdk_monotonic_ms();
        if (now >= deadline_ms) {
            return -ETIMEDOUT;
        }

        uint64_t       remaining = deadline_ms - now;
        uint64_t       slice_ms  = remaining > 50u ? 50u : remaining;
        struct timeval tv        = {
                   .tv_sec  = 0,
                   .tv_usec = (suseconds_t)(slice_ms * 1000u),
        };

        int rc = t->api.handle_events_timeout_completed(t->ctx, &tv, completed);
        if (rc == LIBUSB_ERROR_INTERRUPTED) {
            continue;
        }
        if (rc != LIBUSB_SUCCESS) {
            return sdk_libusb_error(device, rc, "USB event wait");
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Control pipe asynchronous helpers                                          */
/* -------------------------------------------------------------------------- */

static void control_abort(ktos_device *device) {
    sdk_transport *t = &device->transport;
    if (t->control_pending && t->control_transfer) {
        int rc = t->api.cancel_transfer(t->control_transfer);
        if (rc == LIBUSB_SUCCESS || rc == LIBUSB_ERROR_NOT_FOUND) {
            (void)wait_completed(device, &t->control_completed,
                                 sdk_monotonic_ms() + ABORT_REAP_MS);
        }
        if (__atomic_load_n(&t->control_completed, __ATOMIC_ACQUIRE)) {
            t->control_pending = 0;
        }
    }
    if (t->handle) {
        (void)ft601_flush_pipe(device, PIPE_CTRL_IN);
    }
}

static void control_release(ktos_device *device) {
    sdk_transport *t = &device->transport;
    if (t->control_pending) {
        control_abort(device);
    }
    if (t->control_transfer && !t->control_pending) {
        t->api.free_transfer(t->control_transfer);
        t->control_transfer = NULL;
    }
}

static int control_submit(ktos_device *device, size_t offset) {
    sdk_transport *t = &device->transport;
    if (t->control_pending) {
        return sdk_failure(device, -EBUSY, "control read still pending");
    }
    if (!t->control_transfer) {
        t->control_transfer = t->api.alloc_transfer(0);
        if (!t->control_transfer) {
            return sdk_failure(device, -ENOMEM, "control initialize");
        }
    }

    int error = ft601_send_session(device, PIPE_CTRL_IN, FT601_CMD_READ, WIRE_IN_CHUNK);
    if (error) {
        return error;
    }

    t->control_bytes  = 0;
    t->control_status = 0;
    __atomic_store_n(&t->control_completed, 0, __ATOMIC_RELAXED);

    libusb_fill_bulk_transfer(t->control_transfer, t->handle, PIPE_CTRL_IN,
                              t->control_buffer + offset, (int)WIRE_IN_CHUNK, control_callback,
                              device, CTRL_TIMEOUT_MS);

    t->control_pending = 1;
    int rc             = t->api.submit_transfer(t->control_transfer);
    if (rc != LIBUSB_SUCCESS) {
        t->control_pending = 0;
        (void)ft601_flush_pipe(device, PIPE_CTRL_IN);
        return sdk_libusb_error(device, rc, "control submit");
    }
    return 0;
}

static int control_complete(ktos_device *device, uint32_t *actual, uint64_t deadline_ms) {
    sdk_transport *t = &device->transport;
    if (t->control_pending) {
        int err = wait_completed(device, &t->control_completed, deadline_ms);
        if (err) {
            control_abort(device);
            return (err == -ETIMEDOUT) ? sdk_failure(device, -ETIMEDOUT, "control deadline") : err;
        }
        t->control_pending = 0;
        if (t->control_status != LIBUSB_TRANSFER_COMPLETED) {
            int status = t->control_status;
            control_abort(device);
            return sdk_transfer_error(device, status, "control completion");
        }
    }
    *actual = t->control_bytes;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Transport lifecycle                                                        */
/* -------------------------------------------------------------------------- */

static int claim_interface_clean(ktos_device *device, int iface, int *claimed_flag) {
    sdk_transport *t  = &device->transport;
    int            rc = t->api.kernel_driver_active(t->handle, iface);
    if (rc == 1) {
        rc = t->api.detach_kernel_driver(t->handle, iface);
        if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_NOT_FOUND &&
            rc != LIBUSB_ERROR_NOT_SUPPORTED) {
            return sdk_libusb_error(device, rc, "detach kernel driver");
        }
    } else if (rc < 0 && rc != LIBUSB_ERROR_NOT_SUPPORTED) {
        return sdk_libusb_error(device, rc, "query kernel driver");
    }

    rc = t->api.claim_interface(t->handle, iface);
    if (rc != LIBUSB_SUCCESS) {
        return sdk_libusb_error(device, rc, "claim FT601 interface");
    }
    *claimed_flag = 1;
    return 0;
}

int sdk_transport_open(ktos_device *device, uint32_t index) {
    sdk_transport *t     = &device->transport;
    int            error = sdk_mutex_init(&t->session_lock);
    if (!error) {
        error = sdk_mutex_init(&t->abort_lock);
        if (error) {
            sdk_mutex_destroy(&t->session_lock);
            return error;
        }
        t->locks_initialized = 1;
    } else {
        return error;
    }

    error = sdk_libusb_load(&t->api);
    if (error) {
        return sdk_failure(device, error, "load libusb-1.0");
    }

    int rc = t->api.init(&t->ctx);
    if (rc != LIBUSB_SUCCESS) {
        return sdk_libusb_error(device, rc, "initialize libusb");
    }

    libusb_device **list  = NULL;
    ssize_t         count = t->api.get_device_list(t->ctx, &list);
    if (count < 0) {
        return sdk_libusb_error(device, (int)count, "enumerate FT601");
    }

    uint32_t       matched = 0;
    libusb_device *target  = NULL;
    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor desc;
        if (t->api.get_device_descriptor(list[i], &desc) != LIBUSB_SUCCESS) {
            continue;
        }
        if (desc.idVendor == FT601_VID && desc.idProduct == FT601_PID) {
            if (matched == index) {
                target = list[i];
                break;
            }
            matched++;
        }
    }

    if (!target) {
        t->api.free_device_list(list, 1);
        return sdk_failure(device, -ENODEV, "FT601 device index is not enumerated");
    }

    rc = t->api.open(target, &t->handle);
    t->api.free_device_list(list, 1);
    if (rc != LIBUSB_SUCCESS) {
        return sdk_libusb_error(device, rc, "open FT601");
    }

    error = claim_interface_clean(device, FT601_IF_COMM, &t->comm_claimed);
    if (error) {
        return error;
    }
    error = claim_interface_clean(device, FT601_IF_DATA, &t->data_claimed);
    if (error) {
        return error;
    }

    /* Clear any stale FT601 session state left behind by an earlier process. */
    error = ft601_flush_pipe(device, PIPE_CTRL_IN);
    if (error) {
        return error;
    }
    error = ft601_flush_pipe(device, PIPE_IQ_IN);
    if (error) {
        return error;
    }

    return 0;
}

int sdk_transport_is_open(const ktos_device *device) {
    return device && device->transport.handle != NULL;
}

void sdk_transport_abort_rx(ktos_device *device) {
    if (!device || !device->transport.locks_initialized) {
        return;
    }
    sdk_transport *t = &device->transport;
    sdk_mutex_lock(&t->abort_lock);
    for (unsigned i = 0; i < IQ_SLOTS; i++) {
        linux_iq_slot *slot = &t->slots[i];
        if (slot->transfer && slot->pending && !__atomic_load_n(&slot->completed, __ATOMIC_ACQUIRE)) {
            (void)t->api.cancel_transfer(slot->transfer);
        }
    }
    sdk_mutex_unlock(&t->abort_lock);
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

void sdk_transport_close(ktos_device *device) {
    sdk_transport *t = &device->transport;

    if (t->handle) {
        control_release(device);
        if (sdk_transport_can_free(device)) {
            if (t->comm_claimed) {
                (void)ft601_flush_pipe(device, PIPE_CTRL_IN);
                (void)ft601_flush_pipe(device, PIPE_IQ_IN);
            }
            if (t->data_claimed) {
                t->api.release_interface(t->handle, FT601_IF_DATA);
                t->data_claimed = 0;
            }
            if (t->comm_claimed) {
                t->api.release_interface(t->handle, FT601_IF_COMM);
                t->comm_claimed = 0;
            }
            t->api.close(t->handle);
        }
        t->handle = NULL;
    }

    if (!sdk_transport_can_free(device)) {
        return;
    }

    if (t->ctx) {
        t->api.exit(t->ctx);
        t->ctx = NULL;
    }
    if (t->api.module) {
        dlclose(t->api.module);
        memset(&t->api, 0, sizeof(t->api));
    }
    if (t->locks_initialized) {
        sdk_mutex_destroy(&t->session_lock);
        sdk_mutex_destroy(&t->abort_lock);
        t->locks_initialized = 0;
    }
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

    int written = 0;
    int rc      = t->api.bulk_transfer(t->handle, PIPE_CTRL_OUT, packet, (int)sizeof(packet),
                                       &written, CTRL_TIMEOUT_MS);
    if (rc != LIBUSB_SUCCESS || written != (int)sizeof(packet)) {
        if (reply) {
            control_abort(device);
        }
        return (rc != LIBUSB_SUCCESS) ? sdk_libusb_error(device, rc, "control write")
                                      : sdk_failure(device, -EIO, "short control write");
    }

    if (!reply) {
        sdk_sleep_ms(5);
        return 0;
    }

    uint8_t *block       = t->control_buffer;
    size_t   used        = 0;
    uint32_t expected_op = (op == 0x42u) ? 0x41u : op;
    uint64_t deadline    = sdk_monotonic_ms() + CTRL_DEADLINE_MS;

    while (sdk_monotonic_ms() < deadline) {
        uint32_t actual = 0;
        error           = control_complete(device, &actual, deadline);
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
/* Asynchronous 4-slot IQ RX streaming and synchronous IQ TX                  */
/* -------------------------------------------------------------------------- */

static int submit_iq_slot(ktos_device *device, linux_iq_slot *slot) {
    sdk_transport *t = &device->transport;

    slot->bytes  = 0;
    slot->used   = 0;
    slot->status = 0;
    __atomic_store_n(&slot->completed, 0, __ATOMIC_RELAXED);

    /* Use timeout=0 on queued URBs; ktos_iq_read() enforces IQ_READ_TIMEOUT_MS
     * on the active head slot so queued tail slots do not time out prematurely
     * at lower sample rates. */
    libusb_fill_bulk_transfer(slot->transfer, t->handle, PIPE_IQ_IN, slot->data, (int)IQ_CHUNK,
                              iq_slot_callback, slot, 0);

    sdk_mutex_lock(&t->abort_lock);
    slot->pending = 1;
    sdk_mutex_unlock(&t->abort_lock);

    int rc = t->api.submit_transfer(slot->transfer);
    if (rc != LIBUSB_SUCCESS) {
        sdk_mutex_lock(&t->abort_lock);
        slot->pending = 0;
        sdk_mutex_unlock(&t->abort_lock);
        return sdk_libusb_error(device, rc, "IQ submit");
    }
    return 0;
}

void sdk_rx_release(ktos_device *device) {
    sdk_transport *t         = &device->transport;
    unsigned       allocated = device->streaming;
    for (unsigned i = 0; i < IQ_SLOTS; i++) {
        allocated |= (t->slots[i].transfer != NULL) || (t->slots[i].data != NULL) ||
                     t->slots[i].pending;
    }
    if (!allocated) {
        return;
    }

    sdk_transport_abort_rx(device);
    uint64_t deadline = sdk_monotonic_ms() + ABORT_REAP_MS;

    for (unsigned i = 0; i < IQ_SLOTS; i++) {
        linux_iq_slot *slot = &t->slots[i];
        if (slot->pending) {
            (void)wait_completed(device, &slot->completed, deadline);
            /* Never free a transfer or buffer still owned by an active URB. */
            if (!__atomic_load_n(&slot->completed, __ATOMIC_ACQUIRE)) {
                continue;
            }
            sdk_mutex_lock(&t->abort_lock);
            slot->pending = 0;
            sdk_mutex_unlock(&t->abort_lock);
        }

        sdk_mutex_lock(&t->abort_lock);
        struct libusb_transfer *tr = slot->transfer;
        slot->transfer             = NULL;
        sdk_mutex_unlock(&t->abort_lock);

        if (tr) {
            t->api.free_transfer(tr);
        }
        free(slot->data);
        memset(slot, 0, sizeof(*slot));
    }

    if (t->handle) {
        (void)ft601_flush_pipe(device, PIPE_IQ_IN);
    }
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
        error = ft601_set_stream_pipe(device, PIPE_IQ_IN, IQ_CHUNK);
        if (error) {
            goto done;
        }

        for (unsigned i = 0; i < IQ_SLOTS; i++) {
            linux_iq_slot *slot = &t->slots[i];
            if (slot->pending) {
                error = -EBUSY;
                goto done;
            }
            slot->data = malloc(IQ_CHUNK);
            if (!slot->data) {
                error = -ENOMEM;
                goto done;
            }
            struct libusb_transfer *tr = t->api.alloc_transfer(0);
            if (!tr) {
                error = -ENOMEM;
                goto done;
            }
            sdk_mutex_lock(&t->abort_lock);
            slot->transfer = tr;
            sdk_mutex_unlock(&t->abort_lock);

            error = submit_iq_slot(device, slot);
            if (error) {
                goto done;
            }
        }
        device->streaming = 1;
    }

    unsigned empty_transfers = 0;
    while (*actual < bytes && !sdk_atomic_load(&device->stopping)) {
        linux_iq_slot *slot = &t->slots[device->slot];
        if (slot->pending) {
            error = wait_completed(device, &slot->completed,
                                   sdk_monotonic_ms() + IQ_READ_TIMEOUT_MS);
            if (error) {
                if (error == -ETIMEDOUT) {
                    error = sdk_failure(device, -ETIMEDOUT, "IQ completion timeout");
                }
                goto done;
            }
            sdk_mutex_lock(&t->abort_lock);
            slot->pending = 0;
            sdk_mutex_unlock(&t->abort_lock);

            if (slot->status != LIBUSB_TRANSFER_COMPLETED) {
                error = sdk_transfer_error(device, slot->status, "IQ completion");
                goto done;
            }
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
            snprintf(description, sizeof(description), "IQ completion length %u", slot->bytes);
            error = sdk_failure(device, -EPROTO, description);
            goto done;
        }

        uint32_t copy_bytes = slot->bytes - slot->used;
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

    *actual = 0;
    sdk_mutex_lock(&device->tx);
    sdk_transport *t     = &device->transport;
    int            error = 0;

    if (device->mode != KTOS_MODE_IQ) {
        error = -EPERM;
        goto done;
    }

    const uint8_t *ptr       = (const uint8_t *)data;
    uint32_t       remaining = bytes;
    while (remaining > 0) {
        int chunk       = (remaining > (uint32_t)INT_MAX) ? INT_MAX : (int)remaining;
        int transferred = 0;
        int rc          = t->api.bulk_transfer(t->handle, PIPE_IQ_OUT, (unsigned char *)ptr, chunk,
                                               &transferred, IQ_WRITE_TIMEOUT_MS);
        if (transferred > 0) {
            *actual += (uint32_t)transferred;
            ptr += transferred;
            remaining -= (uint32_t)transferred;
        }
        if (rc != LIBUSB_SUCCESS) {
            error = sdk_libusb_error(device, rc, "IQ write");
            goto done;
        }
        if (transferred != chunk) {
            error = sdk_failure(device, -EIO, "short IQ write");
            goto done;
        }
    }

done:
    sdk_mutex_unlock(&device->tx);
    return error;
}
