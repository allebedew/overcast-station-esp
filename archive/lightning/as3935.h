#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* AMS AS3935 lightning sensor. The IRQ line is wired to a GPIO: the handler
 * timestamps the edge, the register read that clears it happens on the next
 * poll, and the driver keeps the running state the part itself does not. */

/* The part reports 1 km as "overhead" and 63 km as "out of range"; both are
 * flags, not distances, so distance_km is only meaningful without them. */
#define AS3935_DISTANCE_OVERHEAD     1
#define AS3935_DISTANCE_OUT_OF_RANGE 63

/* Everything here except the tuning describes the last 24 hours: a strike
 * older than that leaves no trace, because nothing in the part expires by
 * itself. */
typedef struct {
    int32_t last_strike_s;  /* seconds ago, -1 while there has been none */
    uint8_t distance_km;    /* of that strike, with the two flags above */
    uint32_t energy;        /* dimensionless, 21 bits, of that strike */
    uint16_t strikes_24h;   /* detections in the last 24 h, by whole hours */
    uint8_t noise_floor;    /* NF_LEV, 0-7, raised and lowered by the driver */
    uint8_t watchdog;       /* WDTH, and */
    uint8_t spike_reject;   /* SREJ: both follow the disturber rate */
    uint16_t disturbers_min; /* man-made interference per minute */
    uint8_t tun_cap;        /* antenna tuning capacitance picked at start, 0-15 */
    uint16_t lco_hz;        /* LCO/16 measured at it, 31250 nominal */
} as3935_data_t;

/* Probes the address, resets the part to its defaults, sweeps TUN_CAP against
 * the measured antenna frequency, runs the RCO calibration every power-up
 * needs and writes the configuration. Returns ESP_ERR_NOT_FINISHED while the
 * sweep is still walking — call again, the bus is free meanwhile. Safe to call
 * again after a failure. */
esp_err_t as3935_start(void);

/* Services the latched interrupt if one is pending and returns the current
 * state. Always succeeds when the part answers — there is no "no new data":
 * the state is meaningful between strikes too. */
esp_err_t as3935_read(as3935_data_t *out);
