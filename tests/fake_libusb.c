/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Test-only libusb-1.0 / FTDI FT601 emulator for offline Linux SDK verification.
 * Never ship this shared library in production deployments.
 */

#include "ktos_protocol.h"

#include <libusb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FAKE_API __attribute__((visibility("default")))

#define MAX_PENDING_IQ 16

typedef struct {
    struct libusb_transfer *transfer;
    int                     cancelled;
} pending_iq;

static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;

static uint8_t  queue[4096];
static uint32_t tx_id, received, total, sequence;
static char     request_buf[4098];
static char     result_buf[65536];
static size_t   result_bytes;
static uint8_t  image_buf[200000];
static uint8_t  image_hash[32];
static uint32_t image_size, image_offset, image_id;
static unsigned device_mode;
static unsigned streams, submissions, stream_enabled;
static uint32_t last_session_seq;

static int                     comm_claimed;
static int                     data_claimed;
static int                     control_session_armed;
static struct libusb_transfer *control_transfer;
static int                     control_ready;
static int                     control_cancelled;

static pending_iq iq_queue[MAX_PENDING_IQ];
static unsigned   iq_count;

static int fake_ctx_obj;
static int fake_dev_obj;
static int fake_handle_obj;

static int injected_failure(void) {
    const char *value = getenv("KTOS_TEST_FAILURE");
    return (value && value[0]) ? atoi(value) : 0;
}

static int env_enabled(const char *name) {
    const char *value = getenv(name);
    return value && value[0] && strcmp(value, "0") != 0;
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (uint8_t)(value >> (8u * i));
    }
}

static void handle_image_request(const uint8_t *p, size_t bytes) {
    if (bytes < 16) {
        _exit(94);
    }

    uint32_t op     = read_le32(p);
    uint32_t offset = read_le32(p + 8);
    uint32_t count  = read_le32(p + 12);

    memset(result_buf, 0, sizeof(result_buf));
    size_t payload = 0;

    if (op == 1) {
        if (count > sizeof(image_buf) || bytes != 49) {
            _exit(93);
        }
        image_size   = count;
        image_offset = 0;
        image_id     = 7;
        memcpy(image_hash, p + 17, 32);
    } else if (op == 2) {
        if (offset != image_offset || offset + count > image_size || bytes != 16 + count) {
            _exit(92);
        }
        memcpy(image_buf + offset, p + 16, count);
        image_offset += count;
    } else if (op == 4) {
        image_id     = 8;
        image_offset = 0;
    } else if (op == 5) {
        if (offset > image_size) {
            _exit(91);
        }
        payload = image_size - offset;
        if (payload > count) {
            payload = count;
        }
        memcpy(result_buf + 320, image_buf + offset, payload);
        image_offset = offset + (uint32_t)payload;

        if (payload && env_enabled("KTOS_TEST_IMAGE_CORRUPT")) {
            result_buf[320] ^= 1;
        }
    } else if (op == 6) {
        image_id = 0;
    }

    write_le32((uint8_t *)result_buf + 0, 0x474d494bu);
    write_le32((uint8_t *)result_buf + 8, image_id);
    write_le32((uint8_t *)result_buf + 12, image_size);
    write_le32((uint8_t *)result_buf + 16, image_offset);
    write_le32((uint8_t *)result_buf + 20, 100);
    write_le32((uint8_t *)result_buf + 24, 100);
    write_le32((uint8_t *)result_buf + 28, 1);
    memcpy(result_buf + 32, image_hash, 32);
    strcpy(result_buf + 64, "/mnt/sdcard/tmp/ktos-image-0123456789abcdef0123456789abcdef.png");
    result_bytes = 320 + payload;
}

