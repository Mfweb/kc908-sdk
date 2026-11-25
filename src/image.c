/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * High-level SD image upload and download helpers with end-to-end SHA-256
 * verification over ktos_image_request().
 */

#include "internal.h"
#include "image_protocol.h"
#include "sha256.h"

#include <stdlib.h>

#ifndef EBADMSG
#define EBADMSG 74
#endif

static uint8_t detect_image_format(const uint8_t *data, uint32_t bytes) {
    if (bytes >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
        return KTOS_IMAGE_PNG;
    }
    if (bytes >= 2 && data[0] == 0xff && data[1] == 0xd8) {
        return KTOS_IMAGE_JPEG;
    }
    if (bytes >= 2 && data[0] == 'B' && data[1] == 'M') {
        return KTOS_IMAGE_BMP;
    }
    return 0;
}

static int image_call(ktos_device *device, uint32_t op, uint32_t session_id, uint32_t offset,
                      uint32_t count, const void *data, size_t bytes, uint8_t *response,
                      uint32_t *actual) {
    if (bytes > KTOS_IMAGE_WRITE_BYTES) {
        return -EINVAL;
    }

    uint8_t request[KTOS_IMAGE_HEADER + KTOS_IMAGE_WRITE_BYTES];
    ktos_image_put32(request + 0, op);
    ktos_image_put32(request + 4, session_id);
    ktos_image_put32(request + 8, offset);
    ktos_image_put32(request + 12, count);
    if (bytes) {
        memcpy(request + KTOS_IMAGE_HEADER, data, bytes);
    }

    int error = ktos_image_request(device, request, (uint32_t)(KTOS_IMAGE_HEADER + bytes), response,
                                   KTOS_RESPONSE_BYTES + 1, actual);
    if (!error && (*actual < KTOS_IMAGE_INFO || ktos_image_u32(response) != KTOS_IMAGE_MAGIC)) {
        error = -EPROTO;
    }
    if (!error) {
        error = (int32_t)ktos_image_u32(response + 4);
    }
    return error;
}

static int parse_image_metadata(const uint8_t *raw, ktos_image_info *info) {
    info->bytes  = ktos_image_u32(raw + 12);
    info->width  = ktos_image_u32(raw + 20);
    info->height = ktos_image_u32(raw + 24);
    info->format = ktos_image_u32(raw + 28);
    memcpy(info->sha256, raw + 32, 32);
    memcpy(info->device_path, raw + 64, 256);

    if (info->bytes > KTOS_IMAGE_MAX || !memchr(info->device_path, '\0', 256)) {
        return -EPROTO;
    }
    return 0;
}

int KTOS_CALL ktos_image_upload(ktos_device *device, const void *data, uint32_t bytes,
                                ktos_image_info *info) {
    if (!device || !data || bytes < 8 || bytes > KTOS_IMAGE_MAX || !info) {
        return -EINVAL;
    }

    const uint8_t *payload = data;
    uint8_t        meta[33];
    meta[0] = detect_image_format(payload, bytes);
    if (!meta[0]) {
        return -EINVAL;
    }

    kc_sha256 hash;
    kc_sha256_init(&hash);
    kc_sha256_feed(&hash, payload, bytes);
    kc_sha256_digest(&hash, meta + 1);

    uint8_t *response = malloc(KTOS_RESPONSE_BYTES + 1);
    if (!response) {
        return -ENOMEM;
    }

    uint32_t actual     = 0;
    uint32_t session_id = 0;

    int error = image_call(device, KTOS_IMAGE_UPLOAD, 0, 0, bytes, meta, sizeof(meta), response,
                           &actual);
    if (!error) {
        session_id = ktos_image_u32(response + 8);
        if (!session_id) {
            error = -EPROTO;
        }
    }

    for (uint32_t offset = 0; !error && offset < bytes;) {
        uint32_t chunk = bytes - offset;
        if (chunk > KTOS_IMAGE_WRITE_BYTES) {
            chunk = KTOS_IMAGE_WRITE_BYTES;
        }
        error = image_call(device, KTOS_IMAGE_WRITE, session_id, offset, chunk, payload + offset,
                           chunk, response, &actual);
        offset += chunk;
        if (!error &&
            (ktos_image_u32(response + 8) != session_id || ktos_image_u32(response + 16) != offset)) {
            error = -EPROTO;
        }
    }

    if (!error) {
        error = image_call(device, KTOS_IMAGE_COMMIT, session_id, 0, 0, NULL, 0, response, &actual);
    }
    if (!error) {
        error = parse_image_metadata(response, info);
    }
    if (!error && (info->bytes != bytes || memcmp(info->sha256, meta + 1, 32) != 0)) {
        error = -EBADMSG;
    }

    if (session_id) {
        (void)image_call(device, KTOS_IMAGE_CLOSE, session_id, 0, 0, NULL, 0, response, &actual);
    }
    free(response);

    return error ? sdk_failure(device, error, "image upload") : 0;
}

int KTOS_CALL ktos_image_download(ktos_device *device, const char *device_path, void *data,
                                  uint32_t capacity, ktos_image_info *info) {
    if (!device || !device_path || !info || (capacity && !data) || strlen(device_path) > 255) {
        return -EINVAL;
    }

    uint8_t *response = malloc(KTOS_RESPONSE_BYTES + 1);
    if (!response) {
        return -ENOMEM;
    }

    uint32_t actual     = 0;
    uint32_t session_id = 0;

    int error = image_call(device, KTOS_IMAGE_OPEN, 0, 0, 0, device_path, strlen(device_path) + 1,
                           response, &actual);
    if (!error) {
        session_id = ktos_image_u32(response + 8);
        error      = parse_image_metadata(response, info);
        if (!error && (!session_id || !info->bytes)) {
            error = -EPROTO;
        }
        if (!error && info->bytes > capacity) {
            error = -ENOSPC;
        }
    }

    kc_sha256 hash;
    kc_sha256_init(&hash);

    for (uint32_t offset = 0; !error && offset < info->bytes;) {
        error = image_call(device, KTOS_IMAGE_READ, session_id, offset, KTOS_IMAGE_READ_BYTES, NULL,
                           0, response, &actual);
        uint32_t chunk = (actual >= KTOS_IMAGE_INFO) ? (actual - KTOS_IMAGE_INFO) : 0;
        if (!error &&
            (!chunk || chunk > info->bytes - offset ||
             ktos_image_u32(response + 8) != session_id ||
             ktos_image_u32(response + 12) != info->bytes ||
             ktos_image_u32(response + 16) != offset + chunk ||
             memcmp(response + 32, info->sha256, 32) != 0)) {
            error = -EPROTO;
        }
        if (!error) {
            memcpy((uint8_t *)data + offset, response + KTOS_IMAGE_INFO, chunk);
            kc_sha256_feed(&hash, response + KTOS_IMAGE_INFO, chunk);
            offset += chunk;
        }
    }

    uint8_t digest[32];
    kc_sha256_digest(&hash, digest);
    if (!error && memcmp(digest, info->sha256, 32) != 0) {
        error = -EBADMSG;
    }

    if (session_id) {
        (void)image_call(device, KTOS_IMAGE_CLOSE, session_id, 0, 0, NULL, 0, response, &actual);
    }
    free(response);

    return error ? sdk_failure(device, error, "image download") : 0;
}
