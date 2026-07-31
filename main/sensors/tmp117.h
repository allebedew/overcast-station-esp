#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* TI TMP117 — temperature to ±0.1 °C. Transport only; polling, hot-plug and
 * the published snapshot live in sensors.c. */

typedef struct {
    float temp_c;
} tmp117_data_t;

/* Probes the four addresses, verifies the device ID and starts continuous
 * conversion. Safe to call again after a failure. */
esp_err_t tmp117_start(void);

/* Latest conversion result — a plain register read. */
esp_err_t tmp117_read(tmp117_data_t *out);
