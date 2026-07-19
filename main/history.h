#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Sensor history in three ring buffers of different resolution:
 *   HISTORY_5M - 5 min of 1 s samples, RAM only
 *   HISTORY_1H - 1 h of 5 s points (native SCD40 rate), RAM + flash
 *   HISTORY_1D - 24 h of 1 min averaged points, RAM + flash
 */
typedef enum {
    HISTORY_5M,
    HISTORY_1H,
    HISTORY_1D,
    HISTORY_TIER_COUNT,
} history_tier_t;

typedef struct {
    bool valid; /* false = gap (no readings in that slot) */
    uint16_t co2_ppm;
    int16_t temp_cx10;
    uint8_t rh_pct;
} history_point_t;

/* Starts the 1 s aggregation timer driving all tiers. */
void history_init(void);

/* Feeds one sensor reading; readings are averaged into tier points. */
void history_add(uint16_t co2_ppm, float temp_c, float rh_pct);

int history_count(history_tier_t tier);

/* Slot duration of the tier, seconds. */
int history_interval(history_tier_t tier);

/* idx 0 = oldest stored point. Returns false if idx is out of range. */
bool history_get(history_tier_t tier, int idx, history_point_t *out);
