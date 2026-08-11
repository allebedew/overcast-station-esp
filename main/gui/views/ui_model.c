#include "ui_model.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "sun.h"
#include "timesync.h"
#include "weather_store.h"
#include "wifi.h"

void ui_model_refresh(ui_model_t *out, history_quantity_t chart_q,
                      chart_range_t chart_range, uint8_t loc_sel)
{
    memset(out, 0, sizeof(*out));

    out->now       = timesync_is_synced() ? time(NULL) : 0;
    out->utc_off_s = weather_api_utc_offset_s();

    climate_get(&out->climate);
    zambretti_get(&out->zb);
    out->out_ok       = weather_api_get(&out->out);
    out->out_cond     = weather_api_code_short(out->out.weather_code);
    out->out_fetching = weather_api_is_fetching();

    weather_location_t loc;
    if (weather_store_get(loc_sel, &loc)) {
        snprintf(out->loc, sizeof(out->loc), "%s", loc.name);
    }

    const chart_range_def_t *range = &CHART_RANGES[chart_range];
    out->chart_n = history_series(range->tier, chart_q, range->stride,
                                  out->chart, CHART_SERIES_MAX);

    out->sun.day_ok  = sun_get(0, &out->sun.day);
    out->sun.elev_ok = sun_elevation_deg(&out->sun.elev_deg);
    out->sun.next_ok = sun_next_event(&out->sun.next_s, &out->sun.next_is_rise);

    wifi_info_t wifi;
    wifi_get_info(&wifi);
    out->link = wifi.sta_state == WIFI_STA_CONNECTED  ? UI_LINK_UP
              : wifi.sta_state == WIFI_STA_CONNECTING ? UI_LINK_CONNECTING
                                                      : UI_LINK_DOWN;
    out->rssi = wifi.rssi;
    out->ap   = wifi.ap_active;

    out->anim_ms = (uint32_t)(esp_timer_get_time() / 1000);

    // Drawn once per boot: an animation that is a function of anim_ms alone would
    // otherwise replay the same picks in the same order after every restart.
    static uint32_t seed;
    if (seed == 0) {
        seed = esp_random() | 1u;
    }
    out->boot_seed = seed;
}
