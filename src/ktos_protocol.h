/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Portable KTOS control-packet framing and CRC-32 codec.
 */

#ifndef KTOS_PROTOCOL_H
#define KTOS_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define KTOS_PROTOCOL_VERSION     1u
#define KTOS_USB_COMMAND          0xd0u
#define KTOS_USB_KEEPALIVE        0xd1u
#define KTOS_CAP_KEEPALIVE        1u
#define KTOS_HEADER_BYTES         32u
#define KTOS_REQUEST_BYTES        4096u
#define KTOS_RESPONSE_BYTES       65536u
#define KTOS_FRAGMENT_BYTES       148u
#define KTOS_REPLY_FRAGMENT_BYTES 4044u

enum ktos_operation {
    KTOS_PING    = 1,
    KTOS_BEGIN   = 2,
    KTOS_WRITE   = 3,
    KTOS_EXECUTE = 4,
    KTOS_READ    = 5,
    KTOS_CANCEL  = 6,
    KTOS_IQ      = 7
};

typedef struct {
    uint16_t       operation;
    uint32_t       id;
    uint32_t       offset;
    uint32_t       total;
    int32_t        status;
    const uint8_t *data;
    size_t         bytes;
} ktos_packet;

uint32_t ktos_crc32(const void *data, size_t count, uint32_t initial);
int      ktos_packet_decode(ktos_packet *out, const void *data, size_t count);
int      ktos_packet_encode(void *data, size_t capacity, const ktos_packet *packet,
                            size_t *written);

#endif /* KTOS_PROTOCOL_H */
