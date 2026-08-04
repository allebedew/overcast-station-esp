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

#include "lcd1602_rgb.h"
#include "climate.h"
#include "encoder.h"
#include "ld2450.h"
#include "zambretti.h"
#include "ota.h"
#include "settings.h"
#include "sun.h"
#include "sysinfo.h"
#include "timesync.h"
#include "weather_api.h"
#include "weather_store.h"

/* Polling rate, not write rate: the transport drops frames that would repaint
 * the same characters. */
#define FPS       10
#define FRAME_MS  (1000 / FPS)

/* Generous: the transport cuts each line to the panel width. */
#define RENDER_BUF 64

#define DEFAULT_RGB 0x00AAFF

/* Per detent, on a channel of 0-255: 33 detents cross the range, and both ends
 * are reachable exactly because the value clamps rather than wraps. */
#define BL_STEP 4

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

/* The panel lights for whoever is in front of it and starts going out the
 * moment the radar drops presence — the only hold left is the module's own 5 s.
 * Faster on than off: walking in should not be a wait, walking out should not
 * be a blink. */
#define BL_GATE_UP   16 /* 255 in 1.6 s at 10 fps */
#define BL_GATE_DOWN 4  /* and out in 6.4 s */

/* Panel character ROM codes; the UTF-8 degree sign would show up as garbage. */
#define DEG   "\xDF"
#define BLOCK '\xFF'

#define SETTING_PAGE "screen_page"
#define SETTING_RGB  "bl_rgb"

typedef enum {
    PAGE_INDOOR,    /* what the station measures, plus the outdoor headline */
    PAGE_PRECISE,   /* the same air at the sensors' full resolution */
    PAGE_RADAR,     /* who is in front of it, and where */
    PAGE_OUTDOOR,   /* the rest of the Open-Meteo reading */
    PAGE_ZAMBRETTI, /* what the station's own barometer expects */
    PAGE_SUN,       /* the day's length and where the sun is in it */
    PAGE_SYSTEM,    /* clock and how the station itself is doing */
    PAGE_COUNT,
} page_t;

static const char *TAG = "screen";

static volatile int s_page; /* advanced from the button task */
static int s_stored_page;   /* what NVS already holds, screen task only */

/* Screen task writes, button task reads: a press has nothing to advance while
 * an override is up. */
static volatile bool s_override_active;

/* The one screen the knob reaches instead of the pages: a long press enters it,
 * a long press leaves. Screen task only. */
static bool s_bl_edit;
static int  s_bl_chan; /* 0 R, 1 G, 2 B — which one the knob is turning */

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

/*   23.46° 16.8 1250    (TMP117 °C, dew point, SCD40 CO₂)
 *   1013.250  999.9x    (BMP581 hPa at sea level, VEML7700 lx — 16 exactly) */
static void render_precise(char *l0, char *l1)
{
    climate_t c;
    climate_get(&c);

    char temp[12], press[12], co2[12], dew[12];
    fmt_or(temp, sizeof(temp), c.temp_ok, "--.--" DEG, "%5.2f" DEG, c.temp_c);
    fmt_or(press, sizeof(press), c.press_ok, "----.---", "%-8.3f",
           c.press_msl_hpa);
    fmt_or(co2, sizeof(co2), c.co2_ok, "----", "%4.0f", c.co2_ppm);
    fmt_or(dew, sizeof(dew), c.rh_ok, "--.-", "%4.1f", c.dew_c);

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

    snprintf(l0, RENDER_BUF, "%s %s %s", temp, dew, co2);
    snprintf(l1, RENDER_BUF, "%s  %s", press, lux);
}

/*   Radar 2   1.85m  (targets tracked, metres to the nearest — sixteen exactly)
 *   X-0.42  -1.42m/s (that one right of the module, and its speed, negative
 *                     toward it; the depth follows from the two, so the row
 *                     spends its width on the speed instead — also sixteen) */
