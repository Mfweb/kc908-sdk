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

/* Vector Signal Analysis (VSA) enums and configuration mask. */
enum ktos_vsa_modulation {
    KTOS_VSA_MOD_BPSK      = 0,
    KTOS_VSA_MOD_QPSK      = 1,
    KTOS_VSA_MOD_OQPSK     = 2,
    KTOS_VSA_MOD_PI4_DQPSK = 3,
    KTOS_VSA_MOD_8PSK      = 4,
    KTOS_VSA_MOD_16QAM     = 5,
    KTOS_VSA_MOD_64QAM     = 6,
    KTOS_VSA_MOD_2FSK      = 7,
    KTOS_VSA_MOD_4FSK      = 8,
    KTOS_VSA_MOD_MSK       = 9,
    KTOS_VSA_MOD_GMSK      = 10
};

enum ktos_vsa_filter {
    KTOS_VSA_FILTER_RRC      = 0,
    KTOS_VSA_FILTER_RC       = 1,
    KTOS_VSA_FILTER_GAUSSIAN = 2,
    KTOS_VSA_FILTER_RECT     = 3
};

enum ktos_vsa_equalizer {
    KTOS_VSA_EQ_OFF      = 0,
    KTOS_VSA_EQ_ADAPTIVE = 1
};

enum ktos_vsa_view {
    KTOS_VSA_VIEW_SPECTRUM = 0,
    KTOS_VSA_VIEW_EYE      = 1,
    KTOS_VSA_VIEW_EVM      = 2
};

enum ktos_vsa_config_mask {
    KTOS_VSA_CFG_FREQUENCY     = 1u << 0,
    KTOS_VSA_CFG_MODULATION    = 1u << 1,
    KTOS_VSA_CFG_SYMBOL_RATE   = 1u << 2,
    KTOS_VSA_CFG_FILTER        = 1u << 3,
    KTOS_VSA_CFG_ALPHA         = 1u << 4,
    KTOS_VSA_CFG_EQUALIZER     = 1u << 5,
    KTOS_VSA_CFG_SYMBOLS       = 1u << 6,
    KTOS_VSA_CFG_AVERAGE       = 1u << 7,
    KTOS_VSA_CFG_FSK_DEVIATION = 1u << 8,
    KTOS_VSA_CFG_PORT          = 1u << 9,
    KTOS_VSA_CFG_ATT           = 1u << 10,
    KTOS_VSA_CFG_AMP           = 1u << 11,
    KTOS_VSA_CFG_IF_GAIN       = 1u << 12,
    KTOS_VSA_CFG_ALL           = (1u << 13) - 1u
};

typedef struct {
    uint32_t mask;
    uint32_t modulation;
    uint32_t filter;
    uint32_t equalizer;
    uint32_t symbols;
    uint32_t average;
    uint32_t port;
    uint32_t att_db;
    uint32_t amp_mode;
    uint32_t if_gain_db;
    double   frequency_hz;
    double   symbol_rate_hz;
    double   alpha;
    double   fsk_deviation_hz;
} ktos_vsa_config;

/* Sentinel for ktos_gps_sync_clock() to derive timezone from GPS coordinates. */
#define KTOS_GPS_TZ_AUTO INT32_MIN

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
/* Vector Signal Analysis (VSA) control and telemetry (KTOS MCP mode)         */
/* -------------------------------------------------------------------------- */

/* Switches the instrument into VSA mode (mode 6). */
KTOS_API int KTOS_CALL ktos_vsa_enter(ktos_device *device);

/* Applies every VSA setting enabled in `config->mask`. Stops on first error. */
KTOS_API int KTOS_CALL ktos_vsa_configure(ktos_device *device, const ktos_vsa_config *config);

/* Selects a built-in VSA modulation preset (0..6). */
KTOS_API int KTOS_CALL ktos_vsa_preset(ktos_device *device, uint32_t preset);

/* Starts continuous VSA acquisition, stops VSA, or triggers one single shot. */
KTOS_API int KTOS_CALL ktos_vsa_start(ktos_device *device);
KTOS_API int KTOS_CALL ktos_vsa_stop(ktos_device *device);
KTOS_API int KTOS_CALL ktos_vsa_single(ktos_device *device);

/* Clears constellation persistence or selects secondary plot view (0..2). */
KTOS_API int KTOS_CALL ktos_vsa_clear_persist(ktos_device *device);
KTOS_API int KTOS_CALL ktos_vsa_view(ktos_device *device, uint32_t view);

/* Reads live VSA configuration, lock flags, and vector demodulation metrics as
 * a NUL-terminated UTF-8 JSON object. */
KTOS_API int KTOS_CALL ktos_vsa_status(ktos_device *device, char *json, uint32_t capacity,
                                       uint32_t *needed);

/* Reads the latest VSA measurement frame as UTF-8 JSON.
 * `section` may be NULL/"all", "summary", "constellation", "spectrum", "evm", or "eye". */
KTOS_API int KTOS_CALL ktos_vsa_frame(ktos_device *device, const char *section, char *json,
                                      uint32_t capacity, uint32_t *needed);

/* -------------------------------------------------------------------------- */
/* Software GPS L1 control and telemetry (KTOS MCP mode)                      */
/* -------------------------------------------------------------------------- */

/* Enters software GPS L1 mode (1575.42 MHz). Set `open_page` non-zero to display
 * the full-screen GPS status view on the instrument. */
KTOS_API int KTOS_CALL ktos_gps_start(ktos_device *device, uint32_t open_page);

/* Leaves software GPS L1 mode and restores normal receiver demodulation. */
KTOS_API int KTOS_CALL ktos_gps_stop(ktos_device *device);

/* Reads live GPS fix, UTC time, tracked satellites, and track summary as UTF-8 JSON. */
KTOS_API int KTOS_CALL ktos_gps_status(ktos_device *device, char *json, uint32_t capacity,
                                       uint32_t *needed);

/* Reads paginated GPS track points (`limit` in 1..200; 0 uses default 64). */
KTOS_API int KTOS_CALL ktos_gps_track(ktos_device *device, uint32_t offset, uint32_t limit,
                                      char *json, uint32_t capacity, uint32_t *needed);

/* Applies current GPS fix to device location or syncs RTC + timezone
 * (`timezone_quarters` in -48..56, or KTOS_GPS_TZ_AUTO). */
KTOS_API int KTOS_CALL ktos_gps_sync_location(ktos_device *device);
KTOS_API int KTOS_CALL ktos_gps_sync_clock(ktos_device *device, int32_t timezone_quarters);

/* Exports the recorded GPS track as GPX under /mnt/sdcard/tracks/ or clears it. */
KTOS_API int KTOS_CALL ktos_gps_track_save(ktos_device *device);
KTOS_API int KTOS_CALL ktos_gps_track_reset(ktos_device *device);

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
