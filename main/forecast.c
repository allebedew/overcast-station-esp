#include "forecast.h"

#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "climate.h"
#include "history.h"
#include "timesync.h"

/* Three hours of the 1 d tier, one point per minute. */
#define WINDOW_MIN   180
#define MIN_POINTS   (WINDOW_MIN * 3 / 5) /* enough of the window recorded */
#define MIN_SPAN_MIN 120                  /* and spread over enough of it */

/* Tendency thresholds over 3 h, hPa — the WMO gradation. */
#define TREND_SLOW  0.5f
#define TREND_FAST  1.5f
#define TREND_RAPID 3.5f

/* The 1 d ring only moves once a minute, and the display asks ten times a
 * second while its page is up. */
#define CACHE_US (15 * 1000000LL)

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static forecast_t s_cache;
static int64_t s_cache_us;

/* Zambretti: a pressure ladder per tendency, each step one of the 26 wordings.
 * Descending, so the index is the number of steps the reading falls short of. */
static const int16_t RISE_HPA[] = { 1025, 1016, 1009, 1003, 997, 992,
                                    986,  980,  973,  967,  961, 953 };
static const char RISE_CODE[] = "ABCFGIJLMQTYZ";

static const int16_t STEADY_HPA[] = { 1028, 1017, 1011, 1003, 996,
                                      991,  984,  978,  966 };
static const char STEADY_CODE[] = "ABEKNPSWXZ";

static const int16_t FALL_HPA[] = { 1045, 1032, 1020, 1014,
                                    1006, 1000, 993,  987 };
static const char FALL_CODE[] = "ABDHORUVX";

static const char *const TEXT[26] = {
    "Settled fine weather",
    "Fine weather",
    "Becoming fine",
    "Fine, becoming less settled",
    "Fine, possibly showers",
    "Fairly fine, improving",
    "Fairly fine, possibly showers early",
    "Fairly fine, showers later",
    "Showery early, improving",
    "Changeable, improving",
    "Fairly fine, showers likely",
    "Rather unsettled, clearing later",
    "Unsettled, probably improving",
    "Showery, bright intervals",
    "Showery, becoming unsettled",
    "Changeable, some rain",
    "Unsettled, short fine intervals",
    "Unsettled, rain later",
    "Unsettled, rain at times",
    "Very unsettled, finer at times",
    "Rain at times, worse later",
    "Rain at times, becoming very unsettled",
    "Rain at frequent intervals",
    "Very unsettled, rain",
    "Stormy, possibly improving",
    "Stormy, much rain",
};

static const char *const TEXT_SHORT[26] = {
    "Settled fine",   "Fine weather",    "Becoming fine",  "Fine, unsettled",
    "Fine, showers?", "Fair, improving", "Fair, showers",  "Fair, rain later",
    "Showers, better", "Chgable, better", "Fair, showers?", "Unsettl., clear",
    "Unsettl.,better", "Showers, bright", "Showers, worse", "Chgable, rain",
    "Unsettl., sunny", "Unsettl., rain",  "Rain at times",  "V.unsettled",
    "Rain, worse",     "Rain, unsettled", "Frequent rain",  "V.unsettl.,rain",
    "Stormy, better?", "Stormy, heavy",
};

static int8_t classify(float delta)
{
    float mag = delta < 0 ? -delta : delta;
    int8_t step = mag < TREND_SLOW    ? 0
                  : mag < TREND_FAST  ? 1
                  : mag < TREND_RAPID ? 2
                                      : 3;
    return delta < 0 ? -step : step;
}

/* Winter shifts the reading a step, the way the original instrument does.
 * A build constant: the only latitude the station knows belongs to a weather
 * location, which may be any city, and the forecast takes nothing from there.
 * Unsynced clock means no season rather than a guessed one. */
#define NORTHERN_HEMISPHERE 1

static bool is_winter(void)
{
    if (!timesync_is_synced()) {
        return false;
    }
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    int month = tm.tm_mon + 1;
    bool apr_to_sep = month >= 4 && month <= 9;
    return NORTHERN_HEMISPHERE ? !apr_to_sep : apr_to_sep;
}

