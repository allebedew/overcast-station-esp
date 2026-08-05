#include "weather_api.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "buzzer.h"
#include "cJSON.h"
#include "weather_store.h"
#include "wifi.h"

/* Open-Meteo (https://open-meteo.com), no API key. The default "best_match"
 * model is used; coordinates come from the active location in weather_store,
 * so the URL is rebuilt per fetch. */

/* Fields requested in the `current` block (comma-separated, URL-safe). */
#define WEATHER_API_CURRENT_FIELDS \
    "temperature_2m,relative_humidity_2m,apparent_temperature," \
    "surface_pressure,pressure_msl,uv_index,weather_code,cloud_cover," \
    "wind_speed_10m,wind_gusts_10m,wind_direction_10m,precipitation,is_day"

/* Daily aggregates; forecast_days=1 keeps only today, so index 0 is today. */
#define WEATHER_API_DAILY_FIELDS \
    "temperature_2m_max,temperature_2m_min,sunshine_duration,daylight_duration"

#define WEATHER_API_UPDATE_INTERVAL_MS (15 * 60 * 1000)
#define WEATHER_API_RETRY_INTERVAL_MS  (5 * 60 * 1000)  /* retry sooner after a failure */
#define WEATHER_API_FIRST_RETRY_MS     15000            /* first retry after a network failure */
#define WEATHER_API_MAX_AGE_MS         (60 * 60 * 1000) /* a reading older than this is dropped */
#define WEATHER_API_NO_NET_DELAY_MS    10000            /* wait for the link, then re-check */
#define WEATHER_API_HTTP_TIMEOUT_MS    10000
#define WEATHER_API_MAX_RESPONSE       2048             /* current-only reply is well under 1 KB */

static const char *TAG = "weather_api";

/* Written by the weather_api task, read by httpd. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static weather_api_data_t s_data;
static bool s_valid;
static int64_t s_updated_us;

/* Accumulates the response body across HTTP_EVENT_ON_DATA callbacks. */
typedef struct {
    char *buf;
    int len;
    int cap;
} resp_ctx_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        resp_ctx_t *ctx = evt->user_data;
        if (ctx->len + evt->data_len < ctx->cap) {
            memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
            ctx->len += evt->data_len;
        }
    }
    return ESP_OK;
}

/* `dflt` when missing or not a number — a model may omit a metric. */
static float jnum(const cJSON *obj, const char *name, float dflt)
{
    cJSON *it = cJSON_GetObjectItem(obj, name);
    return cJSON_IsNumber(it) ? (float)it->valuedouble : dflt;
}

/* First element of a numeric array; daily arrays hold one, forecast_days=1. */
static float jarr0(const cJSON *arr, float dflt)
{
    cJSON *it = cJSON_GetArrayItem(arr, 0);
    return cJSON_IsNumber(it) ? (float)it->valuedouble : dflt;
}

/* `precipitation` is an accumulation over the model's own step, which `interval`
 * reports: 900 s for the 15-minute models, 3600 s for the hourly ones. Rescaling
 * to mm/h is what makes the number comparable to the usual light/moderate/heavy
 * thresholds instead of being four times too small. */
static float precip_rate(const cJSON *cur)
{
    float mm = jnum(cur, "precipitation", 0);
    float interval_s = jnum(cur, "interval", 3600);
    return interval_s > 0 ? mm * 3600.0f / interval_s : 0;
}

