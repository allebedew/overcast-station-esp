#include "climate.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"

#include "sensors.h"
#include "settings.h"

static const char *TAG = "climate";

/* Site altitude in metres, cached from the settings. */
static int s_altitude_m;

static void apply_altitude(int32_t metres)
{
    s_altitude_m = (int)metres;
    ESP_LOGI(TAG, "site altitude set to %d m", s_altitude_m);
}

void climate_init(void)
{
    s_altitude_m = (int)settings_get(SETTING_ALTITUDE_M);
    settings_on_change(SETTING_ALTITUDE_M, apply_altitude);
    ESP_LOGI(TAG, "site altitude %d m", s_altitude_m);
}

#define SEA_LEVEL_STANDARD_HPA 1013.25f /* ISA standard atmosphere at 0 m */

/* The international barometric formula's altitude factor; exactly 1 at
 * altitude 0, so an unconfigured station changes nothing. */
static float pressure_ratio(int altitude_m)
{
    return powf(1.0f - altitude_m / 44330.0f, 5.255f);
}

/* What the barometer would read at sea level under a standard atmosphere. The
 * standard profile is the whole approximation — in hard frost a
 * temperature-corrected reduction is up to ~2 hPa higher — but the indoor
 * sensor is the wrong air column and the weather API would tie a local reading
 * to the network. */
float climate_to_sea_level(float press_hpa)
{
    return press_hpa / pressure_ratio(s_altitude_m);
}

/* The mirror direction: sea-level standard pressure carried back down to the
 * site. The SCD40's fallback when no BMP581 reading is available. */
float climate_standard_pressure_hpa(void)
{
    return SEA_LEVEL_STANDARD_HPA * pressure_ratio(s_altitude_m);
}

void climate_get(climate_t *out)
{
    memset(out, 0, sizeof(*out));

    tmp117_data_t tmp117;
    if (sensors_tmp117_get(&tmp117)) {
        out->temp_ok = true;
        out->temp_c = tmp117.temp_c;
    }

    /* One reading, so CO2, humidity and dew point appear and disappear
     * together; the SCD40's own temperature is deliberately unused. */
    scd40_data_t air;
    if (sensors_scd40_get(&air)) {
        out->co2_ok = true;
        out->co2_ppm = air.co2_ppm;
        out->rh_ok = true;
        out->rh_pct = air.rh_pct;
        out->dew_c = air.dew_c;
    }

    /* Same for the BMP581's temperature: the part is here for the pressure. */
    bmp581_data_t bmp581;
    if (sensors_bmp581_get(&bmp581)) {
        out->press_ok = true;
        out->press_hpa = bmp581.press_hpa;
        out->press_msl_hpa = climate_to_sea_level(bmp581.press_hpa);
    }

    if (out->temp_ok && out->rh_ok) {
        out->dew_spread_ok = true;
        out->dew_spread_c = out->temp_c - out->dew_c;
    }

    veml7700_data_t veml7700;
    if (sensors_veml7700_get(&veml7700)) {
        out->lux_ok = true;
        out->lux = veml7700.lux;
    }
}
