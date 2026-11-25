/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Binary SD image and KCApp transfer protocol definitions shared between the
 * KC908 host SDK and KTOS firmware (System >= 1.0.9).
 *
 * Wire layout (carried inside the sequenced, CRC-checked KTOS envelope):
 *   Request : operation, session, offset, count (four little-endian u32),
 *             followed by optional payload bytes.
 *   Response: magic, signed errno, session, size, offset, width, height,
 *             format, SHA-256[32], NUL-terminated device path[256] (320 bytes
 *             total), followed by optional payload bytes.
 */

#ifndef KTOS_IMAGE_PROTOCOL_H
#define KTOS_IMAGE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define KTOS_IMAGE_MAGIC       0x474d494bu /* "KIMG" little-endian */
#define KTOS_IMAGE_HEADER      16u
#define KTOS_IMAGE_INFO        320u
#define KTOS_IMAGE_MAX         (16u * 1024u * 1024u)
#define KTOS_APP_MAX           (4u * 1024u * 1024u)
#define KTOS_IMAGE_WRITE_BYTES 4000u
#define KTOS_IMAGE_READ_BYTES  61440u

enum ktos_image_operation {
    KTOS_IMAGE_UPLOAD = 1,
    KTOS_IMAGE_WRITE  = 2,
    KTOS_IMAGE_COMMIT = 3,
    KTOS_IMAGE_OPEN   = 4,
    KTOS_IMAGE_READ   = 5,
    KTOS_IMAGE_CLOSE  = 6,
    KTOS_IMAGE_REMOVE = 7,
    KTOS_APP_UPLOAD   = 8,
    KTOS_APP_REMOVE   = 9
};

enum ktos_image_format {
    KTOS_IMAGE_PNG   = 1,
    KTOS_IMAGE_JPEG  = 2,
    KTOS_IMAGE_BMP   = 3,
    KTOS_IMAGE_KCAPP = 4
};

static inline uint32_t ktos_image_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void ktos_image_put32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (uint8_t)(value >> (8u * i));
    }
}

#endif /* KTOS_IMAGE_PROTOCOL_H */
