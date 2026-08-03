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
    float precip_mmh;       /* precipitation rate (rain + showers + snow
                             * water-equivalent), mm/h — under 2.5 light,
                             * 2.5..7.5 moderate, above that heavy */
    bool is_day;            /* daylight at the location; the WMO code alone does
                             * not tell a clear day from a clear night */
    int32_t sunshine_s;     /* today's total sunshine (direct radiation above
                             * 120 W/m²), seconds; -1 if the model omits it */
    int32_t daylight_s;     /* today's total daylight, seconds; -1 if unknown */
    int cloud_pct;          /* cloud cover, % */
    int weather_code;       /* WMO weather interpretation code, -1 if unknown */
    float elevation_m;      /* model grid-cell elevation for the coordinates, m */
    int utc_offset_s;       /* location's UTC offset (from timezone=auto), seconds */
    int32_t age_s;          /* seconds since the last successful fetch, -1 if none */
} weather_api_data_t;

void weather_api_init(void);

/* Drops the cached reading and refreshes immediately. Call after the active
 * location changes. */
void weather_api_refresh(void);

/* Latest conditions. False until the first successful fetch and again after a
 * failed one; *out is left as the last known values. */
bool weather_api_get(weather_api_data_t *out);

/* The active location's UTC offset: the live reading, else the one stored with
 * the location, else 0. Separate from weather_api_get() so the display clock
 * need not copy the struct per frame. */
int weather_api_utc_offset_s(void);

/* Wind direction as a single cardinal letter, "N" / "E" / "S" / "W". */
const char *weather_api_wind_dir_str(int deg);

/* English description of a WMO weather code. Never NULL. */
const char *weather_api_code_str(int weather_code);

/* The same in 10 characters — all the 16x2 row leaves next to the temperature.
 * Intensity becomes a '-' / '+' suffix. Never NULL. */
const char *weather_api_code_short(int weather_code);
