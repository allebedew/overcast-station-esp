#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Vishay VEML7700 — a lux-matched channel (ALS) and an unfiltered white one.
 * The range spans six decades, so the driver auto-ranges over the gain /
 * integration-time table and reports the step it settled on. */

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

/* Probes the address, powers up and applies the starting range. Safe to call
 * again after a failure. */
esp_err_t veml7700_start(void);

/* Both channels in lux. ESP_ERR_NOT_FINISHED when no trustworthy sample is
 * ready: the reading fell outside the range (which is re-picked and the sample
 * dropped), or the integration under new settings is unfinished. */
esp_err_t veml7700_read(veml7700_data_t *out);
