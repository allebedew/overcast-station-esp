#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Climate and presence history in three ring buffers of different resolution:
 *   HISTORY_5M - 5 min of 1 s samples, RAM only
 *   HISTORY_1H - 1 h of 5 s points, RAM + flash
 *   HISTORY_1D - 24 h of 1 min averaged points, RAM + flash
 *
 * Filled by sampling climate_get() once a second — nothing pushes into it, so
 * each quantity is recorded for as long as its own sensor is alive. A sensor
 * slower than the sampling rate repeats its latest value into the 1 s tier;
 * the averaged tiers are unaffected. */
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
#define HISTORY_HAS_PIR   0x20

/* 16 bytes, and the rings hold 2460 of them. Fixed-point at the firmware-wide
 * resolutions from climate.h; float only for illuminance, which spans six
 * decades and fits no fixed scale. */
typedef struct {
    float lux;
    int32_t press_mhpa;  /* 0.001 hPa, as measured at the site: displayed
                          * reduced, so an altitude correction re-reduces the
                          * whole recorded history */
    uint16_t co2_ppm;
    int16_t temp_cx100;  /* 0.01 °C */
    uint16_t rh_dpct;    /* 0.1 % */
    uint8_t pir;         /* see below */
    uint8_t have; /* 0 = gap: nothing was recorded in that slot */
} history_point_t;

/* The PIR rides in the one byte the layout had spare, which is what keeps the
 * point at 16 bytes: the share of the slot the raw line spent high (0...100 in
 * seven bits, so a 1 s slot is 0 or 100) and the held presence flag on top.
 * Two quantities because they answer different questions -- how much movement
 * there was, and whether the room counted as occupied. */
#define HISTORY_PIR_MOTION(p)   ((p) & 0x7F)
#define HISTORY_PIR_PRESENT(p)  (((p) & 0x80) != 0)

/* Ordered as the panel's indoor rows are, since the knob cycles the chart's
 * quantity along this enum and the two would otherwise disagree. */
typedef enum {
    HISTORY_Q_TEMP,
    HISTORY_Q_PRESS,
    HISTORY_Q_RH,
    HISTORY_Q_CO2,
    HISTORY_Q_LUX,
    HISTORY_Q_COUNT,
} history_quantity_t;

/* Starts the 1 s sampling timer driving all tiers. */
void history_init(void);

int history_count(history_tier_t tier);

/* Slot duration of the tier, seconds. */
int history_interval(history_tier_t tier);

/* idx 0 = oldest stored point. Returns false if idx is out of range. */
bool history_get(history_tier_t tier, int idx, history_point_t *out);

/* One quantity of a tier for plotting: one value per `stride` slots, averaged
 * over them, in display units (C, %, hPa reduced to sea level, ppm, lx), oldest
 * first, NAN where the whole stride is a gap.
 * Returns how many of the `n` were written — a ring holding less than the whole
 * window fills the front of `v` and leaves the rest untouched. The columns lie
 * on a fixed grid of slots and only the rightmost one is still filling, so the
 * plot scrolls once per stride instead of shifting under every sample. That
 * grid holds across the ring's wrap only where `stride` divides the tier's
 * length, which is what CHART_RANGES picks its strides for. */
int history_series(history_tier_t tier, history_quantity_t q, int stride,
                   float *v, int n);

/* Clears all tiers (RAM and flash snapshots). Sampling keeps running and
 * starts filling the rings from empty again. */
void history_reset(void);
