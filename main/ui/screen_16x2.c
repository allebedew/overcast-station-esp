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

/* Backlight brightness follows the room: the stored colour is what the panel
 * shows in a lit room, and it is scaled down as the VEML7700 reading falls.
 * At BL_LUX_BRIGHT and above the colour goes out unscaled; at BL_LUX_DARK and
 * below it sits at BL_MIN. The curve between them is logarithmic, like the
 * perception of brightness and like the range the room spans. */
#define BL_LUX_DARK   1.0f
#define BL_LUX_BRIGHT 70.0f
#define BL_MIN        10 /* never dark: an unreadable screen reads as a fault */

/* log10f(BL_LUX_DARK + 1) and the span from there to log10f(BL_LUX_BRIGHT + 1),
 * precomputed — this runs per frame and the C6 has no FPU. */
#define BL_LOG_DARK 0.30103f
#define BL_LOG_SPAN 1.70329f

/* "\xDF" is the degree sign in the panel's character ROM — the UTF-8 one
 * would take two bytes and show up as garbage. "\xFF" is the solid block at the
 * end of the same table, the one fully-lit cell it offers. */
#define DEG   "\xDF"
#define BLOCK '\xFF'

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

/* Two pages outside that rotation. They pre-empt whatever is selected because
 * while either applies the display has nothing better to say — during the boot
 * connect there are no readings yet, and during an upload the station is
 * seconds away from rebooting into another firmware. Neither is reachable with
 * the button or stored as the selected page: they come and go with the
 * condition, and the selected page is what returns afterwards. */
typedef enum {
    OVERRIDE_NONE,
    OVERRIDE_OTA,  /* firmware upload in progress, with a progress bar */
    OVERRIDE_WIFI, /* the first connection attempt after a restart */
} override_t;

/* The trailing dots on the Wi-Fi page cycle at this rate, so a connect that
 * takes a while looks like it is still trying rather than stuck. */
#define DOTS_PERIOD_US 500000
#define DOTS_MAX       3

static const char *TAG = "screen";

static volatile int s_page; /* advanced from the button task */
static int s_stored_page;   /* what NVS already holds, screen task only */
static TaskHandle_t s_task;

/* Whether one of the two pages above is showing. Written by the screen task,
 * read by the button task to know that a press has nothing to advance. */
static volatile bool s_override_active;

/* Written from the web handler, read by the screen task. */
static volatile uint32_t s_rgb = DEFAULT_RGB;

/* The other way round: computed by the screen task, published for the API and
 * the system page. */
static volatile uint8_t s_bl_scale = 255;

/* A reading, or the dashes standing in for the sensor that did not produce
 * it. Both spellings of a field sit on one line this way, so a row's layout
 * can be read off the rendering function instead of a run of ifs above it.
 * Integer quantities pass through as doubles ("%4.0f" prints what "%4u"
 * would) — the display has no field where that costs anything. */
static void fmt_or(char *dst, size_t len, bool ok, const char *absent,
                   const char *fmt, double value)
{
    if (ok) {
        snprintf(dst, len, fmt, value);
    } else {
        snprintf(dst, len, "%s", absent);
    }
}

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

    char temp[12], rh[12], co2[12];
    fmt_or(temp, sizeof(temp), c.temp_ok, "--.--" DEG, "%.2f" DEG, c.temp_c);
    fmt_or(rh, sizeof(rh), c.rh_ok, " --%", "%3.0f%%", c.rh_pct);
    fmt_or(co2, sizeof(co2), c.co2_ok, "----", "%4.0f", c.co2_ppm);
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
 *   23.46° 1013.250     (TMP117 °C, BMP581 hPa at sea level — 15 characters)
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

    char temp[12], press[12], co2[12], rh[12];
    fmt_or(temp, sizeof(temp), c.temp_ok, "--.--" DEG, "%5.2f" DEG, c.temp_c);
    /* Reduced to sea level, like everywhere else the pressure is shown. The
     * eight columns still hold it: the reduction lands the reading back around
     * a sea-level 1013 hPa whatever the altitude, so it stays four digits. */
    fmt_or(press, sizeof(press), c.press_ok, "----.---", "%-8.3f",
           c.press_msl_hpa);
    fmt_or(co2, sizeof(co2), c.co2_ok, "----", "%4.0f", c.co2_ppm);
    fmt_or(rh, sizeof(rh), c.rh_ok, "--.-", "%4.1f", c.rh_pct);

    /* The illuminance keeps its own ladder: five columns cannot hold both a
     * tenth of a lux and a five-digit reading. */
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

/* The station itself rather than the air around it:
 *   17:27:40 28.07     (the colons blink, one second on, one second off)
 *   12% 184k BL184     (CPU load, free heap, backlight scale) */
static void render_system(char *l0, char *l1)
{
    if (timesync_is_synced()) {
        /* The clock runs in UTC; the active location's offset (from
         * Open-Meteo) turns it into wall-clock time, and without one it stays
         * on UTC. */
        struct timeval tv;
        gettimeofday(&tv, NULL);
        time_t now = tv.tv_sec + weather_api_utc_offset_s();
        struct tm tm;
        gmtime_r(&now, &tm);

        /* The blink is this row's pendulum: colons through one whole second,
         * blanks through the next. Parity is taken off the UTC second rather
         * than the local one — the offset can be a half-hour, never an odd
         * number of seconds, so both give the same swing. The year does not
         * fit next to the seconds — sixteen characters go to the time and the
         * day. */
        strftime(l0, RENDER_BUF,
                 tv.tv_sec % 2 == 0 ? "%H:%M:%S %d.%m" : "%H %M %S %d.%m",
                 &tm);
    } else {
        snprintf(l0, RENDER_BUF, "clock not set");
    }

    /* The backlight scale has no other readout — there is no setting behind
     * it and the web page shows the stored color, not the one on the panel.
     * The worst case still fits: "100% 1024k BL255" is sixteen exactly. */
    snprintf(l1, RENDER_BUF, "%2.d%% %uk BL%u", sysinfo_cpu_load_percent(),
             (unsigned)(esp_get_free_heap_size() / 1024), s_bl_scale);
}

