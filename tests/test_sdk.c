/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Offline unit tests for kcsdr.dll / libkcsdr.so against the fake FT601 emulator:
 *   - open failure reporting via thread-local storage
 *   - original vs. KTOS capability negotiation and keepalive
 *   - queued IQ RX streaming, restart drain, and stalled-pipe errors
 *   - fragmented RPC, binary image transfer, SHA-256 verification, and recovery
 *   - legacy vendor 1.3.4 function table ABI and struct offsets
 */

#include "ktos_sdk.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

static void set_test_env(const char *name, const char *value) {
    SetEnvironmentVariableA(name, value);
}

static void sleep_ms(unsigned ms) {
    Sleep(ms);
}
#else
#include <errno.h>
#include <time.h>

static void set_test_env(const char *name, const char *value) {
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
}

static void sleep_ms(unsigned ms) {
    struct timespec req = {
        .tv_sec  = (time_t)(ms / 1000u),
        .tv_nsec = (long)(ms % 1000u) * 1000000L,
    };
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
}
#endif

int main(void) {
    assert(ktos_sdk_version() == 0x00010005);

    /* Open failure diagnostics via thread-local storage. */
    for (unsigned kind = 1; kind <= 2; kind++) {
        char value[2] = {(char)('0' + kind), '\0'};
        set_test_env("KTOS_TEST_FAILURE", value);
        ktos_device *device = (ktos_device *)1;
        char         error[512];
        int          result = ktos_open(0, &device);
        assert(result < 0 && !device);
        assert(ktos_error(NULL, error, sizeof(error)) == result && error[0]);
    }
    set_test_env("KTOS_TEST_FAILURE", NULL);

    /* Exercise original firmware (0), KTOS IQ (1), and KTOS MCP (2) modes. */
    for (unsigned mode = 0; mode < 3; mode++) {
        char mode_str[2] = {(char)('0' + mode), '\0'};
        set_test_env("KTOS_TEST_MODE", mode_str);

        ktos_device *device = NULL;
        assert(ktos_open(0, &device) == 0 && device);

        char info[512];
        assert(ktos_identity(device, info, sizeof(info)) == 0);
        assert(strstr(info, "\"sdk\":\"1.0.5\""));
        assert(strstr(info, mode ? "\"backend\":\"KTOS\"" : "\"backend\":\"original\""));

        if (mode > 0) {
            sleep_ms(300);
            assert(ktos_error(device, info, sizeof(info)) == 0);
        }

        if (mode < 2) {
            /* Queued IQ RX across 5 MiB of monotonic 32-bit sequence words. */
            assert(ktos_iq_command(device, 6, NULL, 0) == 0);
            uint32_t *buffer = malloc(32768);
            uint32_t  actual = 0;
            assert(buffer);

            uint32_t last = 0;
            for (unsigned i = 0; i < 160; i++) {
                assert(ktos_iq_read(device, buffer, 32768, &actual) == 0 && actual == 32768);
                if (i > 0) {
                    assert(buffer[0] == last + 1);
                }
                for (unsigned j = 1; j < 8192; j++) {
                    assert(buffer[j] == buffer[j - 1] + 1);
                }
                last = buffer[8191];
            }
            assert(ktos_iq_write(device, buffer, 32768, &actual) == 0 && actual == 32768);
            assert(ktos_iq_stop(device) == 0);

            /* Repeated start/stop cycles must cleanly drain empty completions. */
            for (unsigned restart = 0; restart < 3; restart++) {
                assert(ktos_iq_command(device, 6, NULL, 0) == 0);
                assert(ktos_iq_read(device, buffer, 32768, &actual) == 0 && actual == 32768);
                for (unsigned j = 1; j < 8192; j++) {
                    assert(buffer[j] == buffer[j - 1] + 1);
                }
                assert(ktos_iq_stop(device) == 0);
            }

            /* Persistent zero-length transfers must fail with a protocol error. */
            set_test_env("KTOS_TEST_FAILURE", "3");
            assert(ktos_iq_command(device, 6, NULL, 0) == 0);
            assert(ktos_iq_read(device, buffer, 32768, &actual) < 0 && actual == 0);
            char error[256];
            assert(ktos_error(device, error, sizeof(error)) < 0 && strstr(error, "length 0"));
            set_test_env("KTOS_TEST_FAILURE", NULL);
            free(buffer);
        } else {
            /* Fragmented JSON RPC, input validation, and capacity reporting. */
            char    *json   = malloc(65537);
            uint32_t needed = 0;
            assert(json);

            assert(ktos_rpc(device, "fft", json, 65537, &needed) == 0 && needed == 16001);
            assert(strncmp(json, "{\"ok\":true", 10) == 0 && json[needed - 1] == '\0');
            assert(ktos_rpc(device, "fft", json, 16, &needed) < 0 && needed == 16001);
            assert(ktos_rpc(device, "action run\naction stop", json, 65537, &needed) < 0);
            assert(ktos_rpc(device, "capabilities", json, 65537, &needed) == 0);

            /* VSA C convenience API coverage. */
            assert(ktos_vsa_enter(device) == 0);
            ktos_vsa_config vsa_cfg = {
                .mask           = KTOS_VSA_CFG_FREQUENCY | KTOS_VSA_CFG_MODULATION |
                                  KTOS_VSA_CFG_SYMBOL_RATE | KTOS_VSA_CFG_FILTER |
                                  KTOS_VSA_CFG_ALPHA | KTOS_VSA_CFG_SYMBOLS | KTOS_VSA_CFG_PORT,
                .frequency_hz   = 433920000.0,
                .modulation     = KTOS_VSA_MOD_16QAM,
                .symbol_rate_hz = 100000.0,
                .filter         = KTOS_VSA_FILTER_RRC,
                .alpha          = 0.35,
                .symbols        = 512,
                .port           = 2,
            };
            assert(ktos_vsa_configure(device, &vsa_cfg) == 0);
            ktos_vsa_config bad_cfg = {.mask = 0};
            assert(ktos_vsa_configure(device, &bad_cfg) < 0);
            assert(ktos_vsa_preset(device, 1) == 0);
            assert(ktos_vsa_preset(device, 99) < 0);
            assert(ktos_vsa_view(device, KTOS_VSA_VIEW_EVM) == 0);
            assert(ktos_vsa_view(device, 9) < 0);
            assert(ktos_vsa_clear_persist(device) == 0);
            assert(ktos_vsa_single(device) == 0);
            assert(ktos_vsa_start(device) == 0);
            assert(ktos_vsa_status(device, json, 65537, &needed) == 0);
            assert(strstr(json, "\"modulation_name\":\"16QAM\""));
            assert(ktos_vsa_frame(device, "constellation", json, 65537, &needed) == 0);
            assert(strstr(json, "\"points\":[[0.31,0.31]]"));
            assert(ktos_vsa_frame(device, "invalid", json, 65537, &needed) < 0);
            assert(ktos_vsa_stop(device) == 0);

            /* GPS L1 C convenience API coverage. */
            assert(ktos_gps_start(device, 1) == 0);
            assert(ktos_gps_start(device, 2) < 0);
            assert(ktos_gps_status(device, json, 65537, &needed) == 0);
            assert(strstr(json, "\"mode\":\"3d\"") && strstr(json, "\"satellites_tracked\":6"));
            assert(ktos_gps_track(device, 0, 0, json, 65537, &needed) == 0);
            assert(strstr(json, "\"points\":[{\"lat\":36.6512000"));
            assert(ktos_gps_track(device, 0, 201, json, 65537, &needed) < 0);
            assert(ktos_gps_sync_location(device) == -11); /* Device EAGAIN propagated */
            assert(ktos_gps_sync_clock(device, KTOS_GPS_TZ_AUTO) == 0);
            assert(ktos_gps_sync_clock(device, 32) == 0);
            assert(ktos_gps_sync_clock(device, 99) < 0);
            assert(ktos_gps_track_save(device) == 0);
            assert(ktos_gps_track_reset(device) == 0);
            assert(ktos_gps_stop(device) == 0);

            /* DRM30 / DRM+ C convenience API coverage. */
            assert(ktos_drm_start(device, 1) == 0);
            assert(ktos_drm_start(device, 2) < 0);
            ktos_drm_config drm_cfg = {
                .mask          = KTOS_DRM_CFG_FREQUENCY | KTOS_DRM_CFG_PROFILE |
                                 KTOS_DRM_CFG_SERVICE | KTOS_DRM_CFG_STEREO |
                                 KTOS_DRM_CFG_IQ_INVERT,
                .profile       = KTOS_DRM_PROFILE_DRM30_10K,
                .service_index = 0,
                .stereo        = 1,
                .iq_invert     = KTOS_DRM_IQ_NORMAL,
                .frequency_hz  = 9650000.0,
            };
            assert(ktos_drm_configure(device, &drm_cfg) == 0);
            ktos_drm_config bad_drm_cfg = {.mask = 0};
            assert(ktos_drm_configure(device, &bad_drm_cfg) < 0);
            ktos_drm_config bad_drm_prof = {.mask = KTOS_DRM_CFG_PROFILE, .profile = 9};
            assert(ktos_drm_configure(device, &bad_drm_prof) < 0);
            assert(ktos_drm_select_service(device, 2) == 0);
            assert(ktos_drm_select_service(device, 4) < 0);
            assert(ktos_drm_set_profile(device, KTOS_DRM_PROFILE_DRMP_100K) == 0);
            assert(ktos_drm_set_profile(device, 4) < 0);
            assert(ktos_drm_set_stereo(device, 0) == 0);
            assert(ktos_drm_set_stereo(device, 2) < 0);
            assert(ktos_drm_set_iq_invert(device, KTOS_DRM_IQ_INVERT) == 0);
            assert(ktos_drm_set_iq_invert(device, 3) < 0);
            assert(ktos_drm_reset(device) == 0);
            assert(ktos_drm_status(device, json, 65537, &needed) == 0);
            assert(strstr(json, "\"state\":\"audio\"") && strstr(json, "\"label\":\"CNR-1\"") &&
                   strstr(json, "\"text\":\"DRM Live\""));
            assert(ktos_drm_stop(device) == 0);

            /* Raw binary image transport bounds and echo check. */
            uint8_t binary[4016];
            for (unsigned i = 0; i < sizeof(binary); i++) {
                binary[i] = (uint8_t)i;
            }
            assert(ktos_image_request(device, binary, sizeof(binary), json, 65537, &needed) == 0);
            assert(needed == sizeof(binary) && memcmp(binary, json, sizeof(binary)) == 0);
            assert(ktos_image_request(device, binary, 4091, json, 65537, &needed) < 0);
            assert(ktos_image_request(device, binary, UINT32_MAX, json, 65537, &needed) < 0);

            /* High-level image upload/download with SHA-256 verification. */
            set_test_env("KTOS_TEST_IMAGE", "1");
            uint8_t *image    = malloc(100000);
            uint8_t *download = malloc(100000);
            assert(image && download);
            for (unsigned i = 0; i < 100000; i++) {
                image[i] = (uint8_t)i;
            }
            memcpy(image, "\x89PNG\r\n\x1a\n", 8);

            ktos_image_info uploaded, downloaded;
            assert(ktos_image_upload(device, image, 100000, &uploaded) == 0);
            assert(uploaded.bytes == 100000 && uploaded.width == 100);
            assert(ktos_image_download(device, uploaded.device_path, download, 1, &downloaded) < 0);
            assert(downloaded.bytes == 100000);
            assert(ktos_image_download(device, uploaded.device_path, download, 100000,
                                       &downloaded) == 0);
            assert(memcmp(image, download, 100000) == 0);

            set_test_env("KTOS_TEST_IMAGE_CORRUPT", "1");
            assert(ktos_image_download(device, uploaded.device_path, download, 100000,
                                       &downloaded) < 0);
            set_test_env("KTOS_TEST_IMAGE_CORRUPT", NULL);
            set_test_env("KTOS_TEST_IMAGE", NULL);
            free(image);
            free(download);

            /* Failed submit, OUT write, or IN completion must retire the queued
             * read cleanly before the next command or ktos_close(). */
            for (unsigned failure = 4; failure <= 6; failure++) {
                char value[2] = {(char)('0' + failure), '\0'};
                set_test_env("KTOS_TEST_FAILURE", value);
                assert(ktos_rpc(device, "capabilities", json, 65537, &needed) < 0);
                set_test_env("KTOS_TEST_FAILURE", NULL);
                assert(ktos_rpc(device, "capabilities", json, 65537, &needed) == 0);
            }
            free(json);
        }

        ktos_close(device);
    }

    /* Verify legacy vendor 1.3.4 function table and struct offsets. */
    set_test_env("KTOS_TEST_MODE", "0");
    void **api = kcsdr_init();
    typedef ktos_device *(*find_fn)(int);
    typedef uint32_t (*bandwidth_fn)(ktos_device *, uint16_t);
    typedef void (*close_fn)(ktos_device *);

    ktos_device *legacy = ((find_fn)api[0])(0);
    assert(legacy);
    assert(*(int32_t *)((char *)legacy + 100) == 0 && *(uint16_t *)((char *)legacy + 104) == 4096);
    assert(((bandwidth_fn)api[25])(legacy, 3) == 5000000);
    ((close_fn)api[2])(legacy);

    puts("SDK original/custom negotiation, legacy ABI, 5 MiB queued IQ, fragmented RPC, bounds "
         "PASS");
    return 0;
}