static bool parse(const char *json, const weather_location_t *loc)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed");
        return false;
    }

    bool ok = false;
    cJSON *cur = cJSON_GetObjectItem(root, "current");
    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    cJSON *temp = cJSON_GetObjectItem(cur, "temperature_2m");
    cJSON *hum = cJSON_GetObjectItem(cur, "relative_humidity_2m");
    cJSON *press = cJSON_GetObjectItem(cur, "surface_pressure");
    cJSON *uvi = cJSON_GetObjectItem(cur, "uv_index");
    if (cJSON_IsNumber(temp) && cJSON_IsNumber(hum) &&
        cJSON_IsNumber(press) && cJSON_IsNumber(uvi)) {
        weather_api_data_t d = {
            .temp_c = temp->valuedouble,
            .humidity_pct = hum->valuedouble,
            .pressure_hpa = press->valuedouble,
            .uvi = uvi->valuedouble,
            /* the rest default where a model omits them */
            .feels_c = jnum(cur, "apparent_temperature", temp->valuedouble),
            .temp_max_c = jarr0(cJSON_GetObjectItem(daily, "temperature_2m_max"),
                                temp->valuedouble),
            .temp_min_c = jarr0(cJSON_GetObjectItem(daily, "temperature_2m_min"),
                                temp->valuedouble),
            /* whole-day totals, so both are forecasts until the day is over */
            .sunshine_s = (int32_t)lroundf(
                jarr0(cJSON_GetObjectItem(daily, "sunshine_duration"), -1)),
            .daylight_s = (int32_t)lroundf(
                jarr0(cJSON_GetObjectItem(daily, "daylight_duration"), -1)),
            .pressure_msl_hpa = jnum(cur, "pressure_msl", press->valuedouble),
            .wind_kmh = jnum(cur, "wind_speed_10m", 0),
            .gust_kmh = jnum(cur, "wind_gusts_10m", 0),
            .wind_dir_deg = (int)lroundf(jnum(cur, "wind_direction_10m", 0)),
            .precip_mmh = precip_rate(cur),
            .is_day = jnum(cur, "is_day", 1) != 0,
            .cloud_pct = (int)lroundf(jnum(cur, "cloud_cover", 0)),
            .weather_code = (int)lroundf(jnum(cur, "weather_code", -1)),
            .elevation_m = jnum(root, "elevation", 0),
            .utc_offset_s = (int)lroundf(jnum(root, "utc_offset_seconds", 0)),
        };
        taskENTER_CRITICAL(&s_lock);
        s_data = d;
        s_valid = true;
        s_updated_us = esp_timer_get_time();
        taskEXIT_CRITICAL(&s_lock);
        ok = true;
        buzzer_play(BUZZER_CLICK);   /* one tick per fetch that landed */
        /* Outlives the reading: the zone is a property of the place, so the
         * clock stays right through an outage and across a reboot. */
        weather_store_set_offset(loc->name, d.utc_offset_s);
        ESP_LOGI(TAG,
                 "updated: %.1f C (feels %.1f, %.1f..%.1f), %.0f%%, %.1f/%.1f hPa, "
                 "UVI %.2f, wind %.1f (gust %.1f) km/h @%d, clouds %d%%, "
                 "precip %.1f mm/h, sun %.1f/%.1f h, %s, %s",
                 d.temp_c, d.feels_c, d.temp_min_c, d.temp_max_c, d.humidity_pct,
                 d.pressure_hpa, d.pressure_msl_hpa, d.uvi, d.wind_kmh, d.gust_kmh,
                 d.wind_dir_deg, d.cloud_pct, d.precip_mmh,
                 d.sunshine_s / 3600.0f, d.daylight_s / 3600.0f,
                 d.is_day ? "day" : "night",
                 weather_api_code_str(d.weather_code));
    } else {
        ESP_LOGW(TAG, "unexpected JSON shape");
    }

    cJSON_Delete(root);
    return ok;
}

/* The two failures deserve different waits: a request that never reached the
 * API says nothing about the API. */
typedef enum {
    FETCH_OK,
    FETCH_TRANSIENT,  /* never got there: DNS, TLS, timeout, no route */
    FETCH_REJECTED,   /* the reply arrived and was unusable */
} fetch_result_t;

static fetch_result_t fetch(const weather_location_t *loc)
{
    static char buf[WEATHER_API_MAX_RESPONSE];
    resp_ctx_t ctx = { .buf = buf, .len = 0, .cap = sizeof(buf) };

    /* timezone=auto makes the daily min/max span the local calendar day and
     * fills utc_offset_seconds in the reply. */
    char url[512];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=" WEATHER_API_CURRENT_FIELDS
             "&daily=" WEATHER_API_DAILY_FIELDS
             "&timezone=auto&forecast_days=1",
             loc->lat, loc->lon);

    ESP_LOGI(TAG, "GET %s", url);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = WEATHER_API_HTTP_TIMEOUT_MS,
        .event_handler = http_event,
        .user_data = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return FETCH_TRANSIENT;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fetch failed: %s", esp_err_to_name(err));
        return FETCH_TRANSIENT;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "API returned HTTP %d", status);
        return FETCH_REJECTED;
    }
    /* http_event keeps len < cap, so this NUL is always in bounds */
    ctx.buf[ctx.len] = '\0';
    return parse(ctx.buf, loc) ? FETCH_OK : FETCH_REJECTED;
}

/* Milliseconds since the last successful fetch, UINT32_MAX if there was none. */
static uint32_t age_ms(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool valid = s_valid;
    int64_t updated = s_updated_us;
    taskEXIT_CRITICAL(&s_lock);
    return valid ? (uint32_t)((esp_timer_get_time() - updated) / 1000) : UINT32_MAX;
}

/* Drops the cached reading, so nothing stale is reported as current. */
static void invalidate(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_valid = false;
    taskEXIT_CRITICAL(&s_lock);
}

static TaskHandle_t s_task;

