#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Bosch BMP581 — pressure and temperature (also the compatible BMP580).
 * Transport only. */

typedef struct {
    float press_hpa;
    float temp_c;
} bmp581_data_t;

/* Probes both addresses, checks the chip ID, resets and starts normal mode
 * with the pressure IIR filter on. Safe to call again after a failure. */
esp_err_t bmp581_start(void);

/* Reads the most recent conversion (temperature + pressure in one burst). */
esp_err_t bmp581_read(bmp581_data_t *out);
