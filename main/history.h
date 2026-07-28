#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Climate history in three ring buffers of different resolution:
 *   HISTORY_5M - 5 min of 1 s samples, RAM only
 *   HISTORY_1H - 1 h of 5 s points, RAM + flash
 *   HISTORY_1D - 24 h of 1 min averaged points, RAM + flash
 *
 * Filled by sampling climate_get() once a second — nothing pushes into it, so
 * a quantity keeps being recorded for as long as its own sensor is alive,
 * independently of the others. A sensor slower than the sampling rate (the
 * SCD40 produces a result every 5 s) simply repeats its latest value into the
 * 1 s tier; the averaged tiers are unaffected by that.
 */
typedef enum {
    HISTORY_5M,
    HISTORY_1H,
    HISTORY_1D,
    HISTORY_TIER_COUNT,
} history_tier_t;

/* Bits of history_point_t.have: which quantities the slot actually holds. */
#define HISTORY_HAS_CO2   0x01
#define HISTORY_HAS_TEMP  0x02
#define HISTORY_HAS_RH    0x04
#define HISTORY_HAS_PRESS 0x08
#define HISTORY_HAS_LUX   0x10

/* 16 bytes: the rings hold 2460 of these, so the layout is worth keeping
 * tight. Fixed-point where the resolution is known and bounded, float only
 * for illuminance — it spans six decades and no fixed scale fits it.
 *
 * The scales are the firmware-wide reading resolutions listed in climate.h —
 * 0.01 °C, 0.001 hPa, 1 ppm, 0.1 %, 0.1 lx — so a point drawn on a chart and
 * the same reading on the page carry the same digits. They are resolutions,
 * not accuracies: a lone SCD40 humidity sample is worth ±6 % and the third
 * decimal of an instantaneous pressure is noise. It is the minute-long
 * averages and the shape of the curve that make the extra digits worth
 * storing. */
typedef struct {
    float lux;
    int32_t press_mhpa;  /* 0.001 hPa */
    uint16_t co2_ppm;
    int16_t temp_cx100;  /* 0.01 °C */
    uint16_t rh_dpct;    /* 0.1 % */
    uint8_t have; /* 0 = gap: nothing was recorded in that slot */
} history_point_t;

/* Starts the 1 s sampling timer driving all tiers. */
void history_init(void);

int history_count(history_tier_t tier);

/* Slot duration of the tier, seconds. */
int history_interval(history_tier_t tier);

/* idx 0 = oldest stored point. Returns false if idx is out of range. */
bool history_get(history_tier_t tier, int idx, history_point_t *out);
