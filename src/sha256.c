/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * FIPS 180-4 SHA-256 digest implementation used for SD image verification.
 */

#include "sha256.h"

#include <errno.h>
#include <string.h>

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

static uint32_t rotr32(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32u - bits));
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void sha256_transform(kc_sha256 *ctx, const uint8_t *block) {
    uint32_t w[16];
    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (unsigned i = 0; i < 16; i++) {
        w[i] = read_be32(block + 4u * i);
    }

    for (unsigned i = 0; i < 64; i++) {
        unsigned j = i & 15u;
        if (i >= 16) {
            uint32_t x = w[(i + 1u) & 15u];
            uint32_t y = w[(i + 14u) & 15u];
            w[j] += (rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3)) + w[(i + 9u) & 15u] +
                    (rotr32(y, 17) ^ rotr32(y, 19) ^ (y >> 10));
        }

        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + sha256_k[i] + w[j];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void kc_sha256_init(kc_sha256 *ctx) {
    if (!ctx) {
        return;
    }

    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->state, initial, sizeof(initial));
}

int kc_sha256_feed(kc_sha256 *ctx, const void *input, size_t bytes) {
    if (!ctx || (!input && bytes) || ctx->used >= 64 || ctx->bytes % 64 != ctx->used) {
        return -EINVAL;
    }
    if (ctx->bytes > UINT64_MAX / 8 || bytes > UINT64_MAX / 8 - ctx->bytes) {
        return -EOVERFLOW;
    }

    const uint8_t *p = input;
    ctx->bytes += bytes;

    if (ctx->used) {
        size_t take = 64 - ctx->used;
        if (take > bytes) {
            take = bytes;
        }
        if (take) {
            memcpy(ctx->block + ctx->used, p, take);
            p += take;
        }
        ctx->used += (unsigned)take;
        bytes -= take;
        if (ctx->used == 64) {
            sha256_transform(ctx, ctx->block);
            ctx->used = 0;
        }
    }

    while (bytes >= 64) {
        sha256_transform(ctx, p);
        p += 64;
        bytes -= 64;
    }

    if (bytes) {
        memcpy(ctx->block, p, bytes);
        ctx->used = (unsigned)bytes;
    }
    return 0;
}

int kc_sha256_digest(const kc_sha256 *ctx, uint8_t out[32]) {
    if (!ctx || !out || ctx->used >= 64 || ctx->bytes % 64 != ctx->used) {
        return -EINVAL;
    }
    if (ctx->bytes > UINT64_MAX / 8) {
        return -EOVERFLOW;
    }

    kc_sha256 copy = *ctx;
    unsigned  used = copy.used;
    copy.block[used++] = 0x80u;

    if (used > 56) {
        memset(copy.block + used, 0, 64 - used);
        sha256_transform(&copy, copy.block);
        used = 0;
    }
    memset(copy.block + used, 0, 56 - used);

    uint64_t bit_count = copy.bytes * 8u;
    for (unsigned i = 0; i < 8; i++) {
        copy.block[63 - i] = (uint8_t)(bit_count >> (8u * i));
    }
    sha256_transform(&copy, copy.block);

    for (unsigned i = 0; i < 8; i++) {
        for (unsigned j = 0; j < 4; j++) {
            out[4u * i + j] = (uint8_t)(copy.state[i] >> (24u - 8u * j));
        }
    }
    return 0;
}
