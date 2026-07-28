#include "screen_16x2.h"

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
#include "settings.h"
#include "timesync.h"
#include "weather_api.h"
#include "wifi.h"

/* 60 fps, paced by esp_timer: the FreeRTOS tick is 10 ms, too coarse for a
 * 16.7 ms frame. The rate is what the panel is polled at, not what it is
 * written at — readings change every few seconds and the transport drops
 * frames that would repaint the same characters. */
#define FPS 60
#define FRAME_US (1000000 / FPS)

/* Rendering writes into a generous buffer and lets the transport cut the line
 * to the panel width: the decoded weather description is longer than the
 * display, and truncating it here would only duplicate what
 * lcd1602_rgb_set_line() already does. */
#define RENDER_BUF 64

/* Backlight default: a cyan-ish tint at full brightness. */
#define DEFAULT_RGB 0x00AAFF

/* "\xDF" is the degree sign in the panel's character ROM — the UTF-8 one
 * would take two bytes and show up as garbage. */
#define DEG "\xDF"

/* Both survive reboots, like the LED brightness. */
#define SETTING_PAGE "screen_page"
#define SETTING_RGB  "bl_rgb"

typedef enum {
    PAGE_INDOOR,  /* what the station measures, plus the outdoor headline */
    PAGE_PRECISE, /* the same air, read off the dedicated sensors */
    PAGE_OUTDOOR, /* the rest of the Open-Meteo reading */
    PAGE_SYSTEM,  /* clock and how the station itself is doing */
    PAGE_COUNT,
} page_t;

static const char *TAG = "screen";

static volatile int s_page; /* advanced from the button task */
static int s_stored_page;   /* what NVS already holds, screen task only */
static TaskHandle_t s_task;

/* Written from the web handler, read by the screen task. */
static volatile uint32_t s_rgb = DEFAULT_RGB;

/* Air inside on the top row, outside on the bottom:
 *   23.46° 45.3 1250
 *   12.3° Overcast          (description cut to the space left)
 * Sixteen characters take the reading resolution the rest of the firmware
 * uses but not the "%" after the humidity — of the two markers on this row
 * the degree sign is the one worth keeping, since the row below carries a
 * temperature too. */
static void render_indoor(char *l0, char *l1)
{
    climate_t c;
    climate_get(&c);

    char temp[12] = "--.--" DEG;
    char rh[12] = " --%";
    char co2[12] = "----";
    if (c.temp_ok) {
        snprintf(temp, sizeof(temp), "%.2f" DEG, c.temp_c);
    }
    if (c.rh_ok) {
        snprintf(rh, sizeof(rh), "%3.0f%%", c.rh_pct);
    }
    if (c.co2_ok) {
        snprintf(co2, sizeof(co2), "%4u", c.co2_ppm);
    }
    snprintf(l0, RENDER_BUF, "%s %s %s", temp, rh, co2);

    weather_api_data_t w;
    if (weather_api_get(&w)) {
        snprintf(l1, RENDER_BUF, "%.1f" DEG " %s",
                 w.temp_c, weather_api_code_str(w.weather_code));
    } else {
        snprintf(l1, RENDER_BUF, "--.-" DEG " ---");
    }
}

/* Every reading at the resolution the firmware standardised on, which is what
 * the headline page has no room for:
 *   23.46° 1013.250     (TMP117 °C, BMP581 hPa — 15 characters)
 *   1250 45.3% 999.9    (SCD40 CO₂ and humidity, VEML7700 lx — 16 exactly)
 * That leaves five columns for the illuminance, so the tenth of a lux
 * survives only below 1000 lx; brighter than that the row drops to whole lux
 * and then to kilolux. Nowhere else does it have to give way.
 * Each value is filled in on its own, so one absent sensor leaves the others
 * readable instead of blanking the row. */
static void render_precise(char *l0, char *l1)
{
    climate_t c;
    climate_get(&c);

    char temp[12] = "--.--" DEG;
    char press[12] = "----.---";
    char co2[12] = "----";
    char rh[12] = "--.-";
    char lux[10] = "-----x";
    if (c.temp_ok) {
        snprintf(temp, sizeof(temp), "%5.2f" DEG, c.temp_c);
    }
    if (c.press_ok) {
        snprintf(press, sizeof(press), "%-8.3f", c.press_hpa);
    }
    if (c.co2_ok) {
        snprintf(co2, sizeof(co2), "%4u", c.co2_ppm);
    }
    if (c.rh_ok) {
        snprintf(rh, sizeof(rh), "%4.1f", c.rh_pct);
    }
    if (c.lux_ok) {
        if (c.lux < 999.95f) {
            snprintf(lux, sizeof(lux), "%5.1fx", c.lux);
        } else if (c.lux < 9999.5f) {
            snprintf(lux, sizeof(lux), "%5.0fx", c.lux);
        } else {
            snprintf(lux, sizeof(lux), "%4.0fkx", c.lux / 1000.0f);
        }
    }

    snprintf(l0, RENDER_BUF, "%s %s %s", temp, rh, co2);
    snprintf(l1, RENDER_BUF, "%s   %s", press, lux);
}

/* Meteorological degrees to an 8-point compass label — the direction the wind
 * blows from, which is what the API reports. Each point covers 45°, so the
 * boundaries sit at 22.5° and the +22 rounds the reading into its sector. */
static const char *wind_dir(int deg)
{
    static const char *const points[] = {
        "N", "NE", "E", "SE", "S", "SW", "W", "NW",
    };
    return points[((deg + 22) / 45) % 8];
}

/* Everything else Open-Meteo gives us, packed into 32 characters:
 *   12.3° 8..17°U3
 *   78% 1013 NW15 3   (humidity, hPa at sea level, wind from/km/h, WMO code) */
