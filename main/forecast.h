#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Local weather forecast from the station's own barometer — no network, no
 * outside data: the Open-Meteo reading belongs to whichever location is
 * selected, which may be another city entirely.
 *
 * Two things, one derived from the other: the barometric tendency (least
 * squares over the last three hours of the 1 d history) and the Zambretti
 * forecast it feeds, which also takes the sea-level pressure and the season. */

typedef struct {
    bool ok;
    int8_t trend;        /* 0 steady, +-1 slow, +-2 fast, +-3 rapid */
    float delta_3h_hpa;  /* change over 3 h from the fitted slope, at sea level */
    uint8_t code;        /* 0..25, Zambretti A..Z */
} forecast_t;

/* Fills *out; ok is false until three hours of pressure are on record. Pure
 * composition over history.c — no task and no state beyond a short cache. */
void forecast_get(forecast_t *out);

/* The full forecast wording, and the same shortened to the 16 columns of the
 * LCD. Both return "" for a code out of range. */
const char *forecast_code_str(uint8_t code);
const char *forecast_code_short(uint8_t code);
