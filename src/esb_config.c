// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Applies "esb/" settings to the radio: settings_load at boot, settings_runtime_set
 * live. Value is a uint32, cast per key.
 */
#include <errno.h>
#include <stdint.h>

#include <esb.h>

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

static int esb_config_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    uint32_t value;
    if (len != sizeof(value)) {
        return -EINVAL;
    }
    if (read_cb(cb_arg, &value, sizeof(value)) < 0) {
        return -EIO;
    }
    if (settings_name_steq(name, "tx_power", &next) && next == NULL) {
        return esb_set_tx_power((int8_t)value);
    }
    if (settings_name_steq(name, "retransmit_count", &next) && next == NULL) {
        return esb_set_retransmit_count((uint16_t)value);
    }
    if (settings_name_steq(name, "retransmit_delay", &next) && next == NULL) {
        return esb_set_retransmit_delay((uint16_t)value);
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(esb_config, "esb", NULL, esb_config_set, NULL, NULL);

/* After peripheral_init brings the radio up, so a restored value applies live. */
static int esb_config_init(void) {
    int error = settings_subsys_init();
    if (error < 0) {
        LOG_ERR("settings init failed (%d)", error);
        return error;
    }
    error = settings_load_subtree("esb");
    if (error < 0) {
        LOG_ERR("esb settings load failed (%d)", error);
    }
    return error;
}
SYS_INIT(esb_config_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