static void weather_api_task(void *arg)
{
    uint32_t retry_ms = WEATHER_API_FIRST_RETRY_MS;

    for (;;) {
        weather_location_t loc;
        bool have = weather_store_get_active_location(&loc);
        uint32_t wait_ms;
        if (!have || !wifi_is_connected()) {
            /* no location selected yet, or waiting for the link */
            wait_ms = WEATHER_API_NO_NET_DELAY_MS;
        } else {
            switch (fetch(&loc)) {
            case FETCH_OK:
                wait_ms = WEATHER_API_UPDATE_INTERVAL_MS;
                retry_ms = WEATHER_API_FIRST_RETRY_MS;
                break;
            case FETCH_TRANSIENT:
                /* A resolver that drops a query usually answers the next one;
                 * the doubling settles a real outage at the slow rate. */
                wait_ms = retry_ms;
                retry_ms = retry_ms < WEATHER_API_RETRY_INTERVAL_MS / 2
                               ? retry_ms * 2
                               : WEATHER_API_RETRY_INTERVAL_MS;
                break;
            default: /* FETCH_REJECTED — the API answered; a fast retry won't help */
                wait_ms = WEATHER_API_RETRY_INTERVAL_MS;
                retry_ms = WEATHER_API_FIRST_RETRY_MS;
                break;
            }
        }
        /* A reading outlives a failed fetch, carrying its age, but past
         * MAX_AGE it is yesterday's weather and better shown as nothing.
         * Outside the branch: why it was not refreshed — the API, or a link
         * that never came up — does not make it any less stale. */
        if (age_ms() > WEATHER_API_MAX_AGE_MS) {
            invalidate();
        }
        /* weather_api_refresh() cuts the wait short on a location change. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

void weather_api_init(void)
{
    /* 8 KiB stack: TLS handshake and JSON parse run in this task */
    xTaskCreate(weather_api_task, "weather_api", 8192, NULL, 2, &s_task);
}

void weather_api_refresh(void)
{
    /* Drop the cached reading so the old location isn't shown for the new. */
    invalidate();
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

bool weather_api_get(weather_api_data_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    bool valid = s_valid;
    *out = s_data;
    int64_t updated = s_updated_us;
    taskEXIT_CRITICAL(&s_lock);

    out->age_s = valid ? (int32_t)((esp_timer_get_time() - updated) / 1000000)
                       : -1;
    return valid;
}

int weather_api_utc_offset_s(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool valid = s_valid;
    int offset = s_data.utc_offset_s;
    taskEXIT_CRITICAL(&s_lock);
    if (valid) {
        return offset;
    }
    /* No reading yet, or the last one expired: what a past fetch stored is
     * still right, bar the hours around a DST switch. */
    weather_location_t loc;
    if (weather_store_get_active_location(&loc) &&
        loc.utc_offset_s != WEATHER_TZ_UNKNOWN) {
        return loc.utc_offset_s;
    }
    return 0;
}

const char *weather_api_code_str(int code)
{
    switch (code) {
    case 0:  return "Clear sky";
    case 1:  return "Mainly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: return "Fog";
    case 48: return "Rime fog";
    case 51: return "Light drizzle";
    case 53: return "Drizzle";
    case 55: return "Dense drizzle";
    case 56: return "Light freezing drizzle";
    case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: return "Light freezing rain";
    case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Slight showers";
    case 81: return "Rain showers";
    case 82: return "Violent showers";
    case 85: return "Light snow showers";
    case 86: return "Heavy snow showers";
    case 95: return "Thunderstorm";
    case 96: return "Thunderstorm, hail";
    case 99: return "Thunderstorm, heavy hail";
    default: return "Unknown";
    }
}

const char *weather_api_wind_dir_str(int deg)
{
    static const char *const PTS[] = { "N", "E", "S", "W" };
    int d = ((deg % 360) + 360) % 360;
    return PTS[((d + 45) / 90) % 4];
}

const char *weather_api_code_short(int code)
{
    switch (code) {
    case 0:  return "Clear sky";
    case 1:  return "Fair";
    case 2:  return "Cloudy";
    case 3:  return "Overcast";
    case 45: return "Fog";
    case 48: return "Rime fog";
    case 51: return "Drizzle-";
    case 53: return "Drizzle";
    case 55: return "Drizzle+";
    case 56: return "Frz drz-";
    case 57: return "Frz drz";
    case 61: return "Rain-";
    case 63: return "Rain";
    case 65: return "Rain+";
    case 66: return "Frz rain-";
    case 67: return "Frz rain";
    case 71: return "Snow-";
    case 73: return "Snow";
    case 75: return "Snow+";
    case 77: return "Snow grns";
    case 80: return "Showers-";
    case 81: return "Showers";
    case 82: return "Showers+";
    case 85: return "Snow shw-";
    case 86: return "Snow shw+";
    case 95: return "Storm";
    case 96: return "Storm hl";
    case 99: return "Storm hl+";
    default: return "Unknown";
    }
}