static void render_radar(char *l0, char *l1)
{
    ld2450_data_t r;
    if (!ld2450_get(&r)) {
        snprintf(l0, RENDER_BUF, "no radar data");
        l1[0] = '\0';
        return;
    }

    char near[12];
    fmt_or(near, sizeof(near), r.count > 0, "--.--", "%5.2f", r.nearest_m);
    snprintf(l0, RENDER_BUF, "Radar %u   %sm", r.count, near);

    if (r.count == 0) {
        /* The held state, not the count: this is what the backlight follows. */
        snprintf(l1, RENDER_BUF, "%s", r.presence ? "presence held" : "nobody");
        return;
    }

    const ld2450_target_t *n = &r.targets[0];
    for (int i = 1; i < r.count; i++) {
        if (r.targets[i].dist_m < n->dist_m) {
            n = &r.targets[i];
        }
    }
    snprintf(l1, RENDER_BUF, "X%+5.2f %+6.2fm/s", n->x_mm / 1000.0f,
             n->speed_cms / 100.0f);
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

/*   1013.2 hPa v2.1  (sea level, tendency over 3 h; the ROM has no arrows)
 *   Chgable, rain     (the Zambretti wording, shortened to the panel) */
static void render_zambretti(char *l0, char *l1)
{
    climate_t c;
    climate_get(&c);
    zambretti_t f;
    zambretti_get(&f);

    char press[12];
    fmt_or(press, sizeof(press), c.press_ok, "----.-", "%6.1f",
           c.press_msl_hpa);

    if (!f.ok) {
        snprintf(l0, RENDER_BUF, "%s hPa  --", press);
        snprintf(l1, RENDER_BUF, "no 3h trend yet");
        return;
    }

    float delta = f.delta_3h_hpa;
    char dir = f.trend > 0 ? '^' : f.trend < 0 ? 'v' : '=';
    snprintf(l0, RENDER_BUF, "%s hPa %c%.1f", press, dir,
             delta < 0 ? -delta : delta);
    snprintf(l1, RENDER_BUF, "%s", zambretti_code_short(f.code));
}

/*   Day 05:25 20:59  (sunrise and sunset in the location's local time)
 *   Sun+32.4° v13:45  (angle above the horizon, then the wait for the next
 *                      crossing — '^' rise, 'v' set; sixteen exactly) */
static void render_sun(char *l0, char *l1)
{
    sun_info_t s;
    double elev;
    if (!sun_get(0, &s) || !sun_elevation_deg(&elev)) {
        snprintf(l0, RENDER_BUF, "no sun data");
        l1[0] = '\0';
        return;
    }

    weather_location_t loc;
    bool tz_known = weather_store_get_active_location(&loc) &&
                    loc.utc_offset_s != WEATHER_TZ_UNKNOWN;

    if (s.state == SUN_RISES && tz_known) {
        struct tm rise, set;
        time_t r = s.rise + loc.utc_offset_s, t = s.set + loc.utc_offset_s;
        gmtime_r(&r, &rise);
        gmtime_r(&t, &set);
        snprintf(l0, RENDER_BUF, "Day %02d:%02d %02d:%02d", rise.tm_hour,
                 rise.tm_min, set.tm_hour, set.tm_min);
    } else if (s.state == SUN_RISES) {
        /* The angle and the countdown below need no zone; these two do. */
        snprintf(l0, RENDER_BUF, "Day --:-- --:--");
    } else {
        snprintf(l0, RENDER_BUF, "Day: no %s",
                 s.state == SUN_POLAR_DAY ? "sunset" : "sunrise");
    }

    int32_t in_s;
    bool is_rise;
    if (sun_next_event(&in_s, &is_rise)) {
        snprintf(l1, RENDER_BUF, "Sun%+5.1f" DEG " %c%2d:%02d", elev,
                 is_rise ? '^' : 'v', (int)(in_s / 3600), (int)(in_s / 60) % 60);
    } else {
        snprintf(l1, RENDER_BUF, "Sun%+5.1f" DEG, elev);
    }
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

/*   Backlight 00AAFF
 *   >R  0 G170 B255   ('>' marks the channel the knob turns; the panel itself
 *                      is the preview, repainted as it goes) */
static void render_backlight(char *l0, char *l1)
{
    uint32_t rgb = s_rgb & 0xFFFFFF;
    snprintf(l0, RENDER_BUF, "Backlight %06X", (unsigned)rgb);

    static const char CHAN[] = "RGB";
    char *p = l1;
    for (int i = 0; i < 3; i++) {
        p += sprintf(p, "%c%c%3u", i == s_bl_chan ? '>' : ' ', CHAN[i],
                     (unsigned)((rgb >> (8 * (2 - i))) & 0xFF));
    }
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

static int wrap(int v, int n)
{
    v %= n;
    return v < 0 ? v + n : v;
}

static uint32_t rgb_nudge(uint32_t rgb, int chan, int by)
{
    int shift = 8 * (2 - chan);
    int v = (int)((rgb >> shift) & 0xFF) + by * BL_STEP;
    v = v < 0 ? 0 : (v > 255 ? 255 : v);
    return (rgb & ~(0xFFu << shift)) | ((uint32_t)v << shift);
}

/* The knob, classic mode: turning browses the pages, a long press opens the
 * one screen that edits and another closes it, and inside it a click moves
 * between the channels. Drained every frame even when the input goes nowhere,
 * so nothing piles up. */
static void input_apply(void)
{
    encoder_input_t in;
    encoder_take(&in);

    /* Set by the previous frame; steering what the OTA page hides would
     * surprise whoever turned. */
    if (s_override_active) {
        return;
    }

    int turn = in.cw - in.ccw;
    if (s_bl_edit) {
        if (turn) {
            /* Live, persisted on the way out. */
            s_rgb = rgb_nudge(s_rgb, s_bl_chan, turn);
        }
        s_bl_chan = (s_bl_chan + in.click) % 3;
    } else if (turn) {
        s_page = wrap(s_page + turn, PAGE_COUNT);
    }

    /* An even number of long presses in one frame lands where it started. */
    if (in.long_press % 2) {
        s_bl_edit = !s_bl_edit;
        if (s_bl_edit) {
            s_bl_chan = 0;
        } else {
            /* One NVS write per visit, not one per detent. */
            screen_16x2_set_backlight(s_rgb);
        }
    }
}

/* The OTA page pre-empts everything, the backlight screen pre-empts the pages;
 * Wi-Fi state is reported by the LED. */
static void render_page(char *l0, char *l1)
{
    if (ota_is_active()) {
        s_override_active = true;
        render_ota(l0, l1);
        return;
    }
    s_override_active = false;

    if (s_bl_edit) {
        render_backlight(l0, l1);
        return;
    }

    switch (s_page) {
    case PAGE_INDOOR:   render_indoor(l0, l1); break;
    case PAGE_PRECISE:  render_precise(l0, l1); break;
    case PAGE_RADAR:    render_radar(l0, l1); break;
    case PAGE_OUTDOOR:  render_outdoor(l0, l1); break;
    case PAGE_ZAMBRETTI: render_zambretti(l0, l1); break;
    case PAGE_SUN:      render_sun(l0, l1); break;
    default:            render_system(l0, l1); break;
    }
}

/* Scaling a channel to zero would shift the hue, so a lit channel stays lit —
 * unless the whole backlight is off, which is a state of its own. */
static uint8_t scale8(uint8_t c, uint8_t k)
{
    uint8_t v = (uint16_t)c * k / 255;
    return (v == 0 && c != 0 && k != 0) ? 1 : v;
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

/* Presence gate, 0-255, faded rather than switched. A radar that is silent or
 * absent leaves the panel lit — the room is not empty just because the module
 * is. */
static uint8_t backlight_gate(void)
{
    static int g = 255;

    ld2450_data_t r;
    bool lit = !ld2450_get(&r) || r.presence;

    int target = lit ? 255 : 0;
    int slew = target > g ? BL_GATE_UP : BL_GATE_DOWN;
    int step = target - g;
    if (step > slew) {
        step = slew;
    } else if (step < -slew) {
        step = -slew;
    }
    g += step;
    return (uint8_t)g;
}

static void screen_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));

        input_apply();

        climate_t c;
        climate_get(&c);
        uint8_t k = backlight_ramp(backlight_target(c.lux, c.lux_ok));
        k = (uint8_t)((uint16_t)k * backlight_gate() / 255);
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
    if (s_override_active || s_bl_edit) {
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
