#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define WEATHER_API_FORECAST_DAYS 5

/* One day of the daily forecast; index 0 is today. */
typedef struct {
    time_t date;            /* local midnight of that day; read with gmtime_r */
    float  temp_min_c;      /* °C */
    float  temp_max_c;      /* °C */
    int    precip_prob_pct; /* precipitation probability, %; -1 if unknown */
    int    cloud_pct;       /* mean cloud cover over the whole day, %;
                             * -1 if unknown */
    int    weather_code;    /* WMO weather interpretation code, -1 if unknown */
} weather_api_day_t;

/* `uvi` when neither request produced a UV index. */
#define WEATHER_API_UVI_NONE (-1.0f)

/* Current outdoor conditions fetched from Open-Meteo. */
typedef struct {
    float temp_c;           /* air temperature, °C */
    float feels_c;          /* apparent ("feels like") temperature, °C */
    float humidity_pct;     /* relative humidity, % */
    float pressure_hpa;     /* surface pressure at the station elevation, hPa */
    float pressure_msl_hpa; /* pressure reduced to sea level, hPa */
    float uvi;              /* UV index, WEATHER_API_UVI_NONE if unavailable */
    float wind_kmh;         /* wind speed at 10 m, km/h */
    float gust_kmh;         /* wind gusts at 10 m, km/h */
    int wind_dir_deg;       /* wind direction, meteorological degrees (0 = N) */
    float precip_mmh;       /* precipitation rate (rain + showers + snow
                             * water-equivalent), mm/h — under 2.5 light,
                             * 2.5..7.5 moderate, above that heavy */
    bool is_day;            /* daylight at the location; the WMO code alone does
                             * not tell a clear day from a clear night */
    int32_t daylight_s;     /* today's total daylight, seconds; -1 if unknown */
    int cloud_pct;          /* cloud cover, % */
    int weather_code;       /* WMO weather interpretation code, -1 if unknown */
    float elevation_m;      /* model grid-cell elevation for the coordinates, m */
    int utc_offset_s;       /* location's UTC offset (from timezone=auto), seconds */
    int32_t age_s;          /* seconds since the last successful fetch, -1 if none */
    weather_api_day_t days[WEATHER_API_FORECAST_DAYS];
    int day_count;          /* days[] entries filled; today's min/max is days[0] */
} weather_api_data_t;

void weather_api_init(void);

/* Drops the cached reading and refreshes immediately. Call after the active
 * location changes. */
void weather_api_refresh(void);

/* Latest conditions. False until the first successful fetch and again after a
 * failed one; *out is left as the last known values. */
bool weather_api_get(weather_api_data_t *out);

/* True while a fetch is on the air, and for a short while after a very quick
 * one, so the display's indicator is never a single-frame flash. */
bool weather_api_is_fetching(void);

/* The active location's UTC offset: the live reading, else the one stored with
 * the location, else 0. Separate from weather_api_get() so the display clock
 * need not copy the struct per frame. */
int weather_api_utc_offset_s(void);

/* Wind direction as a single cardinal letter, "N" / "E" / "S" / "W". */
const char *weather_api_wind_dir_str(int deg);

/* English description of a WMO weather code. Never NULL. */
const char *weather_api_code_str(int weather_code);

/* The same in 10 characters, for a row that also carries the temperature.
 * Intensity becomes a '-' / '+' suffix. Never NULL. */
const char *weather_api_code_short(int weather_code);
