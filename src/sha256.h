/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Self-contained incremental SHA-256 implementation (no heap allocations).
 */

#ifndef KTOS_SDK_SHA256_H
#define KTOS_SDK_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bytes;
    uint8_t  block[64];
    unsigned used;
} kc_sha256;

void kc_sha256_init(kc_sha256 *ctx);
int  kc_sha256_feed(kc_sha256 *ctx, const void *input, size_t bytes);

/* Computes the digest without mutating `ctx` and without allocating memory. */
int kc_sha256_digest(const kc_sha256 *ctx, uint8_t out[32]);

#endif /* KTOS_SDK_SHA256_H */
