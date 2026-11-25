/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Public C ABI for the KTOS KC908 host SDK (kcsdr.dll).
 *
 * Conventions:
 *   - All functions return 0 on success or a negative error code on failure.
 *   - FTDI D3XX errors are encoded as (-1000 - FT_STATUS); protocol and device
 *     errors follow negative Linux errno values.
 *   - Device handles are exclusive. Callers must stop concurrent requests
 *     before calling ktos_close().
 */

#ifndef KTOS_SDK_H
#define KTOS_SDK_H

#include <stdint.h>

#ifdef _WIN32
#define KTOS_API  __declspec(dllexport)
#define KTOS_CALL __cdecl
#else
#define KTOS_API  __attribute__((visibility("default")))
#define KTOS_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Types and constants                                                        */
/* -------------------------------------------------------------------------- */

typedef struct ktos_device ktos_device;

enum {
    KTOS_BACKEND_ORIGINAL = 0,
    KTOS_BACKEND_CUSTOM   = 1
};

enum {
    KTOS_MODE_IQ  = 0,
    KTOS_MODE_MCP = 1
};

typedef struct {
    uint32_t bytes;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint8_t  sha256[32];
    char     device_path[256];
} ktos_image_info;

/* -------------------------------------------------------------------------- */
/* Lifecycle and diagnostics                                                  */
/* -------------------------------------------------------------------------- */

/* Returns the packed SDK version: (major << 16) | (minor << 8) | patch. */
KTOS_API uint32_t KTOS_CALL ktos_sdk_version(void);

/* Opens FT601 device at `index`, negotiates protocol capabilities, and starts
 * the keepalive thread when connected to the KTOS custom backend. */
KTOS_API int KTOS_CALL ktos_open(uint32_t index, ktos_device **out);

/* Stops active streaming or remote output and releases the device handle. */
KTOS_API void KTOS_CALL ktos_close(ktos_device *device);

/* Writes a NUL-terminated UTF-8 JSON object describing model, serial, backend,
 * active mode, and SDK version. Returns -ENOSPC if `capacity` is too small. */
KTOS_API int KTOS_CALL ktos_identity(ktos_device *device, char *json, uint32_t capacity);

/* Reads the last error code and human-readable message for `device`.
 * Pass NULL for `device` to retrieve the calling thread's last ktos_open() error. */
KTOS_API int KTOS_CALL ktos_error(ktos_device *device, char *text, uint32_t capacity);

/* -------------------------------------------------------------------------- */
/* Control RPC (KTOS MCP mode)                                                */
/* -------------------------------------------------------------------------- */

/* Sends an instrument RPC command (omitting the device's "agent " prefix) and
 * receives a NUL-terminated UTF-8 JSON response.
 *
 * `*needed` receives the required buffer size including the trailing NUL.
 * Returning -ENOSPC never retries a potentially mutating command. */
KTOS_API int KTOS_CALL ktos_rpc(ktos_device *device, const char *command, char *json,
                                uint32_t capacity, uint32_t *needed);

/* -------------------------------------------------------------------------- */
/* Binary SD image transfer (KTOS System >= 1.0.9)                            */
/* -------------------------------------------------------------------------- */

/* Low-level binary transfer packet interface (see image_protocol.h).
 *
 * `capacity` must include one reserved trailing byte; `*actual` excludes it.
 * The binary response carries the device errno at byte offset 4. No implicit
 * retries are performed.
 *
 * Packet summary:
 *   UPLOAD : format byte + SHA-256[32], count = total file size
 *   WRITE  : <= 4000 payload bytes per chunk
 *   COMMIT : validates, decode-checks, and publishes a new SD tmp image
 *   OPEN   : NUL-terminated SD path, or "@sstv" for the current decoded snapshot
 *   READ   : <= 61440 payload bytes per chunk
 *   CLOSE  : releases the transfer session (and aborts partial uploads)
 *   REMOVE : deletes only files created inside the KTOS temporary image cache */
KTOS_API int KTOS_CALL ktos_image_request(ktos_device *device, const void *request, uint32_t bytes,
                                          void *response, uint32_t capacity, uint32_t *actual);

/* High-level convenience wrappers that stream bounded chunks from/to caller
 * memory, verify the full-file SHA-256 digest, and always close the session.
 *
 * - Upload never starts RF transmission.
 * - Download accepts "@sstv"; if `capacity` is too small, returns -ENOSPC and
 *   still populates `info` with the required byte count. Allocate up to 16 MiB
 *   for single-call downloads.
 * - Do not close `device` while these calls are executing. For progress
 *   reporting or cancellation, use ktos_image_request() directly. */
KTOS_API int KTOS_CALL ktos_image_upload(ktos_device *device, const void *data, uint32_t bytes,
                                         ktos_image_info *info);

KTOS_API int KTOS_CALL ktos_image_download(ktos_device *device, const char *device_path, void *data,
                                           uint32_t capacity, ktos_image_info *info);

/* -------------------------------------------------------------------------- */
/* IQ streaming (original USB3 or KTOS IQ mode)                               */
/* -------------------------------------------------------------------------- */

KTOS_API int KTOS_CALL ktos_iq_command(ktos_device *device, uint32_t opcode, const void *data,
                                       uint32_t bytes);

KTOS_API int KTOS_CALL ktos_iq_read(ktos_device *device, void *data, uint32_t bytes,
                                    uint32_t *actual);

KTOS_API int KTOS_CALL ktos_iq_write(ktos_device *device, const void *data, uint32_t bytes,
                                     uint32_t *actual);

KTOS_API int KTOS_CALL ktos_iq_stop(ktos_device *device);

/* -------------------------------------------------------------------------- */
/* Legacy Windows vendor 1.3.4 compatibility                                  */
/* -------------------------------------------------------------------------- */

/* Returns the static 26-entry function pointer table matching the original
 * Windows kcsdr.dll 1.3.4 ABI. */
KTOS_API void *KTOS_CALL kcsdr_init(void);

#ifdef __cplusplus
}
#endif

#endif /* KTOS_SDK_H */
