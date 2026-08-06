#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* AMS AS3935 lightning sensor. No IRQ line is wired, so the part is polled and
 * the driver keeps the running state the interrupt would otherwise carry. */

/* The part reports 1 km as "overhead" and 63 km as "out of range"; both are
 * flags, not distances, so distance_km is only meaningful without them. */
#define AS3935_DISTANCE_OVERHEAD     1
#define AS3935_DISTANCE_OUT_OF_RANGE 63

typedef struct {
    bool storm;             /* a strike inside the activity window */
    uint8_t distance_km;    /* estimated distance to the head of the storm */
    bool overhead;
    bool out_of_range;
    uint32_t energy;        /* dimensionless, 21 bits, of the last strike */
    uint32_t strikes;       /* since start; increments once per detection */
    int32_t last_strike_s;  /* seconds ago, -1 while there has been none */
    uint8_t noise_floor;    /* NF_LEV, 0-7, raised and lowered by the driver */
    uint16_t disturbers_min; /* man-made interference per minute */
} as3935_data_t;

/* Probes the address, runs the RCO calibration every power-up needs and writes
 * the configuration. Safe to call again after a failure. */
esp_err_t as3935_start(void);

/* Services the latched interrupt and returns the current state. Always
 * succeeds when the part answers — there is no "no new data": the state is
 * meaningful between strikes too. */
esp_err_t as3935_read(as3935_data_t *out);
