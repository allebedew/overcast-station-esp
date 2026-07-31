#include "screen_16x2.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "lcd1602_rgb.h"
#include "climate.h"
#include "ota.h"
#include "settings.h"
#include "sysinfo.h"
#include "timesync.h"
#include "weather_api.h"
#include "wifi.h"

/* Polling rate, not write rate: the transport drops frames that would repaint
 * the same characters. */
#define FPS       10
#define FRAME_MS  (1000 / FPS)

/* Generous: the transport cuts each line to the panel width. */
#define RENDER_BUF 64

#define DEFAULT_RGB 0x00AAFF

/* The stored colour is what a lit room gets; below BL_LUX_BRIGHT it is scaled
 * down logarithmically to BL_MIN at BL_LUX_DARK. */
#define BL_LUX_DARK   1.0f
#define BL_LUX_BRIGHT 70.0f
#define BL_MIN        10 /* never dark: an unreadable screen reads as a fault */

/* log10f(BL_LUX_DARK + 1) and the span up to log10f(BL_LUX_BRIGHT + 1),
 * precomputed — the C6 has no FPU. */
#define BL_LOG_DARK 0.30103f
#define BL_LOG_SPAN 1.70329f

#define BL_SLEW 4 /* scale units per frame */

/* Panel character ROM codes; the UTF-8 degree sign would show up as garbage. */
#define DEG   "\xDF"
#define BLOCK '\xFF'

#define SETTING_PAGE "screen_page"
#define SETTING_RGB  "bl_rgb"

typedef enum {
    PAGE_INDOOR,  /* what the station measures, plus the outdoor headline */
    PAGE_PRECISE, /* the same air at the sensors' full resolution */
    PAGE_OUTDOOR, /* the rest of the Open-Meteo reading */
    PAGE_SYSTEM,  /* clock and how the station itself is doing */
    PAGE_COUNT,
} page_t;

#define DOTS_PERIOD_US 500000
#define DOTS_MAX       3

static const char *TAG = "screen";

static volatile int s_page; /* advanced from the button task */
static int s_stored_page;   /* what NVS already holds, screen task only */

/* Screen task writes, button task reads: a press has nothing to advance while
 * an override is up. */
static volatile bool s_override_active;

static volatile uint32_t s_rgb = DEFAULT_RGB; /* written from the web handler */
static volatile uint8_t s_bl_scale = 255;     /* published for the API */

static void fmt_or(char *dst, size_t len, bool ok, const char *absent,
                   const char *fmt, double value)
{
    if (ok) {
        snprintf(dst, len, fmt, value);
    } else {
        snprintf(dst, len, "%s", absent);
    }
}

/*   23.46° 45% 1250
 *   12.3° Overcast     (10 chars for the description, one less below zero) */
static void render_indoor(char *l0, char *l1)
{
    climate_t c;
    climate_get(&c);

    char temp[12], rh[12], co2[12];
    fmt_or(temp, sizeof(temp), c.temp_ok, "--.--" DEG, "%.2f" DEG, c.temp_c);
    fmt_or(rh, sizeof(rh), c.rh_ok, "--%", "%2.0f%%", c.rh_pct);
    fmt_or(co2, sizeof(co2), c.co2_ok, "----", "%4.0f", c.co2_ppm);
    snprintf(l0, RENDER_BUF, "%s %s %s", temp, rh, co2);

    weather_api_data_t w;
    if (weather_api_get(&w)) {
        snprintf(l1, RENDER_BUF, "%.1f" DEG "  %s",
                 w.temp_c, weather_api_code_short(w.weather_code));
    } else {
        snprintf(l1, RENDER_BUF, "--.-" DEG "  ---");
    }
}

/*   23.46° 45.3 1250    (TMP117 °C, SCD40 humidity and CO₂)
 *   1013.250  999.9x    (BMP581 hPa at sea level, VEML7700 lx — 16 exactly) */
