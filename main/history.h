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
#define HISTORY_HAS_RADAR 0x20

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
    uint8_t radar;       /* see below */
    uint8_t have; /* 0 = gap: nothing was recorded in that slot */
} history_point_t;

/* The radar's slot rides in the one byte the layout had spare, which is what
 * keeps the point at 16 bytes: the largest number of targets the slot saw
 * (2 bits) and the mean distance to the nearest of them (5 bits, a quarter of
 * a metre per step, 0 for a slot with an empty fan).
 * Max for the count and mean for the distance, each against its own failure:
 * a person who stops moving drops out of the frame and would dent a mean
 * count, while one ghost frame would pull a minimum distance to the sensor. */
#define HISTORY_RADAR_STEP_M    0.25f
#define HISTORY_RADAR_TARGETS(r) ((r) & 0x03)
#define HISTORY_RADAR_STEPS(r)   (((r) >> 2) & 0x1F)
#define HISTORY_RADAR_NEAR_M(r)  (HISTORY_RADAR_STEPS(r) * HISTORY_RADAR_STEP_M)

/* Starts the 1 s sampling timer driving all tiers. */
void history_init(void);

int history_count(history_tier_t tier);

/* Slot duration of the tier, seconds. */
int history_interval(history_tier_t tier);

/* idx 0 = oldest stored point. Returns false if idx is out of range. */
bool history_get(history_tier_t tier, int idx, history_point_t *out);

/* Clears all tiers (RAM and flash snapshots). Sampling keeps running and
 * starts filling the rings from empty again. */
void history_reset(void);
