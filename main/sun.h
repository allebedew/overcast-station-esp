#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Sunrise and sunset for the active location, from its coordinates and the
 * SNTP clock — the NOAA sunrise equation, no network. Times are UTC; whoever
 * shows them applies the location's offset.
 *
 * Within a minute of the USNO tables in temperate latitudes, a few minutes past
 * 60° where the sun meets the horizon too shallowly for a truncated series. */

typedef enum {
    SUN_RISES,       /* the sun crosses the horizon on this day */
    SUN_POLAR_DAY,   /* it never sets */
    SUN_POLAR_NIGHT, /* it never rises */
} sun_state_t;

/* Where the sun stands right now on the twilight ladder. Unlike sun_state_t,
 * which describes a whole day, this is a property of the moment. The bands are
 * the conventional ones, by the elevation of the sun's centre. */
typedef enum {
    SUN_PHASE_DAY,      /* above +6° */
    SUN_PHASE_GOLDEN,   /* horizon..+6°: up, but low enough to redden */
    SUN_PHASE_CIVIL,    /* -6°..horizon: outdoors without artificial light */
    SUN_PHASE_NAUTICAL, /* -12°..-6°: the sea horizon still shows */
    SUN_PHASE_ASTRO,    /* -18°..-12° */
    SUN_PHASE_NIGHT,    /* below -18° */
} sun_phase_t;

typedef struct {
    sun_state_t state;
    time_t rise;       /* UTC; both meaningless unless SUN_RISES */
    time_t set;
    int32_t day_len_s; /* a full day through a polar day, 0 through a polar night */
    bool up;           /* above the horizon at `now`, whatever day was asked for */
} sun_info_t;

/* The local day `day_offset` days from today. False without a synced clock or
 * an active location. A location whose offset has never been fetched falls back
 * to solar time from its longitude — enough to pick a day, not to show one. */
bool sun_get(int day_offset, sun_info_t *out);

/* Seconds to the next sunrise or sunset. False through a polar day or night,
 * where the next crossing is weeks out. */
bool sun_next_event(int32_t *in_s, bool *is_rise);

/* Degrees above the horizon right now, negative below. Needs no UTC offset. */
bool sun_elevation_deg(double *out);

/* The current phase. False without a synced clock or an active location. */
bool sun_phase(sun_phase_t *out);

/* The phase of a given elevation, in degrees. */
sun_phase_t sun_phase_of(double elev_deg);

/* API name of a phase: "day", "golden", "civil", "nautical", "astro", "night".
 * Never NULL. */
const char *sun_phase_str(sun_phase_t phase);

/* The math alone, for a place and an instant. Longitude is east-positive. */
void sun_compute(double lat, double lon, int32_t utc_offset_s, time_t now,
                 int day_offset, sun_info_t *out);

double sun_elevation_at(double lat, double lon, time_t now);
