#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Vishay VEML7700 — ambient light sensor with a lux-matched channel (ALS) and
 * an unfiltered white channel. The usable range spans six decades, so the
 * driver auto-ranges over the gain / integration-time table and reports which
 * step it settled on. */

typedef struct {
    float lux;       /* ALS channel, corrected */
    float white_lux; /* white channel, same scaling — an estimate: the channel
                      * has its own spectral response and no lux calibration,
                      * so als_raw/white_raw are the honest numbers */
    uint16_t als_raw;
    uint16_t white_raw;
    const char *gain; /* "1/8" | "1/4" | "1" | "2" */
    uint16_t it_ms;   /* integration time */
} veml7700_data_t;

/* Probes the (fixed) address, powers the sensor up and applies the starting
 * range. Safe to call again after a failure. */
esp_err_t veml7700_start(void);

/* Reads both channels and converts them to lux. Returns ESP_ERR_NOT_FINISHED
 * when no trustworthy sample is available yet: either the reading fell outside
 * the current range — the range is re-picked and the sample dropped — or an
 * integration under the newly applied settings has not completed. Either way
 * the next call gets a value. */
esp_err_t veml7700_read(veml7700_data_t *out);
