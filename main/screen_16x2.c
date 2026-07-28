#include "screen_16x2.h"

#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lcd1602_rgb.h"
#include "sensors.h"
#include "settings.h"
#include "timesync.h"
#include "weather_api.h"

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
    PAGE_OUTDOOR, /* the rest of the Open-Meteo reading */
    PAGE_CLOCK,   /* date, time and the backlight color in hex */
    PAGE_COUNT,
} page_t;

static const char *TAG = "screen";

static volatile int s_page; /* advanced from the button task */
static int s_stored_page;   /* what NVS already holds, screen task only */
static TaskHandle_t s_task;

/* Written from the web handler, read by the screen task. */
static volatile uint32_t s_rgb = DEFAULT_RGB;

/* Air inside on the top row, outside on the bottom:
 *   23.4° 45% 1250
 *   12.3° Overcast          (description cut to the space left) */
static void render_indoor(char *l0, char *l1)
{
    scd40_data_t d;
    if (sensors_scd40_get(&d)) {
        snprintf(l0, RENDER_BUF, "%.1f" DEG " %.0f%% %u",
                 d.temp_c, d.rh_pct, d.co2_ppm);
    } else {
        snprintf(l0, RENDER_BUF, "no sensor data");
    }

    weather_api_data_t w;
    if (weather_api_get(&w)) {
        snprintf(l1, RENDER_BUF, "%.1f" DEG " %s",
                 w.temp_c, weather_api_code_str(w.weather_code));
    } else {
        snprintf(l1, RENDER_BUF, "no weather data");
    }
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

/* The clock plus the backlight color, so the value being tuned from the web
 * page is readable on the device itself:
 *   Tue 28 Jun
 *   15:17:19 00AAFF */
static void render_clock(char *l0, char *l1)
{
    char color[8];
    snprintf(color, sizeof(color), "%06X", (unsigned)screen_16x2_backlight_rgb());

    if (!timesync_is_synced()) {
        snprintf(l0, RENDER_BUF, "clock not set");
        snprintf(l1, RENDER_BUF, "%s", color);
        return;
    }

    /* The clock runs in UTC; the active location's offset (from Open-Meteo)
     * turns it into wall-clock time, and without one it stays on UTC. */
    weather_api_data_t w;
    time_t now = time(NULL) + (weather_api_get(&w) ? w.utc_offset_s : 0);
    struct tm tm;
    gmtime_r(&now, &tm);

    strftime(l0, RENDER_BUF, "%a %d %b", &tm);

    char clock[16];
    strftime(clock, sizeof(clock), "%H:%M:%S", &tm);
    snprintf(l1, RENDER_BUF, "%s %s", clock, color);
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
        case PAGE_OUTDOOR: render_outdoor(l0, l1); break;
        default:           render_clock(l0, l1); break;
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

    lcd1602_rgb_init(sensors_i2c_bus()); /* absent display is not an error */
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
