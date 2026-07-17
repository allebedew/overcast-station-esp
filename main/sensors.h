#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint16_t co2_ppm;
    float temp_c;
    float rh_pct;
} scd40_data_t;

void sensors_init(void);

/* Температура чипа, °C; -273 при ошибке чтения. */
float sensors_chip_temp(void);

/* Latest SCD40 reading (updated every ~5 s).
 * Returns false while the sensor is absent or failing. */
bool sensors_scd40_get(scd40_data_t *out);

/* Forced recalibration (FRC): tells the sensor the actual CO2 level.
 * The sensor must have been measuring for ≥3 min in that environment.
 * Blocks for ~1 s; on success stores the applied offset (ppm) in
 * *correction_ppm. ESP_ERR_INVALID_STATE if the sensor is offline,
 * ESP_FAIL if the sensor rejected the calibration. */
esp_err_t sensors_scd40_calibrate(uint16_t target_ppm, int *correction_ppm);
