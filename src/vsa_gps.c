/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Typed C convenience wrappers for KTOS Vector Signal Analysis (VSA) and
 * software GPS L1 control/telemetry over ktos_rpc().
 */

#include "internal.h"

#include <math.h>
#include <stdlib.h>

#define VSA_PRESET_COUNT 7u
#define VSA_VIEW_COUNT   3u
#define GPS_ZONE_MIN     (-48)
#define GPS_ZONE_MAX     56

/* -------------------------------------------------------------------------- */
/* Internal JSON envelope helper                                              */
/* -------------------------------------------------------------------------- */

static int parse_device_error(const char *json) {
    const char *ok_true = strstr(json, "\"ok\":true");
    if (ok_true) {
        return 0;
    }
    const char *code = strstr(json, "\"code\":");
    if (code) {
        long value = strtol(code + 7, NULL, 10);
        if (value < 0) {
            return (int)value;
        }
        if (value > 0) {
            return -(int)value;
        }
    }
    return -EIO;
}

static int rpc_expect_ok(ktos_device *device, const char *command, const char *operation) {
    char     reply[512];
    uint32_t needed = 0;
    int      error  = ktos_rpc(device, command, reply, sizeof(reply), &needed);
    if (error) {
        return error;
    }
    error = parse_device_error(reply);
    return error ? sdk_failure(device, error, operation) : 0;
}

static int set_vsa_double(ktos_device *device, const char *name, double value) {
    if (!isfinite(value)) {
        return -EINVAL;
    }
    char command[160];
    int  written = snprintf(command, sizeof(command), "set %s %.12g", name, value);
    if (written <= 0 || (size_t)written >= sizeof(command)) {
        return -EINVAL;
    }
    return rpc_expect_ok(device, command, name);
}

static int set_vsa_uint(ktos_device *device, const char *name, uint32_t value) {
    char command[160];
    int  written = snprintf(command, sizeof(command), "set %s %u", name, value);
    if (written <= 0 || (size_t)written >= sizeof(command)) {
        return -EINVAL;
    }
    return rpc_expect_ok(device, command, name);
}

/* -------------------------------------------------------------------------- */
/* Vector Signal Analysis (VSA)                                               */
/* -------------------------------------------------------------------------- */

int KTOS_CALL ktos_vsa_enter(ktos_device *device) {
    if (!device) {
        return -EINVAL;
    }
    return rpc_expect_ok(device, "mode 6", "vsa enter");
}

int KTOS_CALL ktos_vsa_configure(ktos_device *device, const ktos_vsa_config *config) {
    if (!device || !config || !config->mask || (config->mask & ~KTOS_VSA_CFG_ALL) != 0) {
        return -EINVAL;
    }

    int error = 0;

    if (!error && (config->mask & KTOS_VSA_CFG_PORT)) {
        error = set_vsa_uint(device, "vsa.port", config->port);
    }
    if (!error && (config->mask & KTOS_VSA_CFG_FREQUENCY)) {
        error = set_vsa_double(device, "vsa.frequency", config->frequency_hz);
    }
    if (!error && (config->mask & KTOS_VSA_CFG_MODULATION)) {
        error = set_vsa_uint(device, "vsa.modulation", config->modulation);
    }
    if (!error && (config->mask & KTOS_VSA_CFG_SYMBOL_RATE)) {
        error = set_vsa_double(device, "vsa.symbol_rate", config->symbol_rate_hz);
    }
    if (!error && (config->mask & KTOS_VSA_CFG_FILTER)) {
        error = set_vsa_uint(device, "vsa.filter", config->filter);
    }
    if (!error && (config->mask & KTOS_VSA_CFG_ALPHA)) {
        error = set_vsa_double(device, "vsa.alpha", config->alpha);
    }
    if (!error && (config->mask & KTOS_VSA_CFG_EQUALIZER)) {
        error = set_vsa_uint(device, "vsa.equalizer", config->equalizer);
    }
    if (!error && (config->mask & KTOS_VSA_CFG_SYMBOLS)) {
        error = set_vsa_uint(device, "vsa.symbols", config->symbols);
    }
    if (!error && (config->mask & KTOS_VSA_CFG_AVERAGE)) {
        error = set_vsa_uint(device, "vsa.average", config->average);
    }
    if (!error && (config->mask & KTOS_VSA_CFG_FSK_DEVIATION)) {
        error = set_vsa_double(device, "vsa.fsk_deviation", config->fsk_deviation_hz);
    }
    if (!error && (config->mask & KTOS_VSA_CFG_ATT)) {
        error = set_vsa_uint(device, "vsa.att", config->att_db);
    }
    if (!error && (config->mask & KTOS_VSA_CFG_AMP)) {
        error = set_vsa_uint(device, "vsa.amp", config->amp_mode);
    }
    if (!error && (config->mask & KTOS_VSA_CFG_IF_GAIN)) {
        error = set_vsa_uint(device, "vsa.if_gain", config->if_gain_db);
    }
    return error;
}