static void render_precise(char *l0, char *l1)
{
    climate_t c;
    climate_get(&c);

    char temp[12], press[12], co2[12], rh[12];
    fmt_or(temp, sizeof(temp), c.temp_ok, "--.--" DEG, "%5.2f" DEG, c.temp_c);
    fmt_or(press, sizeof(press), c.press_ok, "----.---", "%-8.3f",
           c.press_msl_hpa);
    fmt_or(co2, sizeof(co2), c.co2_ok, "----", "%4.0f", c.co2_ppm);
    fmt_or(rh, sizeof(rh), c.rh_ok, "--.-", "%4.1f", c.rh_pct);

    /* Five columns hold either a tenth of a lux or a five-digit reading. */
    char lux[10] = "-----x";
    if (c.lux_ok) {
        if (c.lux < 10) {
            snprintf(lux, sizeof(lux), "%5.1fx", c.lux);
        } else if (c.lux < 1000) {
            snprintf(lux, sizeof(lux), "%5.0fx", c.lux);
        } else {
            snprintf(lux, sizeof(lux), "%4.1fkx", c.lux / 1000.0f);
        }
    }

    snprintf(l0, RENDER_BUF, "%s %s %s", temp, rh, co2);
    snprintf(l1, RENDER_BUF, "%s  %s", press, lux);
}

/* Each point covers 45°; +22 rounds the reading into its sector. */
static const char *wind_dir(int deg)
{
    static const char *const points[] = {
        "N", "NE", "E", "SE", "S", "SW", "W", "NW",
    };
    return points[((deg + 22) / 45) % 8];
}

/*   12.3° 8..17 C3    (current, today's min/max, WMO code)
 *   78% 1013 NW15 U3  (humidity, hPa at sea level, wind from/km/h, UV) */
static void render_outdoor(char *l0, char *l1)
{
    weather_api_data_t w;
    if (!weather_api_get(&w)) {
        snprintf(l0, RENDER_BUF, "no weather data");
        l1[0] = '\0';
        return;
    }
    snprintf(l0, RENDER_BUF, "%.1f" DEG " %.0f..%.0f C%d",
             w.temp_c, w.temp_min_c, w.temp_max_c, w.weather_code);
    snprintf(l1, RENDER_BUF, "%.0f%% %.0f %s%.0f U%.0f",
             w.humidity_pct, w.pressure_msl_hpa, wind_dir(w.wind_dir_deg),
             w.wind_kmh, w.uvi);
}

/*   17:27:40 28.07    (the colons blink, one second on, one second off)
 *   12% 184k BL184     (CPU load, free heap, backlight scale) */
static void render_system(char *l0, char *l1)
{
    if (timesync_is_synced()) {
        /* Clock runs in UTC; Open-Meteo's offset makes it wall-clock time. */
        struct timeval tv;
        gettimeofday(&tv, NULL);
        time_t now = tv.tv_sec + weather_api_utc_offset_s();
        struct tm tm;
        gmtime_r(&now, &tm);

        /* Blink parity off the UTC second — offsets are whole minutes. */
        strftime(l0, RENDER_BUF,
                 tv.tv_sec % 2 == 0 ? "%H:%M:%S %d.%m" : "%H %M %S %d.%m",
                 &tm);
    } else {
        snprintf(l0, RENDER_BUF, "clock not set");
    }

    /* Worst case fits exactly: "100% 1024k BL255" is sixteen. */
    snprintf(l1, RENDER_BUF, "%2.d%% %uk BL%u", sysinfo_cpu_load_percent(),
             (unsigned)(esp_get_free_heap_size() / 1024), s_bl_scale);
}

/* One cell per 6.25 %: the ROM has no partial blocks and this transport
 * defines no custom characters. */
static void render_ota(char *l0, char *l1)
{
    int percent = ota_progress_percent();
    if (percent > 100) {
        percent = 100;
    }
    snprintf(l0, RENDER_BUF, "Updating... %3d%%", percent);

    int filled = percent * LCD1602_COLS / 100;
    for (int i = 0; i < LCD1602_COLS; i++) {
        l1[i] = i < filled ? BLOCK : ' ';
    }
    l1[LCD1602_COLS] = '\0';
}

static void render_wifi(char *l0, char *l1, wifi_sta_state_t state,
                        const char *ssid)
{
    int dots = (int)(esp_timer_get_time() / DOTS_PERIOD_US) % (DOTS_MAX + 1);
    const char *base = state == WIFI_STA_WAITING_RETRY ? "Wi-Fi retry"
                                                       : "Wi-Fi connect";
    snprintf(l0, RENDER_BUF, "%s%.*s", base, dots, "...");
    snprintf(l1, RENDER_BUF, "%s", ssid[0] ? ssid : "(no network)");
}

/* OTA and Wi-Fi pages pre-empt the selected one. Once the station connects, or
 * the round-robin gives up, the Wi-Fi page steps aside for good — a later drop
 * is reported by the LED. */
