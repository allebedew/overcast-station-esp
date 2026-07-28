#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Sensirion SCD40 — photoacoustic CO2 sensor with temperature and humidity.
 * Transport only: the 16-bit command protocol with its CRC-8, the start
 * sequence and the two commands the rest of the firmware can trigger.
 * Polling, hot-plug and the published snapshot live in sensors.c. */

typedef struct {
    uint16_t co2_ppm;
    float temp_c;
    float rh_pct;
} scd40_data_t;

/* Attaches the device handle; no bus traffic. */
void scd40_init(void);

/* Probes the sensor and brings it into periodic measurement. The sequence has
 * waits in it that the sensor, not the bus, needs — up to 500 ms after the
 * stop command and 800 ms after an EEPROM write — so instead of sleeping on
 * the bus it runs one step per call and returns ESP_ERR_NOT_FINISHED for
 * "call me again at the next poll". Any other error abandons the sequence,
 * which restarts from the beginning on the following call. */
esp_err_t scd40_start(void);

/* Latest measurement, if one is due. The sensor produces a result every 5 s
 * at a phase we do not know until we have caught one, so this returns
 * ESP_ERR_NOT_FINISHED both while hunting for that phase and, without
 * touching the bus at all, during the quiet window after a successful read. */
esp_err_t scd40_read(scd40_data_t *out);

/* Ambient pressure compensation, hPa. Accepted during periodic measurement
 * and lost on power-down, so the caller re-applies it after every start. */
esp_err_t scd40_set_pressure(uint16_t hpa);

/* Forced recalibration (FRC): tells the sensor the actual CO2 level. The
 * sensor must have been measuring for ≥3 min in that environment. Stops and
 * restarts periodic measurement, blocking for ~1 s; on success stores the
 * applied offset (ppm) in *correction_ppm. ESP_FAIL if the sensor rejected
 * the calibration. */
esp_err_t scd40_calibrate(uint16_t target_ppm, int *correction_ppm);
