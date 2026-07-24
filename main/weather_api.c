#include "weather_api.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "weather_store.h"
#include "wifi.h"

/* Outside weather from Open-Meteo (https://open-meteo.com). No API key
 * required; the default "best_match" picks the most accurate regional model
 * for the coordinates (ICON / ECMWF over Europe). We read the `current`
 * block: surface_pressure is the real pressure at the location's elevation,
 * pressure_msl is the same reduced to sea level; UV index, cloud cover,
 * wind (speed/gusts/direction), rain/snowfall and the WMO weather_code all
 * come in the same block. Coordinates come from the active location in
 * weather_store; the request URL is built per fetch. */

/* Fields requested in the `current` block (comma-separated, URL-safe). */
#define WEATHER_API_CURRENT_FIELDS \
    "temperature_2m,relative_humidity_2m,apparent_temperature," \
    "surface_pressure,pressure_msl,uv_index,weather_code,cloud_cover," \
    "wind_speed_10m,wind_gusts_10m,wind_direction_10m,precipitation"

/* Daily aggregates; forecast_days=1 keeps only today, so index 0 is today. */
#define WEATHER_API_DAILY_FIELDS "temperature_2m_max,temperature_2m_min"

#define WEATHER_API_UPDATE_INTERVAL_MS (60 * 60 * 1000) /* refresh once an hour */
#define WEATHER_API_RETRY_INTERVAL_MS  (5 * 60 * 1000)  /* retry sooner after a failure */
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

/* Numeric field of `obj`, or `dflt` when it is missing or not a number
 * (Open-Meteo may omit a metric the selected model does not provide). */
static float jnum(const cJSON *obj, const char *name, float dflt)
{
    cJSON *it = cJSON_GetObjectItem(obj, name);
    return cJSON_IsNumber(it) ? (float)it->valuedouble : dflt;
}

/* First element of a numeric array, or `dflt` (daily arrays hold one entry
 * here because we request forecast_days=1). */
static float jarr0(const cJSON *arr, float dflt)
{
    cJSON *it = cJSON_GetArrayItem(arr, 0);
    return cJSON_IsNumber(it) ? (float)it->valuedouble : dflt;
}

static bool parse(const char *json)
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
            /* the rest are read leniently, defaulting where a model omits them */
            .feels_c = jnum(cur, "apparent_temperature", temp->valuedouble),
            .temp_max_c = jarr0(cJSON_GetObjectItem(daily, "temperature_2m_max"),
                                temp->valuedouble),
            .temp_min_c = jarr0(cJSON_GetObjectItem(daily, "temperature_2m_min"),
                                temp->valuedouble),
            .pressure_msl_hpa = jnum(cur, "pressure_msl", press->valuedouble),
            .wind_kmh = jnum(cur, "wind_speed_10m", 0),
            .gust_kmh = jnum(cur, "wind_gusts_10m", 0),
            .wind_dir_deg = (int)lroundf(jnum(cur, "wind_direction_10m", 0)),
            .precip_mm = jnum(cur, "precipitation", 0),
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
        ESP_LOGI(TAG,
                 "updated: %.1f C (feels %.1f, %.1f..%.1f), %.0f%%, %.0f/%.0f hPa, "
                 "UVI %.1f, wind %.0f (gust %.0f) km/h @%d, clouds %d%%, "
                 "precip %.1f mm, %s",
                 d.temp_c, d.feels_c, d.temp_min_c, d.temp_max_c, d.humidity_pct,
                 d.pressure_hpa, d.pressure_msl_hpa, d.uvi, d.wind_kmh, d.gust_kmh,
                 d.wind_dir_deg, d.cloud_pct, d.precip_mm,
                 weather_api_code_str(d.weather_code));
    } else {
        ESP_LOGW(TAG, "unexpected JSON shape");
    }

    cJSON_Delete(root);
    return ok;
}

static bool fetch(const weather_location_t *loc)
{
    static char buf[WEATHER_API_MAX_RESPONSE];
    resp_ctx_t ctx = { .buf = buf, .len = 0, .cap = sizeof(buf) };

    /* timezone=auto makes the daily min/max span the local calendar day and
     * fills utc_offset_seconds in the reply. */
    char url[384];
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
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fetch failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "API returned HTTP %d", status);
        return false;
    }
    /* http_event keeps len < cap, so this NUL is always in bounds */
    ctx.buf[ctx.len] = '\0';
    return parse(ctx.buf);
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
    for (;;) {
        weather_location_t loc;
        bool have = weather_store_get_active_location(&loc);
        uint32_t wait_ms;
        if (!have || wifi_get_status() != WIFI_STATUS_CONNECTED) {
            /* no location selected yet, or waiting for the link */
            wait_ms = WEATHER_API_NO_NET_DELAY_MS;
        } else if (fetch(&loc)) {
            wait_ms = WEATHER_API_UPDATE_INTERVAL_MS;
        } else {
            /* A failed fetch expires the reading rather than keeping the old
             * one on screen: better no weather than yesterday's weather. */
            invalidate();
            wait_ms = WEATHER_API_RETRY_INTERVAL_MS;
        }
        /* A notification from weather_api_refresh() cuts the wait short so a
         * location change is picked up right away. */
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
    /* Drop the cached reading so the old location isn't shown for the new
     * one, then wake the task to fetch immediately. */
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

const char *weather_api_code_str(int code)
{
    /* WMO weather interpretation codes (WW) as documented by Open-Meteo. */
    switch (code) {
    case 0:  return "Clear sky";
    case 1:  return "Mainly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: return "Fog";
    case 48: return "Depositing rime fog";
    case 51: return "Light drizzle";
    case 53: return "Moderate drizzle";
    case 55: return "Dense drizzle";
    case 56: return "Light freezing drizzle";
    case 57: return "Dense freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Moderate rain";
    case 65: return "Heavy rain";
    case 66: return "Light freezing rain";
    case 67: return "Heavy freezing rain";
    case 71: return "Slight snowfall";
    case 73: return "Moderate snowfall";
    case 75: return "Heavy snowfall";
    case 77: return "Snow grains";
    case 80: return "Slight rain showers";
    case 81: return "Moderate rain showers";
    case 82: return "Violent rain showers";
    case 85: return "Slight snow showers";
    case 86: return "Heavy snow showers";
    case 95: return "Thunderstorm";
    case 96: return "Thunderstorm with slight hail";
    case 99: return "Thunderstorm with heavy hail";
    default: return "Unknown";
    }
}
