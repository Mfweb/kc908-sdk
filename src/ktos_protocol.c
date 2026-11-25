/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Portable KTOS packet encoder, decoder, and IEEE 802.3 CRC-32.
 *
 * Header layout (32 bytes, little-endian):
 *   0..3   : magic "KTOS" (0x534f544b)
 *   4      : protocol version (1)
 *   5      : reserved (0)
 *   6..7   : operation (enum ktos_operation)
 *   8..11  : non-zero transaction id
 *   12..15 : fragment byte offset
 *   16..19 : total payload length
 *   20..23 : signed device status (errno)
 *   24..27 : payload byte count following the header
 *   28..31 : CRC-32 over header[0..27] and payload[0..bytes-1]
 */

#include "ktos_protocol.h"

#include <errno.h>
#include <string.h>

#ifndef EBADMSG
#define EBADMSG 74
#endif

#define KTOS_PACKET_MAGIC 0x534f544bu /* "KTOS" little-endian */
#define KTOS_CRC32_POLY   0xedb88320u

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (uint8_t)(value >> (8u * i));
    }
}

uint32_t ktos_crc32(const void *data, size_t count, uint32_t initial) {
    const uint8_t *bytes = data;
    uint32_t       crc   = ~initial;

    for (size_t i = 0; i < count; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (KTOS_CRC32_POLY & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

int ktos_packet_decode(ktos_packet *out, const void *data, size_t count) {
    if (!out || !data || count < KTOS_HEADER_BYTES) {
        return -EINVAL;
    }

    const uint8_t *raw = data;
    if (read_le32(raw) != KTOS_PACKET_MAGIC || raw[4] != KTOS_PROTOCOL_VERSION || raw[5] != 0) {
        return -EPROTONOSUPPORT;
    }

    unsigned operation = raw[6] | ((unsigned)raw[7] << 8);
    uint32_t id        = read_le32(raw + 8);
    size_t   payload   = read_le32(raw + 24);

    if (operation < KTOS_PING || operation > KTOS_IQ || payload > KTOS_RESPONSE_BYTES ||
        payload != count - KTOS_HEADER_BYTES || id == 0) {
        return -EPROTO;
    }

    uint32_t crc = ktos_crc32(raw, 28, 0);
    crc          = ktos_crc32(raw + KTOS_HEADER_BYTES, payload, crc);
    if (crc != read_le32(raw + 28)) {
        return -EBADMSG;
    }

    *out = (ktos_packet){
        .operation = (uint16_t)operation,
        .id        = id,
        .offset    = read_le32(raw + 12),
        .total     = read_le32(raw + 16),
        .status    = (int32_t)read_le32(raw + 20),
        .data      = raw + KTOS_HEADER_BYTES,
        .bytes     = payload,
    };
    return 0;
}

int ktos_packet_encode(void *data, size_t capacity, const ktos_packet *packet, size_t *written) {
    if (!data || !packet || !written || (!packet->data && packet->bytes) || packet->id == 0 ||
        packet->operation < KTOS_PING || packet->operation > KTOS_IQ) {
        return -EINVAL;
    }
    if (capacity < KTOS_HEADER_BYTES || packet->bytes > capacity - KTOS_HEADER_BYTES ||
        packet->bytes > KTOS_RESPONSE_BYTES) {
        return -EMSGSIZE;
    }

    uint8_t *out = data;
    if (packet->bytes) {
        memmove(out + KTOS_HEADER_BYTES, packet->data, packet->bytes);
    }

    write_le32(out + 0, KTOS_PACKET_MAGIC);
    write_le32(out + 4, KTOS_PROTOCOL_VERSION | ((uint32_t)packet->operation << 16));
    write_le32(out + 8, packet->id);
    write_le32(out + 12, packet->offset);
    write_le32(out + 16, packet->total);
    write_le32(out + 20, (uint32_t)packet->status);
    write_le32(out + 24, (uint32_t)packet->bytes);

    uint32_t crc = ktos_crc32(out, 28, 0);
    write_le32(out + 28, ktos_crc32(out + KTOS_HEADER_BYTES, packet->bytes, crc));

    *written = KTOS_HEADER_BYTES + packet->bytes;
    return 0;
}
