/*
 * Copyright (c) 2026 Mfweb
 * SPDX-License-Identifier: MIT
 *
 * Typed C convenience wrappers for KTOS Digital Radio Mondiale (DRM30 / DRM+)
 * control and telemetry over ktos_rpc().
 */

#include "internal.h"

#include <math.h>
#include <stdlib.h>

#define DRM_SERVICE_COUNT   4u
#define DRM_PROFILE_COUNT   4u
#define DRM_IQ_INVERT_COUNT 3u

static int parse_device_error(const char *json) {
    if (strstr(json, "\"ok\":true")) {
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

int KTOS_CALL ktos_drm_start(ktos_device *device, uint32_t open_page) {
    if (!device || open_page > 1u) {
        return -EINVAL;
    }
    char command[64];
    snprintf(command, sizeof(command), "action drm_start %u", open_page);
    return rpc_expect_ok(device, command, "drm start");
}

int KTOS_CALL ktos_drm_stop(ktos_device *device) {
    if (!device) {
        return -EINVAL;
    }
    return rpc_expect_ok(device, "action drm_stop", "drm stop");
}

int KTOS_CALL ktos_drm_select_service(ktos_device *device, uint32_t service_index) {
    if (!device || service_index >= DRM_SERVICE_COUNT) {
        return -EINVAL;
    }
    char command[64];
    snprintf(command, sizeof(command), "action drm_service %u", service_index);
    return rpc_expect_ok(device, command, "drm service");
}

int KTOS_CALL ktos_drm_set_profile(ktos_device *device, uint32_t profile) {
    if (!device || profile >= DRM_PROFILE_COUNT) {
        return -EINVAL;
    }
    char command[64];
    snprintf(command, sizeof(command), "action drm_profile %u", profile);
    return rpc_expect_ok(device, command, "drm profile");
}

int KTOS_CALL ktos_drm_set_stereo(ktos_device *device, uint32_t enabled) {
    if (!device || enabled > 1u) {
        return -EINVAL;
    }
    char command[64];
    snprintf(command, sizeof(command), "action drm_stereo %u", enabled);
    return rpc_expect_ok(device, command, "drm stereo");
}

int KTOS_CALL ktos_drm_set_iq_invert(ktos_device *device, uint32_t mode) {
    if (!device || mode >= DRM_IQ_INVERT_COUNT) {
        return -EINVAL;
    }
    char command[64];
    snprintf(command, sizeof(command), "action drm_iq_invert %u", mode);
    return rpc_expect_ok(device, command, "drm iq invert");
}

int KTOS_CALL ktos_drm_reset(ktos_device *device) {
    if (!device) {
        return -EINVAL;
    }
    return rpc_expect_ok(device, "action drm_reset", "drm reset");
}

int KTOS_CALL ktos_drm_configure(ktos_device *device, const ktos_drm_config *config) {
    if (!device || !config || !config->mask || (config->mask & ~KTOS_DRM_CFG_ALL) != 0) {
        return -EINVAL;
    }
    if ((config->mask & KTOS_DRM_CFG_FREQUENCY) &&
        (!isfinite(config->frequency_hz) || config->frequency_hz <= 0.0)) {
        return -EINVAL;
    }
    if ((config->mask & KTOS_DRM_CFG_PROFILE) && config->profile >= DRM_PROFILE_COUNT) {
        return -EINVAL;
    }
    if ((config->mask & KTOS_DRM_CFG_SERVICE) && config->service_index >= DRM_SERVICE_COUNT) {
        return -EINVAL;
    }
    if ((config->mask & KTOS_DRM_CFG_STEREO) && config->stereo > 1u) {
        return -EINVAL;
    }
    if ((config->mask & KTOS_DRM_CFG_IQ_INVERT) && config->iq_invert >= DRM_IQ_INVERT_COUNT) {
        return -EINVAL;
    }

    int error = 0;

    if (!error && (config->mask & KTOS_DRM_CFG_FREQUENCY)) {
        char command[160];
        int  written = snprintf(command, sizeof(command), "set RX_FREQ %.12g", config->frequency_hz);
        if (written <= 0 || (size_t)written >= sizeof(command)) {
            return -EINVAL;
        }
        error = rpc_expect_ok(device, command, "RX_FREQ");
    }
    if (!error && (config->mask & KTOS_DRM_CFG_PROFILE)) {
        error = ktos_drm_set_profile(device, config->profile);
    }
    if (!error && (config->mask & KTOS_DRM_CFG_SERVICE)) {
        error = ktos_drm_select_service(device, config->service_index);
    }
    if (!error && (config->mask & KTOS_DRM_CFG_STEREO)) {
        error = ktos_drm_set_stereo(device, config->stereo);
    }
    if (!error && (config->mask & KTOS_DRM_CFG_IQ_INVERT)) {
        error = ktos_drm_set_iq_invert(device, config->iq_invert);
    }
    return error;
}

int KTOS_CALL ktos_drm_status(ktos_device *device, char *json, uint32_t capacity,
                              uint32_t *needed) {
    return ktos_rpc(device, "drm", json, capacity, needed);
}