static uint8_t zambretti(float msl_hpa, int8_t trend)
{
    const int16_t *ladder;
    const char *codes;
    int steps, season;

    if (trend > 0) {
        ladder = RISE_HPA;
        steps = sizeof(RISE_HPA) / sizeof(RISE_HPA[0]);
        codes = RISE_CODE;
        season = is_winter() ? 1 : 0;
    } else if (trend < 0) {
        ladder = FALL_HPA;
        steps = sizeof(FALL_HPA) / sizeof(FALL_HPA[0]);
        codes = FALL_CODE;
        season = is_winter() ? -1 : 0;
    } else {
        ladder = STEADY_HPA;
        steps = sizeof(STEADY_HPA) / sizeof(STEADY_HPA[0]);
        codes = STEADY_CODE;
        season = 0;
    }

    int idx = 0;
    while (idx < steps && msl_hpa < ladder[idx]) {
        idx++;
    }
    idx += season;
    if (idx < 0) {
        idx = 0;
    } else if (idx > steps) {
        idx = steps;
    }
    return codes[idx] - 'A';
}

/* Least squares over the window rather than the difference between its ends:
 * one outlier, or a slot missing where the sensor dropped out, would otherwise
 * be the whole answer. */
static bool fit_slope(float *slope_hpa_min)
{
    int count = history_count(HISTORY_1D);
    int first = count > WINDOW_MIN ? count - WINDOW_MIN : 0;

    int64_t sx = 0, sxx = 0;
    float sy = 0, sxy = 0;
    int n = 0, x_first = 0, x_last = 0;
    float origin = 0; /* pressures are ~1000 hPa: fit the offsets, not the
                       * absolute values, or float loses the slope in them */

    for (int i = first; i < count; i++) {
        history_point_t p;
        if (!history_get(HISTORY_1D, i, &p) || !(p.have & HISTORY_HAS_PRESS)) {
            continue;
        }
        float y = p.press_mhpa / 1000.0f;
        if (n == 0) {
            origin = y;
            x_first = i - first;
        }
        y -= origin;
        int x = i - first;
        sx += x;
        sxx += (int64_t)x * x;
        sy += y;
        sxy += x * y;
        x_last = x;
        n++;
    }

    if (n < MIN_POINTS || x_last - x_first < MIN_SPAN_MIN) {
        return false;
    }
    float denom = (float)(n * sxx - sx * sx);
    if (denom == 0.0f) {
        return false;
    }
    *slope_hpa_min = (n * sxy - sx * sy) / denom;
    return true;
}

static void compute(forecast_t *out)
{
    memset(out, 0, sizeof(*out));

    climate_t c;
    climate_get(&c);
    float slope;
    if (!c.press_ok || !fit_slope(&slope)) {
        return;
    }

    /* The reduction to sea level is a multiplication by a constant, so it
     * applies to the change as it does to the reading. */
    out->delta_3h_hpa = climate_to_sea_level(slope * WINDOW_MIN);
    out->trend = classify(out->delta_3h_hpa);
    out->code = zambretti(c.press_msl_hpa, out->trend);
    out->ok = true;
}

void forecast_get(forecast_t *out)
{
    int64_t now = esp_timer_get_time();

    taskENTER_CRITICAL(&s_lock);
    bool cached = s_cache_us != 0 && now - s_cache_us < CACHE_US;
    if (cached) {
        *out = s_cache;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (cached) {
        return;
    }

    compute(out);

    taskENTER_CRITICAL(&s_lock);
    s_cache = *out;
    s_cache_us = now;
    taskEXIT_CRITICAL(&s_lock);
}

const char *forecast_code_str(uint8_t code)
{
    return code < 26 ? TEXT[code] : "";
}

const char *forecast_code_short(uint8_t code)
{
    return code < 26 ? TEXT_SHORT[code] : "";
}