int KTOS_CALL ktos_vsa_preset(ktos_device *device, uint32_t preset) {
    if (!device || preset >= VSA_PRESET_COUNT) {
        return -EINVAL;
    }
    char command[64];
    snprintf(command, sizeof(command), "action vsa_preset %u", preset);
    return rpc_expect_ok(device, command, "vsa preset");
}

int KTOS_CALL ktos_vsa_start(ktos_device *device) {
    if (!device) {
        return -EINVAL;
    }
    return rpc_expect_ok(device, "action run", "vsa start");
}

int KTOS_CALL ktos_vsa_stop(ktos_device *device) {
    if (!device) {
        return -EINVAL;
    }
    return rpc_expect_ok(device, "action stop", "vsa stop");
}

int KTOS_CALL ktos_vsa_single(ktos_device *device) {
    if (!device) {
        return -EINVAL;
    }
    return rpc_expect_ok(device, "action vsa_single", "vsa single");
}

int KTOS_CALL ktos_vsa_clear_persist(ktos_device *device) {
    if (!device) {
        return -EINVAL;
    }
    return rpc_expect_ok(device, "action vsa_clear_persist", "vsa clear persist");
}

int KTOS_CALL ktos_vsa_view(ktos_device *device, uint32_t view) {
    if (!device || view >= VSA_VIEW_COUNT) {
        return -EINVAL;
    }
    char command[64];
    snprintf(command, sizeof(command), "action vsa_view %u", view);
    return rpc_expect_ok(device, command, "vsa view");
}

int KTOS_CALL ktos_vsa_status(ktos_device *device, char *json, uint32_t capacity,
                              uint32_t *needed) {
    return ktos_rpc(device, "vsa", json, capacity, needed);
}

int KTOS_CALL ktos_vsa_frame(ktos_device *device, const char *section, char *json,
                             uint32_t capacity, uint32_t *needed) {
    if (!section || !section[0]) {
        return ktos_rpc(device, "vsa_frame", json, capacity, needed);
    }
    if (strcmp(section, "all") != 0 && strcmp(section, "summary") != 0 &&
        strcmp(section, "constellation") != 0 && strcmp(section, "spectrum") != 0 &&
        strcmp(section, "evm") != 0 && strcmp(section, "eye") != 0) {
        return -EINVAL;
    }
    char command[64];
    snprintf(command, sizeof(command), "vsa_frame %s", section);
    return ktos_rpc(device, command, json, capacity, needed);
}

/* -------------------------------------------------------------------------- */
/* Software GPS L1                                                            */
/* -------------------------------------------------------------------------- */

int KTOS_CALL ktos_gps_start(ktos_device *device, uint32_t open_page) {
    if (!device || open_page > 1u) {
        return -EINVAL;
    }
    char command[64];
    snprintf(command, sizeof(command), "action gps_start %u", open_page);
    return rpc_expect_ok(device, command, "gps start");
}

int KTOS_CALL ktos_gps_stop(ktos_device *device) {
    if (!device) {
        return -EINVAL;
    }
    return rpc_expect_ok(device, "action gps_stop", "gps stop");
}

int KTOS_CALL ktos_gps_status(ktos_device *device, char *json, uint32_t capacity,
                              uint32_t *needed) {
    return ktos_rpc(device, "gps", json, capacity, needed);
}

int KTOS_CALL ktos_gps_track(ktos_device *device, uint32_t offset, uint32_t limit, char *json,
                             uint32_t capacity, uint32_t *needed) {
    if (limit == 0) {
        limit = 64u;
    }
    if (limit > 200u) {
        return -EINVAL;
    }
    char command[64];
    snprintf(command, sizeof(command), "gps_track %u %u", offset, limit);
    return ktos_rpc(device, command, json, capacity, needed);
}

int KTOS_CALL ktos_gps_sync_location(ktos_device *device) {
    if (!device) {
        return -EINVAL;
    }
    return rpc_expect_ok(device, "action gps_sync_location", "gps sync location");
}

int KTOS_CALL ktos_gps_sync_clock(ktos_device *device, int32_t timezone_quarters) {
    if (!device) {
        return -EINVAL;
    }
    if (timezone_quarters == KTOS_GPS_TZ_AUTO) {
        return rpc_expect_ok(device, "action gps_sync_clock", "gps sync clock");
    }
    if (timezone_quarters < GPS_ZONE_MIN || timezone_quarters > GPS_ZONE_MAX) {
        return -EINVAL;
    }
    char command[64];
    snprintf(command, sizeof(command), "action gps_sync_clock %d", (int)timezone_quarters);
    return rpc_expect_ok(device, command, "gps sync clock");
}

int KTOS_CALL ktos_gps_track_save(ktos_device *device) {
    if (!device) {
        return -EINVAL;
    }
    return rpc_expect_ok(device, "action gps_track_save", "gps track save");
}

int KTOS_CALL ktos_gps_track_reset(ktos_device *device) {
    if (!device) {
        return -EINVAL;
    }
    return rpc_expect_ok(device, "action gps_track_reset", "gps track reset");
}