static void enqueue_reply_frame(uint32_t op, const void *payload, size_t bytes) {
    /* Replies can arrive before OUT returns; require session cmd + IN URB to be pre-armed. */
    if (!control_transfer || !control_session_armed) {
        _exit(95);
    }

    memset(queue, 0, sizeof(queue));
    write_le32(queue + 0, 0x89abcdefu);
    write_le32(queue + 4, 0x10234567u);
    write_le32(queue + 12, 0x80000000u + (uint32_t)bytes + 12u);
    write_le32(queue + 16, op);
    memcpy(queue + 20, payload, bytes);
    memcpy(control_transfer->buffer, queue, sizeof(queue));
    control_ready = 1;
}

static int handle_session_command(const uint8_t *p, int length, int *actual) {
    if (length != 20) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    uint32_t seq  = read_le32(p + 0);
    uint8_t  pipe = p[4];
    uint8_t  cmd  = p[5];
    uint32_t len  = read_le32(p + 8);

    if (seq == 0 || seq <= last_session_seq) {
        _exit(90);
    }
    last_session_seq = seq;

    if (cmd == 0x00u) {
        if (pipe == 0x82u) {
            control_session_armed = 0;
        } else if (pipe == 0x83u) {
            stream_enabled = 0;
        }
    } else if (cmd == 0x01u) {
        if (pipe != 0x82u || len < 4096u) {
            return LIBUSB_ERROR_INVALID_PARAM;
        }
        control_session_armed = 1;
    } else if (cmd == 0x02u) {
        if (pipe != 0x83u || len == 0u) {
            return LIBUSB_ERROR_INVALID_PARAM;
        }
        streams++;
        submissions    = 0;
        stream_enabled = 1;
    } else if (cmd == 0x03u) {
        if (pipe == 0x82u) {
            control_session_armed = 0;
        } else if (pipe == 0x83u) {
            stream_enabled = 0;
        }
    } else {
        return LIBUSB_ERROR_INVALID_PARAM;
    }

    *actual = 20;
    return LIBUSB_SUCCESS;
}

