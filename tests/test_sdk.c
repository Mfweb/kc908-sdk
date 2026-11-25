/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Offline unit tests for kcsdr.dll against the fake FT601 emulator:
 *   - open failure reporting via FLS
 *   - original vs. KTOS capability negotiation and keepalive
 *   - queued IQ RX streaming, restart drain, and stalled-pipe errors
 *   - fragmented RPC, binary image transfer, SHA-256 verification, and recovery
 *   - legacy Windows vendor 1.3.4 function table ABI
 */

#include "ktos_sdk.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int main(void) {
    assert(ktos_sdk_version() == 0x00010003);

    /* Open failure diagnostics via thread-local FLS storage. */
    for (unsigned kind = 1; kind <= 2; kind++) {
        char value[2] = {(char)('0' + kind), '\0'};
        SetEnvironmentVariableA("KTOS_TEST_FAILURE", value);
        ktos_device *device = (ktos_device *)1;
        char         error[512];
        int          result = ktos_open(0, &device);
        assert(result < 0 && !device);
        assert(ktos_error(NULL, error, sizeof(error)) == result && error[0]);
    }
    SetEnvironmentVariableA("KTOS_TEST_FAILURE", NULL);

    /* Exercise original firmware (0), KTOS IQ (1), and KTOS MCP (2) modes. */
    for (unsigned mode = 0; mode < 3; mode++) {
        char mode_str[2] = {(char)('0' + mode), '\0'};
        SetEnvironmentVariableA("KTOS_TEST_MODE", mode_str);

        ktos_device *device = NULL;
        assert(ktos_open(0, &device) == 0 && device);

        char info[512];
        assert(ktos_identity(device, info, sizeof(info)) == 0);
        assert(strstr(info, "\"sdk\":\"1.0.3\""));
        assert(strstr(info, mode ? "\"backend\":\"KTOS\"" : "\"backend\":\"original\""));

        if (mode > 0) {
            Sleep(300);
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
            SetEnvironmentVariableA("KTOS_TEST_FAILURE", "3");
            assert(ktos_iq_command(device, 6, NULL, 0) == 0);
            assert(ktos_iq_read(device, buffer, 32768, &actual) < 0 && actual == 0);
            char error[256];
            assert(ktos_error(device, error, sizeof(error)) < 0 && strstr(error, "length 0"));
            SetEnvironmentVariableA("KTOS_TEST_FAILURE", NULL);
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
            SetEnvironmentVariableA("KTOS_TEST_IMAGE", "1");
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

            SetEnvironmentVariableA("KTOS_TEST_IMAGE_CORRUPT", "1");
            assert(ktos_image_download(device, uploaded.device_path, download, 100000,
                                       &downloaded) < 0);
            SetEnvironmentVariableA("KTOS_TEST_IMAGE_CORRUPT", NULL);
            SetEnvironmentVariableA("KTOS_TEST_IMAGE", NULL);
            free(image);
            free(download);

            /* Failed submit, OUT write, or IN completion must retire the queued
             * read cleanly before the next command or ktos_close(). */
            for (unsigned failure = 4; failure <= 6; failure++) {
                char value[2] = {(char)('0' + failure), '\0'};
                SetEnvironmentVariableA("KTOS_TEST_FAILURE", value);
                assert(ktos_rpc(device, "capabilities", json, 65537, &needed) < 0);
                SetEnvironmentVariableA("KTOS_TEST_FAILURE", NULL);
                assert(ktos_rpc(device, "capabilities", json, 65537, &needed) == 0);
            }
            free(json);
        }

        ktos_close(device);
    }

    /* Verify legacy Windows vendor 1.3.4 function table and struct offsets. */
    SetEnvironmentVariableA("KTOS_TEST_MODE", "0");
    void **api = kcsdr_init();
    typedef ktos_device *(*find_fn)(int);
    typedef uint32_t (*bandwidth_fn)(ktos_device *, uint16_t);
    typedef void (*close_fn)(ktos_device *);

    ktos_device *legacy = ((find_fn)api[0])(0);
    assert(legacy);
    assert(*(int32_t *)((char *)legacy + 100) == 0 && *(uint16_t *)((char *)legacy + 104) == 4096);
    assert(((bandwidth_fn)api[25])(legacy, 3) == 5000000);
    ((close_fn)api[2])(legacy);

    puts("SDK original/custom negotiation, Windows ABI, 5 MiB queued IQ, fragmented RPC, bounds "
         "PASS");
    return 0;
}
