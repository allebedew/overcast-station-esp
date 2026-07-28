#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Bosch BMP581 — barometric pressure + temperature (also covers the pin- and
 * register-compatible BMP580). Transport only: probing, configuration and the
 * burst read of the six data registers. */

typedef struct {
    float press_hpa;
    float temp_c;
} bmp581_data_t;

/* Probes both addresses, checks the chip ID, soft-resets and puts the sensor
 * into normal mode at 4 Hz with the pressure IIR filter on. Safe to call again
 * after a failure. */
esp_err_t bmp581_start(void);

/* Reads the most recent conversion (temperature + pressure in one burst). */
esp_err_t bmp581_read(bmp581_data_t *out);