static int handle_control_out(const uint8_t *p, int length, int *actual) {
    if (length != 200) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    if (injected_failure() == 5) {
        return LIBUSB_ERROR_IO;
    }

    *actual     = 200;
    uint32_t op = read_le32(p + 8);
    size_t   n  = (read_le32(p + 4) & 0xffffu) - 12u;

    if (op == 0x41u) {
        char serial[20] = "KTOS-TEST";
        enqueue_reply_frame(0x41u, serial, 20);
        return LIBUSB_SUCCESS;
    }
    if (op == 0x42u) {
        uint8_t caps[48] = {0xf5, 0x12};
        memcpy(caps + 2, "KC908V", 6);
        if (device_mode) {
            memcpy(caps + 34, "KTOS", 4);
            caps[38] = 1;
            caps[40] = (uint8_t)(device_mode - 1);
            caps[41] = KTOS_CAP_KEEPALIVE;
        }
        enqueue_reply_frame(0x41u, caps, device_mode ? 48 : 34);
        return LIBUSB_SUCCESS;
    }

    if (!device_mode) {
        return LIBUSB_SUCCESS;
    }
    if (op != KTOS_USB_COMMAND && op != KTOS_USB_KEEPALIVE) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }

    ktos_packet req;
    if (ktos_packet_decode(&req, p + 12, n) != 0) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    if (op == KTOS_USB_KEEPALIVE) {
        return (req.operation == KTOS_PING && !req.offset && !req.total && !req.status &&
                !req.bytes)
                   ? LIBUSB_SUCCESS
                   : LIBUSB_ERROR_INVALID_PARAM;
    }

    ktos_packet reply = {.operation = req.operation, .id = req.id};
    if (req.operation == KTOS_BEGIN) {
        tx_id    = req.id;
        total    = req.total;
        received = 0;
    } else if (req.operation == KTOS_WRITE) {
        if (tx_id != req.id || req.offset != received ||
            received + req.bytes > sizeof(request_buf) - 1) {
            return LIBUSB_ERROR_INVALID_PARAM;
        }
        memcpy(request_buf + received, req.data, req.bytes);
        received += (uint32_t)req.bytes;
    } else if (req.operation == KTOS_EXECUTE) {
        request_buf[received] = '\0';
        if (received != total) {
            return LIBUSB_ERROR_INVALID_PARAM;
        }
        if (memcmp(request_buf, "image ", 6) == 0) {
            if (env_enabled("KTOS_TEST_IMAGE")) {
                handle_image_request((uint8_t *)request_buf + 6, received - 6);
            } else {
                memcpy(result_buf, request_buf + 6, received - 6);
                result_bytes = received - 6;
            }
        } else if (strcmp(request_buf, "agent fft") == 0) {
            memset(result_buf, 'a', 16000);
            memcpy(result_buf, "{\"ok\":true,\"data\":\"", 19);
            memcpy(result_buf + 16000 - 3, "\"}\n", 3);
        } else if (strcmp(request_buf, "agent vsa") == 0) {
            strcpy(result_buf,
                   "{\"ok\":true,\"active\":1,\"running\":1,\"locked\":1,"
                   "\"config\":{\"modulation_name\":\"16QAM\",\"symbols\":512},"
                   "\"metrics\":{\"evm_rms_pct\":2.34,\"mer_db\":32.6}}\n");
        } else if (strncmp(request_buf, "agent vsa_frame", 15) == 0) {
            strcpy(result_buf,
                   "{\"ok\":true,\"sequence\":7,\"modulation_name\":\"16QAM\","
                   "\"points\":[[0.31,0.31]],\"spectrum_db\":[-30.0]}\n");
        } else if (strcmp(request_buf, "agent gps") == 0) {
            strcpy(result_buf,
                   "{\"ok\":true,\"enabled\":1,\"active\":1,\"have_position\":true,"
                   "\"fix\":{\"mode\":\"3d\",\"latitude\":36.6512000,\"longitude\":117.1200000},"
                   "\"satellites_tracked\":6,\"satellites\":[{\"prn\":1,\"cn0_dbhz\":41.5}]}\n");
        } else if (strncmp(request_buf, "agent gps_track", 15) == 0) {
            strcpy(result_buf,
                   "{\"ok\":true,\"count\":1,\"offset\":0,\"next\":1,\"done\":true,"
                   "\"points\":[{\"lat\":36.6512000,\"lon\":117.1200000}]}\n");
        } else if (strcmp(request_buf, "agent action gps_sync_location") == 0) {
            strcpy(result_buf, "{\"ok\":false,\"code\":-11,\"error\":\"Try again\"}\n");
        } else if (strcmp(request_buf, "agent drm") == 0) {
            strcpy(result_buf,
                   "{\"ok\":true,\"enabled\":1,\"active\":1,\"state\":\"audio\","
                   "\"lock\":{\"time\":true,\"frame\":true,\"fac\":true,\"sdc\":true,"
                   "\"msc\":true,\"audio\":true},"
                   "\"profile\":\"auto\",\"robustness_mode\":\"B\",\"snr_db\":19.5,"
                   "\"services_count\":1,\"active_service\":0,"
                   "\"services\":[{\"index\":0,\"id\":1193046,\"label\":\"CNR-1\","
                   "\"audio\":true,\"codec\":\"xHE-AAC\"}],"
                   "\"text\":\"DRM Live\"}\n");
        } else {
            strcpy(result_buf, "{\"ok\":true}\n");
        }
    } else if (req.operation == KTOS_READ) {
        size_t length = (memcmp(request_buf, "image ", 6) == 0)   ? result_bytes
                        : (strcmp(request_buf, "agent fft") == 0) ? 16000
                                                                  : strlen(result_buf);
        reply.offset  = req.offset;
        reply.total   = (uint32_t)length;
        if (req.offset > length) {
            return LIBUSB_ERROR_INVALID_PARAM;
        }
        reply.bytes = length - req.offset;
        if (reply.bytes > KTOS_REPLY_FRAGMENT_BYTES) {
            reply.bytes = KTOS_REPLY_FRAGMENT_BYTES;
        }
        reply.data = (uint8_t *)result_buf + req.offset;
    }

    uint8_t encoded[4076];
    size_t  encoded_size = 0;
    if (ktos_packet_encode(encoded, sizeof(encoded), &reply, &encoded_size) != 0) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    enqueue_reply_frame(op, encoded, encoded_size);
    return LIBUSB_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Emulated libusb-1.0 exports                                                */
