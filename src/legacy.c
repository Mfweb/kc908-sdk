/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Legacy Windows vendor kcsdr.dll 1.3.4 26-slot function-table adapter.
 *
 * Note: Slot order matches the Windows 1.3.4 DLL ABI. Other platforms (such as
 * Linux GNU Radio headers) use a different table layout and are not compatible.
 */

#include "internal.h"

#include <stdbool.h>

static ktos_device *legacy_find_device(int type) {
    if (type != 0) {
        return NULL;
    }
    ktos_device *device = NULL;
    return ktos_open(0, &device) == 0 ? device : NULL;
}

static bool legacy_network_connect(const char *address, uint16_t port) {
    (void)address;
    (void)port;
    return false;
}

static void send_u8_command(ktos_device *device, uint32_t opcode, uint8_t value) {
    (void)ktos_iq_command(device, opcode, &value, 1);
}

static void send_u32_command(ktos_device *device, uint32_t opcode, uint32_t value) {
    uint8_t buf[4];
    write32(buf, value);
    (void)ktos_iq_command(device, opcode, buf, 4);
}

static void send_u64_command(ktos_device *device, uint32_t opcode, uint64_t value) {
    uint8_t buf[8];
    for (unsigned i = 0; i < 8; i++) {
        buf[i] = (uint8_t)(value >> (i * 8));
    }
    (void)ktos_iq_command(device, opcode, buf, 8);
}

/* RX control adapters */
static void rx_frequency(ktos_device *d, uint64_t hz)   { send_u64_command(d, 0x02, hz); }
static void rx_port(ktos_device *d, uint8_t port)       { send_u8_command(d, 0x01, port); }
static void rx_rate(ktos_device *d, uint32_t hz)        { send_u32_command(d, 0x05, hz); }
static void rx_att(ktos_device *d, uint8_t db)          { send_u8_command(d, 0x03, db); }
static void rx_if(ktos_device *d, uint8_t value)        { send_u8_command(d, 0x04, value); }
static void rx_amp(ktos_device *d, uint8_t on)          { send_u8_command(d, 0x08, on); }
static void rx_start(ktos_device *d)                    { (void)ktos_iq_command(d, 0x06, NULL, 0); }
static void rx_stop(ktos_device *d)                     { (void)ktos_iq_stop(d); }

/* TX control adapters */
static void tx_frequency(ktos_device *d, uint64_t hz)   { send_u64_command(d, 0x82, hz); }
static void tx_port(ktos_device *d, uint8_t port)       { send_u8_command(d, 0x81, port); }
static void tx_rate(ktos_device *d, uint32_t hz)        { send_u32_command(d, 0x85, hz); }
static void tx_att(ktos_device *d, uint8_t db)          { send_u8_command(d, 0x83, db); }
static void tx_if(ktos_device *d, uint8_t value)        { send_u8_command(d, 0x84, value); }
static void tx_start(ktos_device *d)                    { (void)ktos_iq_command(d, 0x86, NULL, 0); }
static void tx_stop(ktos_device *d)                     { (void)ktos_iq_command(d, 0x87, NULL, 0); }

static int legacy_status(ktos_device *device) {
    if (!device) {
        return 1;
    }
    return device->error ? 2 : 0;
}

static bool legacy_read_iq(ktos_device *device, void *buffer, uint32_t bytes) {
    uint32_t actual = 0;
    return ktos_iq_read(device, buffer, bytes, &actual) == 0 && actual == bytes;
}

static bool legacy_write_iq(ktos_device *device, void *buffer, uint32_t bytes) {
    uint32_t actual = 0;
    return ktos_iq_write(device, buffer, bytes, &actual) == 0 && actual == bytes;
}

static uint32_t legacy_bandwidth(ktos_device *device, uint16_t index) {
    (void)device;
    static const uint32_t rates[] = {
        40000000u, 20000000u, 10000000u, 5000000u, 2500000u,
        1250000u,  625000u,   312500u,   156250u,  78125u,
    };
    return index < sizeof(rates) / sizeof(rates[0]) ? rates[index] : 0;
}

static void *legacy_calibration_unsupported(ktos_device *device, void *request) {
    (void)request;
    sdk_failure(device, -ENOSYS, "factory calibration uses device UI");
    return NULL;
}

void *KTOS_CALL kcsdr_init(void) {
    static void *table[26] = {
        /* 00 */ legacy_find_device,
        /* 01 */ legacy_network_connect,
        /* 02 */ ktos_close,
        /* 03 */ rx_frequency,
        /* 04 */ rx_port,
        /* 05 */ rx_rate,
        /* 06 */ rx_att,
        /* 07 */ rx_if,
        /* 08 */ rx_amp,
        /* 09 */ NULL,
        /* 10 */ rx_start,
        /* 11 */ rx_stop,
        /* 12 */ legacy_calibration_unsupported,
        /* 13 */ legacy_calibration_unsupported,
        /* 14 */ tx_frequency,
        /* 15 */ tx_port,
        /* 16 */ tx_rate,
        /* 17 */ tx_att,
        /* 18 */ tx_if,
        /* 19 */ NULL,
        /* 20 */ tx_start,
        /* 21 */ tx_stop,
        /* 22 */ legacy_status,
        /* 23 */ legacy_read_iq,
        /* 24 */ legacy_write_iq,
        /* 25 */ legacy_bandwidth,
    };
    return table;
}
