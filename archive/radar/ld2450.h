#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Hi-Link LD2450 — a 24 GHz tracker, not a presence sensor: it reports the
 * position and speed of up to three *moving* targets and loses whoever sits
 * still. Everything it offers arrives as an unprompted UART stream; the module
 * needs no configuration to produce it. */

#define LD2450_MAX_TARGETS 3

typedef struct {
    int16_t x_mm;      /* across the antenna, right of centre positive */
    int16_t y_mm;      /* away from the antenna */
    int16_t speed_cms; /* approaching the module negative */
    uint16_t res_mm;   /* the module's own distance resolution for this target */
    float dist_m;
} ld2450_target_t;

typedef struct {
    uint8_t count; /* targets currently tracked, 0..LD2450_MAX_TARGETS */
    ld2450_target_t targets[LD2450_MAX_TARGETS];

    /* The nearest target, valid while count > 0. */
    float nearest_m;

    /* count > 0 now or recently: a tracked person stops moving for seconds at a
     * time and drops out of the frame, so the raw count flickers and this does
     * not. */
    bool presence;

    int32_t age_ms; /* since the frame these targets came from */
} ld2450_data_t;

/* Brings up the UART and starts the reader task. */
void ld2450_init(void);

/* Latest frame. False — and *out untouched — until the first frame arrives and
 * again once the stream has been silent for a couple of seconds, which is the
 * module being unplugged or dead. */
bool ld2450_get(ld2450_data_t *out);
