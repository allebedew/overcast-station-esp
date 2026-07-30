#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Current outdoor conditions fetched from Open-Meteo. */
typedef struct {
    float temp_c;           /* air temperature, °C */
    float feels_c;          /* apparent ("feels like") temperature, °C */
    float temp_min_c;       /* today's forecast minimum temperature, °C */
    float temp_max_c;       /* today's forecast maximum temperature, °C */
    float humidity_pct;     /* relative humidity, % */
    float pressure_hpa;     /* surface pressure at the station elevation, hPa */
    float pressure_msl_hpa; /* pressure reduced to sea level, hPa */
    float uvi;              /* UV index */
    float wind_kmh;         /* wind speed at 10 m, km/h */
    float gust_kmh;         /* wind gusts at 10 m, km/h */
    int wind_dir_deg;       /* wind direction, meteorological degrees (0 = N) */
    float precip_mm;        /* total precipitation over the last hour (rain +
                             * showers + snow water-equivalent), mm */
    int cloud_pct;          /* cloud cover, % */
    int weather_code;       /* WMO weather interpretation code, -1 if unknown */
    float elevation_m;      /* model grid-cell elevation for the coordinates, m */
    int utc_offset_s;       /* location's UTC offset (from timezone=auto), seconds */
    int32_t age_s;          /* seconds since the last successful fetch, -1 if none */
} weather_api_data_t;

void weather_api_init(void);

/* Wakes the fetch task to refresh immediately and drops the cached reading
 * (so it isn't shown for the newly selected location). Call after the active
 * location changes. */
void weather_api_refresh(void);

/* Latest fetched conditions. Returns false until the first successful fetch,
 * and again after a failed one — a reading is only reported while it comes
 * from the most recent attempt (*out is left as the last known values). */
bool weather_api_get(weather_api_data_t *out);

/* The active location's UTC offset in seconds, 0 while there is no reading.
 * Separate from weather_api_get() because the clock on the display needs
 * nothing else from the fetch and copying the whole struct on every frame —
 * under the lock — is all it would get. */
int weather_api_utc_offset_s(void);

/* Human-readable (English) description of a WMO weather code, e.g. for the
 * on-device display. Never NULL; returns "Unknown" for unmapped codes. */
const char *weather_api_code_str(int weather_code);
