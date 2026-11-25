/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Test-only FTDI FT601 (FTD3XX.dll) emulator for offline SDK verification.
 * Never ship this DLL in production deployments.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ktos_protocol.h"

#include <stdlib.h>
#include <string.h>

#define FAKE_API __declspec(dllexport) ULONG WINAPI

static uint8_t     queue[4096];
static uint32_t    tx_id, received, total, sequence;
static char        request_buf[4098];
static char        result_buf[65536];
static size_t      result_bytes;
static uint8_t     image_buf[200000];
static uint8_t     image_hash[32];
static uint32_t    image_size, image_offset, image_id;
static unsigned    device_mode;
static unsigned    streams, submissions, stream_enabled;
static void       *control_buffer;
static OVERLAPPED *control_overlap;
static unsigned    control_ready, control_cancelled;

static int injected_failure(void) {
    char value[8] = {0};
    GetEnvironmentVariableA("KTOS_TEST_FAILURE", value, sizeof(value));
    return atoi(value);
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
        ExitProcess(94);
    }

    uint32_t op     = read_le32(p);
    uint32_t offset = read_le32(p + 8);
    uint32_t count  = read_le32(p + 12);

    memset(result_buf, 0, sizeof(result_buf));
    size_t payload = 0;

    if (op == 1) {
        if (count > sizeof(image_buf) || bytes != 49) {
            ExitProcess(93);
        }
        image_size   = count;
        image_offset = 0;
        image_id     = 7;
        memcpy(image_hash, p + 17, 32);
    } else if (op == 2) {
        if (offset != image_offset || offset + count > image_size || bytes != 16 + count) {
            ExitProcess(92);
        }
        memcpy(image_buf + offset, p + 16, count);
        image_offset += count;
    } else if (op == 4) {
        image_id     = 8;
        image_offset = 0;
    } else if (op == 5) {
        if (offset > image_size) {
            ExitProcess(91);
        }
        payload = image_size - offset;
        if (payload > count) {
            payload = count;
        }
        memcpy(result_buf + 320, image_buf + offset, payload);
        image_offset = offset + (uint32_t)payload;

        char corrupt[8];
        if (payload && GetEnvironmentVariableA("KTOS_TEST_IMAGE_CORRUPT", corrupt, sizeof(corrupt))) {
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
    /* Replies can arrive before the synchronous OUT call returns; require IN to be pre-armed. */
    if (!control_buffer || !control_overlap) {
        ExitProcess(95);
    }

    memset(queue, 0, sizeof(queue));
    write_le32(queue + 0, 0x89abcdefu);
    write_le32(queue + 4, 0x10234567u);
    write_le32(queue + 12, 0x80000000u + (uint32_t)bytes + 12u);
    write_le32(queue + 16, op);
    memcpy(queue + 20, payload, bytes);
    memcpy(control_buffer, queue, sizeof(queue));
    control_ready = 1;
}

/* -------------------------------------------------------------------------- */
/* Emulated FTD3XX exports                                                    */
/* -------------------------------------------------------------------------- */

FAKE_API FT_CreateDeviceInfoList(ULONG *count) {
    *count = injected_failure() == 1 ? 0 : 1;
    return 0;
}

FAKE_API FT_Create(void *index, DWORD flags, void **handle) {
    (void)index;
    (void)flags;

    char value[8] = {0};
    GetEnvironmentVariableA("KTOS_TEST_MODE", value, sizeof(value));
    device_mode = (unsigned)atoi(value);

    *handle = injected_failure() == 2 ? (void *)0xdead : (void *)1;
    return injected_failure() == 2 ? 3 : 0;
}

FAKE_API FT_Close(void *handle) {
    if (handle != (void *)1) {
        ExitProcess(99);
    }
    if (control_buffer) {
        ExitProcess(96);
    }
    return 0;
}

FAKE_API FT_GetVIDPID(void *handle, USHORT *vid, USHORT *pid) {
    (void)handle;
    *vid = 0x0403;
    *pid = 0x601f;
    return 0;
}

FAKE_API FT_SetPipeTimeout(void *handle, UCHAR pipe, ULONG timeout) {
    (void)handle;
    (void)pipe;
    (void)timeout;
    return 0;
}

FAKE_API FT_AbortPipe(void *handle, UCHAR pipe) {
    (void)handle;
    if (pipe == 0x83 && !stream_enabled) {
        ExitProcess(98);
    }
    if (pipe == 0x82 && control_buffer) {
        control_cancelled = 1;
    }
    return 0;
}

FAKE_API FT_InitializeOverlapped(void *handle, OVERLAPPED *overlap) {
    (void)handle;
    memset(overlap, 0, sizeof(*overlap));
    return 0;
}

FAKE_API FT_ReleaseOverlapped(void *handle, OVERLAPPED *overlap) {
    (void)handle;
    if (overlap == control_overlap) {
        ExitProcess(97);
    }
    return 0;
}

FAKE_API FT_GetOverlappedResult(void *handle, OVERLAPPED *overlap, ULONG *transferred, BOOL wait) {
    (void)handle;
    (void)wait;

    if (overlap == control_overlap) {
        ULONG status = control_cancelled ? 32 : (injected_failure() == 6 ? 19 : 0);
        if (!status && !control_ready) {
            return 24;
        }
        *transferred      = status ? 0 : 4096;
        control_buffer    = NULL;
        control_overlap   = NULL;
        control_ready     = 0;
        control_cancelled = 0;
        return status;
    }

    *transferred = (ULONG)overlap->InternalHigh;
    return 0;
}

FAKE_API FT_SetStreamPipe(void *handle, BOOL all_write, BOOL all_read, UCHAR pipe, ULONG size) {
    (void)handle;
    (void)all_write;
    (void)all_read;
    (void)pipe;
    (void)size;

    streams++;
    submissions    = 0;
    stream_enabled = 1;
    return 0;
}

FAKE_API FT_ClearStreamPipe(void *handle, BOOL all_write, BOOL all_read, UCHAR pipe) {
    (void)handle;
    (void)all_write;
    (void)all_read;
    (void)pipe;

    stream_enabled = 0;
    return 0;
}

FAKE_API FT_ReadPipe(void *handle, UCHAR pipe, void *buffer, ULONG bytes, ULONG *actual,
                     OVERLAPPED *overlap) {
    (void)handle;

    if (pipe == 0x82) {
        if (bytes < 4096 || !overlap || control_buffer || injected_failure() == 4) {
            return 6;
        }
        control_buffer    = buffer;
        control_overlap   = overlap;
        control_ready     = 0;
        control_cancelled = 0;
        *actual           = 0;
        return 24;
    }

    if (pipe != 0x83) {
        return 6;
    }

    submissions++;
    if (injected_failure() == 3 || (streams > 1 && submissions == 1)) {
        *actual = 0;
        if (overlap) {
            overlap->InternalHigh = 0;
        }
        return overlap ? 24 : 0;
    }

    uint32_t *words = buffer;
    for (ULONG i = 0; i < bytes / 4; i++) {
        words[i] = sequence++;
    }
    *actual = bytes;

    int pending = overlap && (submissions & 1u);
    if (overlap) {
        overlap->InternalHigh = pending ? bytes : 0;
    }
    return pending ? 24 : 0;
}

FAKE_API FT_WritePipe(void *handle, UCHAR pipe, void *buffer, ULONG bytes, ULONG *actual,
                      OVERLAPPED *overlap) {
    (void)handle;
    (void)overlap;

    *actual = bytes;
    if (pipe == 3) {
        return 0;
    }
    if (pipe != 2 || bytes != 200) {
        return 6;
    }
    if (injected_failure() == 5) {
        return 4;
    }

    uint8_t *p  = buffer;
    uint32_t op = read_le32(p + 8);
    size_t   n  = (read_le32(p + 4) & 0xffffu) - 12u;

    if (op == 0x41) {
        char serial[20] = "KTOS-TEST";
        enqueue_reply_frame(0x41, serial, 20);
        return 0;
    }
    if (op == 0x42) {
        uint8_t caps[48] = {0xf5, 0x12};
        memcpy(caps + 2, "KC908V", 6);
        if (device_mode) {
            memcpy(caps + 34, "KTOS", 4);
            caps[38] = 1;
            caps[40] = (uint8_t)(device_mode - 1);
            caps[41] = KTOS_CAP_KEEPALIVE;
        }
        enqueue_reply_frame(0x41, caps, device_mode ? 48 : 34);
        return 0;
    }

    if (!device_mode) {
        return 0;
    }
    if (op != KTOS_USB_COMMAND && op != KTOS_USB_KEEPALIVE) {
        return 6;
    }

    ktos_packet req;
    if (ktos_packet_decode(&req, p + 12, n) != 0) {
        return 6;
    }
    if (op == KTOS_USB_KEEPALIVE) {
        return (req.operation == KTOS_PING && !req.offset && !req.total && !req.status &&
                !req.bytes)
                   ? 0
                   : 6;
    }

    ktos_packet reply = {.operation = req.operation, .id = req.id};
    if (req.operation == KTOS_BEGIN) {
        tx_id    = req.id;
        total    = req.total;
        received = 0;
    } else if (req.operation == KTOS_WRITE) {
        if (tx_id != req.id || req.offset != received ||
            received + req.bytes > sizeof(request_buf) - 1) {
            return 6;
        }
        memcpy(request_buf + received, req.data, req.bytes);
        received += (uint32_t)req.bytes;
    } else if (req.operation == KTOS_EXECUTE) {
        request_buf[received] = '\0';
        if (received != total) {
            return 6;
        }
        if (memcmp(request_buf, "image ", 6) == 0) {
            char enabled[8];
            if (GetEnvironmentVariableA("KTOS_TEST_IMAGE", enabled, sizeof(enabled))) {
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
            return 6;
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
        return 6;
    }
    enqueue_reply_frame(op, encoded, encoded_size);
    return 0;
}
