#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Sensirion SCD40 — photoacoustic CO2 sensor with temperature and humidity.
 * The 16-bit command protocol with its CRC-8, the start sequence, the two
 * commands the rest of the firmware can trigger, and the dew point, which is
 * derived from this part's own pair and shown on its card.
 * Polling, hot-plug and the published snapshot live in sensors.c. */

typedef struct {
    uint16_t co2_ppm;
    float temp_c;
    float rh_pct;
    float dew_c; /* derived from temp_c and rh_pct: the chip's warm offset
                  * cancels, its RH being relative to that same reading */
} scd40_data_t;

/* Attaches the device handle; no bus traffic. */
void scd40_init(void);

/* Brings the sensor into periodic measurement. Its waits are the sensor's, not
 * the bus's (up to 500 ms after stop, 800 ms after an EEPROM write), so it runs
 * one step per call and returns ESP_ERR_NOT_FINISHED for "call me again". Any
 * other error abandons the sequence, which restarts from the beginning. */
esp_err_t scd40_start(void);

/* Latest measurement, if one is due. A result appears every 5 s at a phase not
 * known until one is caught, so ESP_ERR_NOT_FINISHED comes back both while
 * hunting and, without touching the bus, in the quiet window after a read. */
esp_err_t scd40_read(scd40_data_t *out);

/* Ambient pressure compensation, hPa; lost on power-down, so the caller
 * re-applies it after every start. */
esp_err_t scd40_set_pressure(uint16_t hpa);

/* Forced recalibration: tells the sensor the actual CO2 level, after it has
 * measured ≥3 min in that environment. Blocks ~1 s restarting the measurement;
 * on success stores the applied offset in *correction_ppm. */
esp_err_t scd40_calibrate(uint16_t target_ppm, int *correction_ppm);