static void render_page(int page, char *l0, char *l1)
{
    switch (page) {
    case PAGE_INDOOR:  render_indoor(l0, l1); break;
    case PAGE_PRECISE: render_precise(l0, l1); break;
    case PAGE_OUTDOOR: render_outdoor(l0, l1); break;
    default:           render_system(l0, l1); break;
    }
}

/* The upload, with the row below given over to the bar:
 *   OTA update   67%
 *   ██████████------
 * The bar is one cell per 6.25 %, filled with the ROM's solid block; there are
 * no partial blocks in it and this transport defines no custom characters, so
 * that is the resolution. The track is spelled out with '-' rather than left
 * blank so a stalled upload still shows how far it got. */
static void render_ota(char *l0, char *l1)
{
    int percent = ota_progress_percent();
    if (percent > 100) {
        percent = 100;
    }
    snprintf(l0, RENDER_BUF, "OTA update  %3d%%", percent);

    int filled = percent * LCD1602_COLS / 100;
    for (int i = 0; i < LCD1602_COLS; i++) {
        l1[i] = i < filled ? BLOCK : ' ';
    }
    l1[LCD1602_COLS] = '\0';
}

/* The connection attempt, with the network it is attempting on the row below:
 *   Wi-Fi connect..
 *   HomeNetwork
 * The SSID takes the whole row and the transport cuts it to the panel width —
 * a long name is still recognisable from its first sixteen characters. */
static void render_wifi(char *l0, char *l1, wifi_sta_state_t state,
                        const char *ssid)
{
    int dots = (int)(esp_timer_get_time() / DOTS_PERIOD_US) % (DOTS_MAX + 1);
    const char *base = state == WIFI_STA_WAITING_RETRY ? "Wi-Fi retry"
                                                       : "Wi-Fi connect";
    snprintf(l0, RENDER_BUF, "%s%.*s", base, dots, "...");
    snprintf(l1, RENDER_BUF, "%s", ssid[0] ? ssid : "(no network)");
}

/* Which of the two applies, if either. Once the station has connected, or the
 * round-robin has given up and left the access point up, the Wi-Fi page steps
 * aside for good: from then on the readings are the more useful thing to show
 * and a dropped link is reported by the LED, not by taking over the display. */
static override_t current_override(wifi_sta_state_t *state, char *ssid,
                                   size_t len)
{
    static bool settled; /* screen task only */

    if (ota_is_active()) {
        return OVERRIDE_OTA;
    }
    if (settled) {
        return OVERRIDE_NONE;
    }

    *state = wifi_sta_state(ssid, len);
    if (*state == WIFI_STA_CONNECTING || *state == WIFI_STA_WAITING_RETRY) {
        return OVERRIDE_WIFI;
    }
    settled = true;
    return OVERRIDE_NONE;
}

/* Scaling a channel to zero would drop it out of the mix and shift the hue, so
 * a channel that was lit stays lit. */
static uint8_t scale8(uint8_t c, uint8_t k)
{
    uint8_t v = (uint16_t)c * k / 255;
    return (v == 0 && c != 0) ? 1 : v;
}

/* Backlight scale for the current illuminance, 0-255. Without a reading the
 * colour is left alone: a dimmed screen is not the way to report a missing
 * sensor. The result is cached against the reading it came from — the VEML7700
 * produces at most ten a second and this is asked sixty times a second. */
static uint8_t backlight_scale(float lux, bool lux_ok)
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

        /* Ahead of the rendering: the system page prints this. The scale only
         * moves when the illuminance does, a few times a second at most, so
         * the colour written below is identical across most frames and the
         * transport drops it — same as it does for the text. */
        climate_t c;
        climate_get(&c);
        uint8_t k = backlight_scale(c.lux, c.lux_ok);
        s_bl_scale = k;

        char l0[RENDER_BUF] = "", l1[RENDER_BUF] = "";
        char ssid[33] = "";
        wifi_sta_state_t sta = WIFI_STA_IDLE;
        override_t override = current_override(&sta, ssid, sizeof(ssid));
        s_override_active = override != OVERRIDE_NONE;

        switch (override) {
        case OVERRIDE_OTA:  render_ota(l0, l1); break;
        case OVERRIDE_WIFI: render_wifi(l0, l1, sta, ssid); break;
        default:            render_page(s_page, l0, l1); break;
        }

        uint32_t rgb = s_rgb;
        lcd1602_rgb_set_line(0, l0);
        lcd1602_rgb_set_line(1, l1);
        lcd1602_rgb_set_color(scale8(rgb >> 16, k), scale8((rgb >> 8) & 0xFF, k),
                              scale8(rgb & 0xFF, k));

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
    /* The two conditional pages are not part of the rotation, so while one of
     * them holds the display a press has nothing to advance — changing the
     * selection out of sight would only surprise whoever pressed it once the
     * page came back. */
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
    xTaskCreate(screen_task, "screen", 3072, NULL, 2, &s_task);

    const esp_timer_create_args_t timer_args = {
        .callback = frame_tick,
        .name = "screen_frame",
    };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, FRAME_US));

    ESP_LOGI(TAG, "Screen task started, %d pages (+2 conditional) at %d fps",
             PAGE_COUNT, FPS);
}