/* -------------------------------------------------------------------------- */

FAKE_API int libusb_init(libusb_context **ctx) {
    if (ctx) {
        *ctx = (libusb_context *)&fake_ctx_obj;
    }
    return LIBUSB_SUCCESS;
}

FAKE_API void libusb_exit(libusb_context *ctx) {
    (void)ctx;
}

FAKE_API ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list) {
    (void)ctx;
    if (injected_failure() == 1) {
        *list = calloc(1, sizeof(libusb_device *));
        return *list ? 0 : LIBUSB_ERROR_NO_MEM;
    }
    libusb_device **arr = calloc(2, sizeof(libusb_device *));
    if (!arr) {
        return LIBUSB_ERROR_NO_MEM;
    }
    arr[0] = (libusb_device *)&fake_dev_obj;
    *list  = arr;
    return 1;
}

FAKE_API void libusb_free_device_list(libusb_device **list, int unref_devices) {
    (void)unref_devices;
    free(list);
}

FAKE_API int libusb_get_device_descriptor(libusb_device *dev,
                                          struct libusb_device_descriptor *desc) {
    if (dev != (libusb_device *)&fake_dev_obj || !desc) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    memset(desc, 0, sizeof(*desc));
    desc->bLength            = sizeof(*desc);
    desc->bDescriptorType    = LIBUSB_DT_DEVICE;
    desc->idVendor           = 0x0403u;
    desc->idProduct          = 0x601fu;
    desc->bNumConfigurations = 1;
    return LIBUSB_SUCCESS;
}

FAKE_API int libusb_open(libusb_device *dev, libusb_device_handle **handle) {
    if (dev != (libusb_device *)&fake_dev_obj || !handle) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    pthread_mutex_lock(&fake_lock);
    const char *mode_env = getenv("KTOS_TEST_MODE");
    device_mode          = (mode_env && mode_env[0]) ? (unsigned)atoi(mode_env) : 0u;
    last_session_seq     = 0;
    control_session_armed = 0;
    control_transfer     = NULL;
    control_ready        = 0;
    control_cancelled    = 0;
    iq_count             = 0;
    comm_claimed         = 0;
    data_claimed         = 0;
    stream_enabled       = 0;
    int fail             = injected_failure();
    pthread_mutex_unlock(&fake_lock);

    if (fail == 2) {
        *handle = NULL;
        return LIBUSB_ERROR_ACCESS;
    }
    *handle = (libusb_device_handle *)&fake_handle_obj;
    return LIBUSB_SUCCESS;
}

FAKE_API void libusb_close(libusb_device_handle *handle) {
    if (handle != (libusb_device_handle *)&fake_handle_obj) {
        _exit(99);
    }
    pthread_mutex_lock(&fake_lock);
    int leaked = (control_transfer != NULL) || (iq_count != 0) || comm_claimed || data_claimed;
    pthread_mutex_unlock(&fake_lock);
    if (leaked) {
        _exit(96);
    }
}

FAKE_API int libusb_kernel_driver_active(libusb_device_handle *handle, int interface_number) {
    (void)handle;
    (void)interface_number;
    return 0;
}

FAKE_API int libusb_detach_kernel_driver(libusb_device_handle *handle, int interface_number) {
    (void)handle;
    (void)interface_number;
    return LIBUSB_SUCCESS;
}

FAKE_API int libusb_claim_interface(libusb_device_handle *handle, int interface_number) {
    if (handle != (libusb_device_handle *)&fake_handle_obj) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    pthread_mutex_lock(&fake_lock);
    if (interface_number == 0) {
        comm_claimed = 1;
    } else if (interface_number == 1) {
        data_claimed = 1;
    } else {
        pthread_mutex_unlock(&fake_lock);
        return LIBUSB_ERROR_NOT_FOUND;
    }
    pthread_mutex_unlock(&fake_lock);
    return LIBUSB_SUCCESS;
}

