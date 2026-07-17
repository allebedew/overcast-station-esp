#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 24 h of 1-minute averaged sensor readings in a RAM ring buffer. */
#define HISTORY_LEN 1440

typedef struct {
    bool valid; /* false = gap (no readings that minute) */
    uint16_t co2_ppm;
    int16_t temp_cx10;
    uint8_t rh_pct;
} history_point_t;

/* Starts the once-a-minute aggregation timer. */
void history_init(void);

/* Feeds one sensor reading; readings are averaged into 1-min points. */
void history_add(uint16_t co2_ppm, float temp_c, float rh_pct);

int history_count(void);

/* idx 0 = oldest stored point. Returns false if idx is out of range. */
bool history_get(int idx, history_point_t *out);
