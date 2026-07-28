#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "bmp581.h"
#include "scd40.h"
#include "tmp117.h"
#include "veml7700.h"

/* Polling and publication for every sensor on the I2C bus. One task walks all
 * four, each at its own period, through a shared hot-plug state machine; the
 * drivers next to this file own the transport and nothing else. The bus itself
 * belongs to i2c_bus.c — a module that needs it for something other than a
 * sensor (the display) takes it from there. */

/* Starts the polling task. i2c_bus_init() must have run first. */
void sensors_init(void);

/* Latest reading of each device. Each returns false — and leaves *out
 * untouched — while its sensor is absent, failing, or has not produced a
 * first reading yet. */
bool sensors_scd40_get(scd40_data_t *out);
bool sensors_tmp117_get(tmp117_data_t *out);
bool sensors_bmp581_get(bmp581_data_t *out);
bool sensors_veml7700_get(veml7700_data_t *out);

/* Forced recalibration (FRC) of the SCD40: tells the sensor the actual CO2
 * level. The sensor must have been measuring for ≥3 min in that environment.
 * Blocks the whole I2C bus for ~1 s; on success stores the applied offset
 * (ppm) in *correction_ppm. ESP_ERR_INVALID_STATE if the sensor is offline,
 * ESP_FAIL if the sensor rejected the calibration. */
esp_err_t sensors_scd40_calibrate(uint16_t target_ppm, int *correction_ppm);