FAKE_API int libusb_release_interface(libusb_device_handle *handle, int interface_number) {
    if (handle != (libusb_device_handle *)&fake_handle_obj) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    pthread_mutex_lock(&fake_lock);
    if (interface_number == 0) {
        comm_claimed = 0;
    } else if (interface_number == 1) {
        data_claimed = 0;
    }
    pthread_mutex_unlock(&fake_lock);
    return LIBUSB_SUCCESS;
}

FAKE_API int libusb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint,
                                  unsigned char *data, int length, int *actual_length,
                                  unsigned int timeout) {
    (void)timeout;
    if (handle != (libusb_device_handle *)&fake_handle_obj || !data || !actual_length ||
        length <= 0) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }

    *actual_length = 0;
    pthread_mutex_lock(&fake_lock);
    int rc;
    if (endpoint == 0x01u) {
        if (!comm_claimed) {
            pthread_mutex_unlock(&fake_lock);
            return LIBUSB_ERROR_ACCESS;
        }
        rc = handle_session_command(data, length, actual_length);
    } else if (endpoint == 0x02u) {
        if (!data_claimed) {
            pthread_mutex_unlock(&fake_lock);
            return LIBUSB_ERROR_ACCESS;
        }
        rc = handle_control_out(data, length, actual_length);
    } else if (endpoint == 0x03u) {
        if (!data_claimed) {
            pthread_mutex_unlock(&fake_lock);
            return LIBUSB_ERROR_ACCESS;
        }
        *actual_length = length;
        rc             = LIBUSB_SUCCESS;
    } else {
        rc = LIBUSB_ERROR_INVALID_PARAM;
    }
    pthread_mutex_unlock(&fake_lock);
    return rc;
}

FAKE_API struct libusb_transfer *libusb_alloc_transfer(int iso_packets) {
    (void)iso_packets;
    return calloc(1, sizeof(struct libusb_transfer));
}

FAKE_API void libusb_free_transfer(struct libusb_transfer *transfer) {
    if (!transfer) {
        return;
    }
    pthread_mutex_lock(&fake_lock);
    int active = (transfer == control_transfer);
    for (unsigned i = 0; i < iq_count; i++) {
        if (iq_queue[i].transfer == transfer) {
            active = 1;
            break;
        }
    }
    pthread_mutex_unlock(&fake_lock);
    if (active) {
        _exit(97);
    }
    free(transfer);
}

FAKE_API int libusb_submit_transfer(struct libusb_transfer *transfer) {
    if (!transfer || transfer->dev_handle != (libusb_device_handle *)&fake_handle_obj ||
        !transfer->buffer || transfer->length <= 0) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }

    pthread_mutex_lock(&fake_lock);
    if (!data_claimed) {
        pthread_mutex_unlock(&fake_lock);
        return LIBUSB_ERROR_ACCESS;
    }

    if (transfer->endpoint == 0x82u) {
        if (transfer->length < 4096 || !control_session_armed || control_transfer ||
            injected_failure() == 4) {
            pthread_mutex_unlock(&fake_lock);
            return LIBUSB_ERROR_IO;
        }
        control_transfer  = transfer;
        control_ready     = 0;
        control_cancelled = 0;
        pthread_mutex_unlock(&fake_lock);
        return LIBUSB_SUCCESS;
    }

    if (transfer->endpoint == 0x83u) {
        if (!stream_enabled || iq_count >= MAX_PENDING_IQ) {
            pthread_mutex_unlock(&fake_lock);
            return LIBUSB_ERROR_IO;
        }
        iq_queue[iq_count].transfer  = transfer;
        iq_queue[iq_count].cancelled = 0;
        iq_count++;
        pthread_mutex_unlock(&fake_lock);
        return LIBUSB_SUCCESS;
    }

    pthread_mutex_unlock(&fake_lock);
    return LIBUSB_ERROR_INVALID_PARAM;
}

