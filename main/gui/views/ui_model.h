#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "alert_rules.h"
#include "chart.h"   /* the window the series is sampled over */
#include "climate.h"
#include "history.h"
#include "sun.h"
#include "weather_api.h"
#include "zambretti.h"

/* Station side of the link, as much of it as the screen distinguishes. The pause
 * between attempts is UI_LINK_DOWN: nothing is on the air, so nothing moves. */
typedef enum {
    UI_LINK_DOWN,
    UI_LINK_CONNECTING,   /* association attempt actually in progress */
    UI_LINK_UP,           /* associated and holding an IP */
} ui_link_t;

/* Everything a frame is allowed to read, collected once before it starts
 * drawing. Rendering never calls a module directly: a reading that changed
 * between the heading and the value below it would draw an inconsistent frame,
 * and a lock taken mid-render would hold the sensor task for the length of it.
 *
 * Each source's own struct is embedded whole rather than unpacked into fields —
 * a new quantity in climate.h reaches the screen without a copy step to keep in
 * sync. Grows as the screens do; sources are added here as they are first
 * drawn, not in advance.
 *
 * Everything is refreshed on every call. Once a source appears that costs real
 * time (the sun's series, the Zambretti fit), this is where a per-quantity rate
 * limit goes. */
typedef struct {
    time_t  now;         /* 0 until the clock is synced */
    int32_t utc_off_s;   /* of the active weather location */

    climate_t climate;   /* the room; each quantity carries its own _ok */

    /* Comfort-band state per quantity, signed severity from alert_rules.h; 0
     * both for a reading in its band and for one with no sensor behind it. Held
     * here rather than worked out while drawing: the hysteresis needs the
     * previous zone, and a frame is a pure function of the model. */
    int8_t alert[ALERT_Q_COUNT];

    zambretti_t zb;      /* the local forecast; ok is false for the first 3 h */

    weather_api_data_t out;      /* the fetched forecast */
    bool               out_ok;
    bool               out_fetching; /* a fetch is running; the age is animated instead */
    const char        *out_cond; /* its WMO code as words, never NULL */
    char               loc[33];  /* selected location's name; "" when none is set */

    /* The link, unpacked rather than embedded: wifi.h pulls in esp_err.h, and
     * this header also compiles on the host for the simulator. */
    ui_link_t link;
    int       rssi;   /* dBm; meaningless unless UI_LINK_UP */
    bool      ap;     /* own SoftAP is up; the station side has nothing to show */

    /* The sun for the active location, from sun.c rather than the forecast:
     * today's crossings, where it stands right now, and how long until it next
     * crosses. Each part carries its own flag — all three are false without a
     * synced clock or an active location, and `next_ok` is also false through a
     * polar day or night, where the next crossing is weeks out. */
    struct {
        sun_info_t day;
        bool       day_ok;
        double     elev_deg;
        bool       elev_ok;
        int32_t    next_s;
        bool       next_is_rise;
        bool       next_ok;
    } sun;

    /* Monotonic since boot, for whatever moves on its own. Kept in the model so
     * a frame animates off one stamp rather than each widget reading a clock. */
    uint32_t anim_ms;

    /* Fixed for the run, random across boots: what a stateless animation mixes
     * into its picks so they do not repeat the same order after a restart. */
    uint32_t boot_seed;

    /* The chart's series, sampled here rather than under the screen so that
     * rendering keeps reading the model and nothing else. Oldest first, NAN for
     * a gap; chart_n is short of the window until the ring has filled it. */
    float chart[CHART_SERIES_MAX];
    int   chart_n;
} ui_model_t;

/* Which series to sample comes from the caller: the selection belongs to the
 * screen that draws the chart, and the model does not know about screens. So
 * does the location, and by index rather than by whichever is active — the knob
 * shows the name it stands on for the seconds before the pick is applied. */
void ui_model_refresh(ui_model_t *out, history_quantity_t chart_q,
                      chart_range_t chart_range, uint8_t loc_sel);
