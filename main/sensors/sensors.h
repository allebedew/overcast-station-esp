#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "bmp581.h"
#include "scd40.h"
#include "tmp117.h"
#include "veml7700.h"

/* Polling and publication for every sensor on the I2C bus: one task walks all
 * four at their own periods through a shared hot-plug state machine. The
 * drivers next to this file own the transport, the bus itself i2c_bus.c. */

/* Starts the polling task. i2c_bus_init() must have run first. */
void sensors_init(void);

/* Latest reading of each device. Each returns false — and leaves *out
 * untouched — while its sensor is absent, failing, or has not produced a
 * first reading yet. */
bool sensors_scd40_get(scd40_data_t *out);
bool sensors_tmp117_get(tmp117_data_t *out);
bool sensors_bmp581_get(bmp581_data_t *out);
bool sensors_veml7700_get(veml7700_data_t *out);

/* Forced recalibration of the SCD40, after it has measured ≥3 min in that
 * environment. Blocks the whole I2C bus for ~1 s; on success stores the applied
 * offset in *correction_ppm. ESP_ERR_INVALID_STATE if the sensor is offline. */
esp_err_t sensors_scd40_calibrate(uint16_t target_ppm, int *correction_ppm);