FAKE_API int libusb_cancel_transfer(struct libusb_transfer *transfer) {
    if (!transfer) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    pthread_mutex_lock(&fake_lock);
    if (transfer == control_transfer) {
        control_cancelled = 1;
        pthread_mutex_unlock(&fake_lock);
        return LIBUSB_SUCCESS;
    }
    for (unsigned i = 0; i < iq_count; i++) {
        if (iq_queue[i].transfer == transfer) {
            iq_queue[i].cancelled = 1;
            pthread_mutex_unlock(&fake_lock);
            return LIBUSB_SUCCESS;
        }
    }
    pthread_mutex_unlock(&fake_lock);
    return LIBUSB_ERROR_NOT_FOUND;
}

FAKE_API int libusb_handle_events_timeout_completed(libusb_context *ctx, struct timeval *tv,
                                                    int *completed) {
    (void)ctx;
    (void)tv;
    (void)completed;

    struct libusb_transfer *ready_list[MAX_PENDING_IQ + 1];
    unsigned                ready_count = 0;

    pthread_mutex_lock(&fake_lock);
    if (control_transfer && (control_ready || control_cancelled || injected_failure() == 6)) {
        struct libusb_transfer *tr = control_transfer;
        if (control_cancelled) {
            tr->status        = LIBUSB_TRANSFER_CANCELLED;
            tr->actual_length = 0;
        } else if (injected_failure() == 6) {
            tr->status        = LIBUSB_TRANSFER_TIMED_OUT;
            tr->actual_length = 0;
        } else {
            tr->status        = LIBUSB_TRANSFER_COMPLETED;
            tr->actual_length = 4096;
        }
        control_transfer      = NULL;
        control_session_armed = 0;
        control_ready         = 0;
        control_cancelled     = 0;
        ready_list[ready_count++] = tr;
    }

    while (iq_count > 0) {
        pending_iq item = iq_queue[0];
        memmove(&iq_queue[0], &iq_queue[1], (iq_count - 1) * sizeof(pending_iq));
        iq_count--;

        struct libusb_transfer *tr = item.transfer;
        if (item.cancelled) {
            tr->status        = LIBUSB_TRANSFER_CANCELLED;
            tr->actual_length = 0;
        } else {
            submissions++;
            if (injected_failure() == 3 || (streams > 1 && submissions == 1)) {
                tr->status        = LIBUSB_TRANSFER_COMPLETED;
                tr->actual_length = 0;
            } else {
                uint32_t *words = (uint32_t *)tr->buffer;
                for (int i = 0; i < tr->length / 4; i++) {
                    words[i] = sequence++;
                }
                tr->status        = LIBUSB_TRANSFER_COMPLETED;
                tr->actual_length = tr->length;
            }
        }
        ready_list[ready_count++] = tr;
    }
    pthread_mutex_unlock(&fake_lock);

    for (unsigned i = 0; i < ready_count; i++) {
        if (ready_list[i]->callback) {
            ready_list[i]->callback(ready_list[i]);
        }
    }
    return LIBUSB_SUCCESS;
}

FAKE_API const char *libusb_strerror(int errcode) {
    switch (errcode) {
        case LIBUSB_SUCCESS:
            return "Success";
        case LIBUSB_ERROR_IO:
            return "Input/Output Error";
        case LIBUSB_ERROR_INVALID_PARAM:
            return "Invalid parameter";
        case LIBUSB_ERROR_ACCESS:
            return "Access denied";
        case LIBUSB_ERROR_NO_DEVICE:
            return "No such device";
        case LIBUSB_ERROR_NOT_FOUND:
            return "Entity not found";
        case LIBUSB_ERROR_BUSY:
            return "Resource busy";
        case LIBUSB_ERROR_TIMEOUT:
            return "Operation timed out";
        default:
            return "Other error";
    }
}
