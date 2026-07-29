#include "climate.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"

#include "sensors.h"
#include "settings.h"

#define NVS_KEY_ALTITUDE "altitude_m"

static const char *TAG = "climate";

/* Site altitude in metres, cached from NVS. */
static int s_altitude_m;

void climate_init(void)
{
    s_altitude_m = (int)settings_get_i32(NVS_KEY_ALTITUDE, 0);
    ESP_LOGI(TAG, "site altitude %d m", s_altitude_m);
}

int climate_altitude_m(void)
{
    return s_altitude_m;
}

void climate_set_altitude_m(int metres)
{
    if (metres < CLIMATE_ALTITUDE_MIN) {
        metres = CLIMATE_ALTITUDE_MIN;
    } else if (metres > CLIMATE_ALTITUDE_MAX) {
        metres = CLIMATE_ALTITUDE_MAX;
    }
    if (metres == s_altitude_m) {
        return;
    }
    s_altitude_m = metres;
    settings_set_i32(NVS_KEY_ALTITUDE, metres);
    ESP_LOGI(TAG, "site altitude set to %d m", metres);
}

#define SEA_LEVEL_STANDARD_HPA 1013.25f /* ISA standard atmosphere at 0 m */

/* The international barometric formula's altitude factor: at altitude 0 it is
 * exactly 1, so an unconfigured station leaves both directions below
 * unchanged rather than reporting something subtly wrong. */
static float pressure_ratio(int altitude_m)
{
    return powf(1.0f - altitude_m / 44330.0f, 5.255f);
}

/* Reduction to sea level: what the barometer would read if it stood at sea
 * level, under a standard atmosphere. That assumption is the whole
 * approximation — the real air column between the station and sea level has
 * its own temperature, and in hard frost a temperature-corrected reduction
 * comes out up to about 2 hPa higher. Taking that temperature from the indoor
 * sensor would be worse than assuming the standard profile (the column is
 * outdoors), and taking it from the weather API would make a local reading
 * stop working when the network does. */
static float to_sea_level(float press_hpa, int altitude_m)
{
    return press_hpa / pressure_ratio(altitude_m);
}

/* The mirror direction: what a standard atmosphere reads at the site
 * altitude, i.e. the sea-level standard pressure carried back down. Used as
 * the SCD40's ambient-pressure fallback when no BMP581 reading is available. */
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

    /* CO2 and humidity share one reading, so they appear and disappear
     * together; the SCD40's own temperature is deliberately not used. */
    scd40_data_t air;
    if (sensors_scd40_get(&air)) {
        out->co2_ok = true;
        out->co2_ppm = air.co2_ppm;
        out->rh_ok = true;
        out->rh_pct = air.rh_pct;
    }

    /* Same for the BMP581's temperature: the part is here for the pressure. */
    bmp581_data_t bmp581;
    if (sensors_bmp581_get(&bmp581)) {
        out->press_ok = true;
        out->press_hpa = bmp581.press_hpa;
        out->press_msl_hpa = to_sea_level(bmp581.press_hpa, s_altitude_m);
    }

    veml7700_data_t veml7700;
    if (sensors_veml7700_get(&veml7700)) {
        out->lux_ok = true;
        out->lux = veml7700.lux;
    }
}
