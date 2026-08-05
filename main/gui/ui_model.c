#include "ui_model.h"

#include <stdio.h>
#include <string.h>

#include "timesync.h"
#include "weather_store.h"
#include "wifi.h"

void ui_model_refresh(ui_model_t *out)
{
    memset(out, 0, sizeof(*out));

    out->now       = timesync_is_synced() ? time(NULL) : 0;
    out->utc_off_s = weather_api_utc_offset_s();

    climate_get(&out->climate);
    out->out_ok   = weather_api_get(&out->out);
    out->out_cond = weather_api_code_short(out->out.weather_code);

    weather_location_t loc;
    if (weather_store_get_active_location(&loc)) {
        snprintf(out->loc, sizeof(out->loc), "%s", loc.name);
    }

    wifi_info_t wifi;
    wifi_get_info(&wifi);
    out->link = wifi.sta_state == WIFI_STA_CONNECTED;
    out->rssi = wifi.rssi;
}
