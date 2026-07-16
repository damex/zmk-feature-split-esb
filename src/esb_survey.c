// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Boot-time energy sweep over the hop pool, raw RADIO registers.
 * ESB owns the radio afterwards, so this runs only before esb_init.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>

#include <hal/nrf_radio.h>

#include "esb_survey.h"
#include "hop_policy.h"

#define SURVEY_PASSES 4
#define SURVEY_SAMPLES_PER_PASS 8
#define SURVEY_PASS_GAP_MS 5
#define SURVEY_SILENT_DBM INT8_MIN
#define SURVEY_RSSI_SETTLE_US 15

static int8_t sample_channel_dbm(uint8_t channel) {
    nrf_radio_frequency_set(NRF_RADIO, 2400 + channel);
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_READY);
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
    while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_READY)) {
    }
    k_busy_wait(SURVEY_RSSI_SETTLE_US);
    int8_t peak_dbm = SURVEY_SILENT_DBM;
    for (uint8_t sample = 0; sample < SURVEY_SAMPLES_PER_PASS; sample++) {
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_RSSIEND);
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTART);
        while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_RSSIEND)) {
        }
        int8_t dbm = hop_policy_rssi_to_dbm((int8_t)nrf_radio_rssi_sample_get(NRF_RADIO));
        if (dbm > peak_dbm) {
            peak_dbm = dbm;
        }
    }
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
    while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED)) {
    }
    return peak_dbm;
}

void esb_survey_run(const uint8_t *channels, size_t count, int8_t *energy_dbm) {
    __ASSERT_NO_MSG(channels != NULL);
    __ASSERT_NO_MSG(energy_dbm != NULL);
    nrf_radio_power_set(NRF_RADIO, true);
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_1MBIT);
    for (size_t index = 0; index < count; index++) {
        energy_dbm[index] = SURVEY_SILENT_DBM;
    }
    for (uint8_t pass = 0; pass < SURVEY_PASSES; pass++) {
        for (size_t index = 0; index < count; index++) {
            int8_t dbm = sample_channel_dbm(channels[index]);
            if (dbm > energy_dbm[index]) {
                energy_dbm[index] = dbm;
            }
        }
        k_msleep(SURVEY_PASS_GAP_MS);
    }
    /* Power cycle resets every register, esb_init starts from reset state. */
    nrf_radio_power_set(NRF_RADIO, false);
    nrf_radio_power_set(NRF_RADIO, true);
}