static void render_outdoor(char *l0, char *l1)
{
    weather_api_data_t w;
    if (!weather_api_get(&w)) {
        snprintf(l0, RENDER_BUF, "no weather data");
        l1[0] = '\0'; /* blank row */
        return;
    }
    snprintf(l0, RENDER_BUF, "%.1f" DEG " %.0f..%.0f" DEG " U%.0f",
             w.temp_c, w.temp_min_c, w.temp_max_c, w.uvi);
    snprintf(l1, RENDER_BUF, "%.0f%% %.0f %s%.0f %d",
             w.humidity_pct, w.pressure_msl_hpa, wind_dir(w.wind_dir_deg),
             w.wind_kmh, w.weather_code);
}

/* Share of the last second spent outside the idle task. The counters are
 * 32-bit microseconds; the ~71-minute wrap comes out right in the uint32_t
 * subtraction. The web handler keeps its own pair of them — the counters are
 * global, only the measurement window belongs to the caller. */
#define CPU_WINDOW_US 1000000

static int cpu_load_percent(void)
{
    static uint32_t prev_idle, prev_total;
    static int64_t next_us;
    static int load;

    /* Rendering runs at 60 fps and a 16 ms window is pure noise, so the
     * measurement keeps its own pace and the frames reuse the last figure. */
    int64_t now = esp_timer_get_time();
    if (now < next_us) {
        return load;
    }
    next_us = now + CPU_WINDOW_US;

    uint32_t idle = ulTaskGetIdleRunTimeCounter();
    uint32_t total = (uint32_t)now;
    uint32_t d_idle = idle - prev_idle;
    uint32_t d_total = total - prev_total;
    prev_idle = idle;
    prev_total = total;

    if (d_total != 0 && d_idle <= d_total) {
        load = 100 - (int)((uint64_t)100 * d_idle / d_total);
    }
    return load;
}

/* The station itself rather than the air around it:
 *   17:27:40 28.07     (the colons blink, once a second)
 *   12% 184k -58dBm    (CPU load, free heap, Wi-Fi signal) */
static void render_system(char *l0, char *l1)
{
    if (timesync_is_synced()) {
        /* The clock runs in UTC; the active location's offset (from
         * Open-Meteo) turns it into wall-clock time, and without one it stays
         * on UTC. */
        weather_api_data_t w;
        struct timeval tv;
        gettimeofday(&tv, NULL);
        time_t now = tv.tv_sec + (weather_api_get(&w) ? w.utc_offset_s : 0);
        struct tm tm;
        gmtime_r(&now, &tm);

        /* The blink is this row's seconds hand: colons for the first half of
         * every second, blanks for the second. The year does not fit next to
         * the seconds — sixteen characters go to the time and the day. */
        strftime(l0, RENDER_BUF,
                 tv.tv_usec < 500000 ? "%H:%M:%S %d.%m" : "%H %M %S %d.%m",
                 &tm);
    } else {
        snprintf(l0, RENDER_BUF, "clock not set");
    }

    /* An idle station is on Wi-Fi, so the link is worth a permanent slot:
     * signal when associated, "AP" while it serves its own network. */
    wifi_info_t wifi;
    wifi_get_info(&wifi);
    char link[12];
    if (wifi.sta_state == WIFI_STA_CONNECTED) {
        snprintf(link, sizeof(link), "%ddBm", wifi.rssi);
    } else {
        snprintf(link, sizeof(link), "%s", wifi.ap_active ? "AP" : "--");
    }

    snprintf(l1, RENDER_BUF, "%2.d%% %uk %s", cpu_load_percent(),
             (unsigned)(esp_get_free_heap_size() / 1024), link);
}

static void frame_tick(void *arg)
{
    xTaskNotifyGive(s_task); /* esp_timer task context, not an ISR */
}

static void screen_task(void *arg)
{
    for (;;) {
        /* Frames pile up only if a repaint overran the period; taking them
         * all at once drops the backlog instead of running behind. */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        char l0[RENDER_BUF] = "", l1[RENDER_BUF] = "";
        switch (s_page) {
        case PAGE_INDOOR:  render_indoor(l0, l1); break;
        case PAGE_PRECISE: render_precise(l0, l1); break;
        case PAGE_OUTDOOR: render_outdoor(l0, l1); break;
        default:           render_system(l0, l1); break;
        }

        uint32_t rgb = screen_16x2_backlight_rgb();
        lcd1602_rgb_set_line(0, l0);
        lcd1602_rgb_set_line(1, l1);
        lcd1602_rgb_set_color(rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF);

        /* Persist here rather than in the button callback: that one runs in
         * the timer context, and an NVS write can take tens of ms. */
        int page = s_page;
        if (page != s_stored_page) {
            s_stored_page = page;
            settings_set_u8(SETTING_PAGE, page);
        }
    }
}

void screen_16x2_next_page(void)
{
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

void screen_16x2_init(void)
{
    uint8_t page = settings_get_u8(SETTING_PAGE, PAGE_INDOOR);
    s_page = page < PAGE_COUNT ? page : PAGE_INDOOR; /* page count may shrink */
    s_stored_page = s_page;

    s_rgb = settings_get_u32(SETTING_RGB, DEFAULT_RGB) & 0xFFFFFF;

    lcd1602_rgb_init(); /* absent display is not an error */
    xTaskCreate(screen_task, "screen", 3072, NULL, 2, &s_task);

    const esp_timer_create_args_t timer_args = {
        .callback = frame_tick,
        .name = "screen_frame",
    };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, FRAME_US));

    ESP_LOGI(TAG, "Screen task started, %d pages at %d fps", PAGE_COUNT, FPS);
}