static void render_page(char *l0, char *l1)
{
    static bool settled;

    if (ota_is_active()) {
        s_override_active = true;
        render_ota(l0, l1);
        return;
    }

    if (!settled) {
        char ssid[33] = "";
        wifi_sta_state_t sta = wifi_sta_state(ssid, sizeof(ssid));
        if (sta == WIFI_STA_CONNECTING || sta == WIFI_STA_WAITING_RETRY) {
            s_override_active = true;
            render_wifi(l0, l1, sta, ssid);
            return;
        }
        settled = true;
    }

    s_override_active = false;
    switch (s_page) {
    case PAGE_INDOOR:  render_indoor(l0, l1); break;
    case PAGE_PRECISE: render_precise(l0, l1); break;
    case PAGE_OUTDOOR: render_outdoor(l0, l1); break;
    default:           render_system(l0, l1); break;
    }
}

/* Scaling a channel to zero would shift the hue, so a lit channel stays lit. */
static uint8_t scale8(uint8_t c, uint8_t k)
{
    uint8_t v = (uint16_t)c * k / 255;
    return (v == 0 && c != 0) ? 1 : v;
}

/* Target scale, 0-255. Without a reading the colour is left alone: a dimmed
 * screen is not the way to report a missing sensor. */
static uint8_t backlight_target(float lux, bool lux_ok)
{
    static float cached_lux;
    static uint8_t cached_k = 255;

    if (!lux_ok) {
        return 255;
    }
    if (lux == cached_lux) {
        return cached_k;
    }

    float t = (log10f(lux + 1.0f) - BL_LOG_DARK) / BL_LOG_SPAN;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);

    cached_lux = lux;
    cached_k = BL_MIN + (uint8_t)(t * (255 - BL_MIN));
    return cached_k;
}

/* Tracking the illuminance exactly would flicker on every passing shadow. */
static uint8_t backlight_ramp(uint8_t target)
{
    static int k = 255;

    int step = target - k;
    if (step > BL_SLEW) {
        step = BL_SLEW;
    } else if (step < -BL_SLEW) {
        step = -BL_SLEW;
    }
    k += step;
    return (uint8_t)k;
}

static void screen_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));

        climate_t c;
        climate_get(&c);
        uint8_t k = backlight_ramp(backlight_target(c.lux, c.lux_ok));
        s_bl_scale = k; /* before rendering: the system page prints it */

        char l0[RENDER_BUF] = "", l1[RENDER_BUF] = "";
        render_page(l0, l1);

        uint32_t rgb = s_rgb;
        lcd1602_rgb_set_line(0, l0);
        lcd1602_rgb_set_line(1, l1);
        lcd1602_rgb_set_color(scale8(rgb >> 16, k), scale8((rgb >> 8) & 0xFF, k),
                              scale8(rgb & 0xFF, k));

        /* Not in the button callback: it runs in the timer context and an NVS
         * write can take tens of ms. */
        int page = s_page;
        if (page != s_stored_page) {
            s_stored_page = page;
            settings_set_u8(SETTING_PAGE, page);
        }
    }
}

void screen_16x2_next_page(void)
{
    /* Changing the selection out of sight would surprise whoever pressed. */
    if (s_override_active) {
        return;
    }
    s_page = (s_page + 1) % PAGE_COUNT;
}

void screen_16x2_set_backlight(uint32_t rgb)
{
    s_rgb = rgb & 0xFFFFFF;
    settings_set_u32(SETTING_RGB, s_rgb);
}

uint32_t screen_16x2_backlight_rgb(void)
{
    return s_rgb;
}

uint8_t screen_16x2_backlight_scale(void)
{
    return s_bl_scale;
}

void screen_16x2_init(void)
{
    uint8_t page = settings_get_u8(SETTING_PAGE, PAGE_INDOOR);
    s_page = page < PAGE_COUNT ? page : PAGE_INDOOR; /* page count may shrink */
    s_stored_page = s_page;

    s_rgb = settings_get_u32(SETTING_RGB, DEFAULT_RGB) & 0xFFFFFF;

    lcd1602_rgb_init(); /* absent display is not an error */
    xTaskCreate(screen_task, "screen", 3072, NULL, 2, NULL);

    ESP_LOGI(TAG, "Screen task started, %d pages (+2 conditional) at %d fps",
             PAGE_COUNT, FPS);
}
