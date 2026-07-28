#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* TI TMP117 — high-accuracy digital temperature sensor (±0.1 °C).
 * Transport only: probing, configuration and one register read. Polling,
 * hot-plug and the published snapshot live in sensors.c. */

typedef struct {
    float temp_c;
} tmp117_data_t;

/* Probes the four possible addresses, verifies the device ID and starts
 * continuous conversion (8 averaged samples, 125 ms cycle — see the config
 * comment in tmp117.c). Safe to call again after a failure. */
esp_err_t tmp117_start(void);

/* Latest conversion result. The sensor keeps converting on its own, so this
 * is a plain register read. */
esp_err_t tmp117_read(tmp117_data_t *out);
